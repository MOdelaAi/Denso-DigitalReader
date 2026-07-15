#include "ui/camera/grid/camera_grid.h"

#include "brazing/config.h"
#include "camera/repo.h"
#include "detection/repo.h"
#include "brazing/brazing_client.h"
#include "brazing/brazing_reporter.h"
#include "camera/camera_stream.h"
#include "ui/camera/grid/camera_tile.h"
#include "camera/frame_processor.h"
#include "ui/camera/grid/grid_layout.h"
#include "brazing/zone_reporter.h"
#include "detection/engine_registry.h"
#include "ui/common/async_runner.h"  // post_to_gui
#include "ui/warmup_state.h"

#include <QCoreApplication>
#include <QColor>
#include <QDebug>
#include <QGridLayout>
#include <QPainter>
#include <QPaintEvent>
#include <QResizeEvent>
#include <QString>

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <memory>

namespace denso::ui {

namespace {
constexpr int kMaxTiles = 4;
constexpr double kTileAspect = 16.0 / 9.0;  // each cell is 16:9 (camera native)
}

CameraGrid::CameraGrid(QSqlDatabase db, std::shared_ptr<EngineRegistry> engines,
                       WarmupState* warmup, QWidget* parent)
    : QWidget(parent), db_(std::move(db)), engines_(std::move(engines)),
      warmup_(warmup) {
    grid_ = new QGridLayout(this);
    grid_->setContentsMargins(0, 0, 0, 0);
    grid_->setSpacing(0);  // flush tiles — no gap between feeds (CCTV wall)
    if (warmup_) {
        connect(warmup_, &WarmupState::model_ready, this, &CameraGrid::on_model_ready);
        connect(warmup_, &WarmupState::finished, this, &CameraGrid::on_warmup_finished);
    }
}

CameraGrid::~CameraGrid() { clear(); }

void CameraGrid::clear() {
    // Drop any deferred-start bookkeeping; the tiles it references are deleted
    // just below, so the dangling pointers must not outlive this call.
    pending_ = PendingStart{};
    pending_cams_.clear();
    // Stop (join) every worker before deleting anything so no frame can land on
    // a destroyed tile; Qt then drops any queued events for the deleted objects.
    for (CameraStream* s : streams_) {
        s->stop();
        delete s;
    }
    streams_.clear();
    for (CameraTile* t : tiles_) {
        grid_->removeWidget(t);
        delete t;
    }
    tiles_.clear();
    // Streams (and their DetectionProcessors that reference the reporter) are
    // stopped/joined above, so no capture thread can still call the reporter —
    // safe to tear it down, then the client it posts to.
    reporter_.reset();
    brazing_reporter_.reset();
    rows_ = 0;
    cols_ = 0;
    grid_->setContentsMargins(0, 0, 0, 0);
    // Reset stretch so a previous larger layout doesn't leave empty tracks.
    for (int i = 0; i < 2; ++i) {
        grid_->setRowStretch(i, 0);
        grid_->setColumnStretch(i, 0);
    }
}

void CameraGrid::reload() {
    clear();

    std::vector<camera::Camera> cams = camera::all(db_);
    if (cams.size() > static_cast<size_t>(kMaxTiles)) {
        cams.resize(kMaxTiles);  // first four by id
    }
    if (cams.empty()) {
        return;
    }

    // Brazing zone reporting: when enabled, a single machine-wide ZoneReporter
    // collects every camera's assembled zones and POSTs the combined snapshot on
    // change. The reporter is called from capture threads; its callback hops to
    // the GUI thread (post_to_gui) where the BrazingReporter lives.
    const brazing::BrazingConfig bcfg = brazing::load(db_);
    if (bcfg.enabled && !bcfg.base_url.empty()) {
        brazing_reporter_ = std::make_unique<BrazingReporter>(
            std::make_unique<BrazingClient>(bcfg.base_url));
        BrazingReporter* reporter = brazing_reporter_.get();
        reporter_ = std::make_unique<ZoneReporter>(
            [reporter](const std::map<int, int>& snap) {
                common::post_to_gui(reporter,
                                    [reporter, snap] { reporter->submit(snap); });
            });
    }

    const GridDims dims = grid_dims(static_cast<int>(cams.size()));
    for (int i = 0; i < static_cast<int>(cams.size()); ++i) {
        const camera::Camera& cam = cams[static_cast<size_t>(i)];

        auto* tile = new CameraTile(QString::fromStdString(cam.name));
        std::vector<camera::CameraArea> areas = camera::areas_for(db_, cam.id);
        tile->set_areas(areas);  // ROI overlay (if any)

        grid_->addWidget(tile, i / dims.cols, i % dims.cols);
        tiles_.push_back(tile);

        const detection::CameraDetection det = detection::detection_for(db_, cam.id);
        if (det.models.empty() || warmup_ == nullptr) {
            // No detection (or no warm-up coordinator): start immediately, exactly
            // as before. start_one handles the orientation/detection selection.
            start_one(cam, tile);
            continue;
        }
        // Which of this camera's models are not yet warm?
        std::vector<std::string> waiting;
        for (const detection::ResolvedModel& rm : det.models) {
            if (!warmup_->is_ready(rm.filename)) {
                waiting.push_back(rm.filename);
            }
        }
        if (waiting.empty()) {
            start_one(cam, tile);  // all models already warm → cache-hit get()
        } else {
            tile->set_preparing(true);
            pending_cams_[cam.id] = PendingCam{cam, tile};
            pending_.add(cam.id, std::move(waiting));
        }
    }
    for (int r = 0; r < dims.rows; ++r) grid_->setRowStretch(r, 1);
    for (int c = 0; c < dims.cols; ++c) grid_->setColumnStretch(c, 1);
    rows_ = dims.rows;
    cols_ = dims.cols;
    relayout_letterbox();
    // Streams are created + started per camera in start_one (now or as models
    // warm), so there is no batch start here.
}

void CameraGrid::start_one(const camera::Camera& cam, CameraTile* tile) {
    // ROI quarantine: after a view-significant source/geometry edit the camera's
    // areas may no longer align with the frame, so they are excluded from ROI
    // filtering and zone reporting is PAUSED until the operator re-verifies them
    // (Areas → "Verify & save" clears the flag). Feeding stale geometry could POST
    // wrong brazing zone numbers — so under review we pass NO areas and NO zone
    // sink (see below), and flag the tile.
    const bool review = cam.areas_need_review;
    std::vector<camera::CameraArea> areas =
        review ? std::vector<camera::CameraArea>{} : camera::areas_for(db_, cam.id);
    if (review) {
        qWarning().noquote() << "[camera] camera" << cam.id
                             << "— areas need review; zone reporting paused";
    }
    const detection::CameraDetection det = detection::detection_for(db_, cam.id);

    std::unique_ptr<FrameProcessor> proc;
    if (det.models.empty()) {
        proc = std::make_unique<OrientationProcessor>(
            static_cast<int>(cam.rotation), cam.pitch, cam.roll);
    } else {
        // Engine-construction firewall. engines_->get() builds the native TensorRT
        // engine on Linux, whose ctor THROWS on a missing/bad <engine>.names.json
        // sidecar or an invalid engine. start_one() runs on the GUI thread — often
        // from a Qt slot (on_model_ready / on_warmup_finished) — so an escaping
        // exception crosses the event loop and std::terminates the whole app
        // (this is exactly the field core-dump we saw). Fail loud but survivable:
        // log, show the tile Offline, and skip this camera — never a fake
        // orientation-only stream that would silently hide the missing detection.
        try {
            std::vector<DetectionProcessor::ModelRun> runs;
            for (const detection::ResolvedModel& rm : det.models) {
                InferenceEngine* eng = engines_->get(rm.filename);  // may throw (TRT)
                if (!eng) continue;  // model failed to load — skip it
                runs.push_back({eng, rm.class_names, rm.classes});
            }
            if (runs.empty()) {
                proc = std::make_unique<OrientationProcessor>(
                    static_cast<int>(cam.rotation), cam.pitch, cam.roll);
            } else {
                proc = std::make_unique<DetectionProcessor>(
                    static_cast<int>(cam.rotation), cam.pitch, cam.roll,
                    std::move(runs), std::move(areas), cam.id,
                    /*ReadingSink*/ nullptr,
                    /*ZoneSink*/ review ? nullptr : reporter_.get());
            }
        } catch (const std::exception& e) {
            qCritical().noquote()
                << "[camera] camera" << cam.id
                << "— detection model failed to load, camera not started:" << e.what();
            tile->set_preparing(false);
            tile->set_status(static_cast<int>(CameraStream::Status::Offline));
            return;
        }
    }
    tile->set_preparing(false);
    tile->set_review_paused(review);
    auto* stream = new CameraStream(cam, std::move(proc));
    connect(stream, &CameraStream::frame_ready, tile, &CameraTile::set_frame);
    connect(stream, &CameraStream::status_changed, tile, &CameraTile::set_status);
    tile->set_frame_counter(stream->frame_counter());
    streams_.push_back(stream);
    stream->start();
}

void CameraGrid::on_model_ready(const QString& filename) {
    const std::vector<int64_t> ids = pending_.ready(filename.toStdString());
    for (int64_t id : ids) {
        auto it = pending_cams_.find(id);
        if (it == pending_cams_.end()) continue;
        start_one(it->second.cam, it->second.tile);
        pending_cams_.erase(it);
    }
}

void CameraGrid::on_warmup_finished() {
    // Any camera still waiting has a model that never loaded → start with whatever
    // resolved (start_one falls back to OrientationProcessor when no model loads).
    for (int64_t id : pending_.drain()) {
        auto it = pending_cams_.find(id);
        if (it == pending_cams_.end()) continue;
        start_one(it->second.cam, it->second.tile);
        pending_cams_.erase(it);
    }
}

void CameraGrid::release_streams() {
    for (CameraStream* s : streams_) {
        s->stop();
    }
}

void CameraGrid::start_streams() {
    for (CameraStream* s : streams_) {
        s->start();
    }
}

void CameraGrid::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    relayout_letterbox();
}

void CameraGrid::paintEvent(QPaintEvent*) {
    // The tiles cover the centred block; this fills the letterbox margins around
    // it with black so the wall reads as one framed 16:9 surface.
    QPainter p(this);
    p.fillRect(rect(), QColor(0, 0, 0));
}

void CameraGrid::relayout_letterbox() {
    if (rows_ <= 0 || cols_ <= 0) {
        return;  // no cameras → nothing to centre
    }
    // Largest rect whose per-tile aspect is 16:9 that fits the widget, centred.
    // Block aspect = (cols·16) : (rows·9); solve for the block, then split the
    // leftover space into equal margins the grid layout reserves on each side.
    const double block_aspect = (cols_ * kTileAspect) / rows_;
    const double avail_w = width();
    const double avail_h = height();
    double block_w = avail_w;
    double block_h = avail_w / block_aspect;
    if (block_h > avail_h) {
        block_h = avail_h;
        block_w = avail_h * block_aspect;
    }
    const int mx = std::max(0, static_cast<int>(std::lround((avail_w - block_w) / 2.0)));
    const int my = std::max(0, static_cast<int>(std::lround((avail_h - block_h) / 2.0)));
    grid_->setContentsMargins(mx, my, mx, my);
}

} // namespace denso::ui
