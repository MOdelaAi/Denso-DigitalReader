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
#include "health/status_file.h"
#include "paths/paths.h"
#include "ui/common/async_runner.h"  // post_to_gui
#include "ui/warmup_state.h"

#include <QCoreApplication>
#include <QColor>
#include <QDebug>
#include <QDir>
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
    tiles_by_cam_.clear();   // the tiles it pointed at were just deleted
    // Safe to tear down now: deleting each stream above destroyed the
    // FrameProcessor it owns, and ~DetectionProcessor joins its INFERENCE worker
    // — the thread that actually calls on_zones (capture threads never do). That
    // join, not the capture-thread stop, is what guarantees no worker can still
    // reach the reporter. Tear down the reporter, then the client it posts to.
    reporter_.reset();
    brazing_reporter_.reset();
    // health_ last: its callback references reporter_. No worker can fire it now
    // (workers joined above), and its destructor raises no cause, so this is safe.
    health_.reset();
    last_applied_seq_ = 0;
    verdict_ = health::IntegrityVerdict{};
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

    // runtime(), not all(): an unfinished camera must never stream, and the
    // filter happens in SQL so it cannot eat one of the four tile slots below.
    std::vector<camera::Camera> cams = camera::runtime(db_);
    if (cams.size() > static_cast<size_t>(kMaxTiles)) {
        cams.resize(kMaxTiles);  // first four by id
    }
    if (cams.empty()) {
        return;
    }

    // Boot readiness verdict, computed once per reload: drives the per-camera
    // ModelUnavailable cause below and status.json. Read-only (spec §2).
    verdict_ = health::evaluate_integrity(db_, denso::paths::models_dir());

    // Per-camera inhibit owner (GUI thread, no mutex). Any cause transition gates
    // the reporter, repaints the camera's tile, and rewrites status.json. Created
    // even without brazing so tiles + status.json still reflect faults.
    health_ = std::make_unique<health::ZoneHealth>(
        [this](int64_t camera_id, bool inhibited) {
            if (reporter_) reporter_->set_camera_inhibited(camera_id, inhibited);
            refresh_tile_inhibit(camera_id);
            refresh_status_file();
        });

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
            [this, reporter](const std::map<int, int>& snap, uint64_t seq) {
                // Marshal to `reporter` (not `this`) so Qt drops the queued call if
                // the BrazingReporter is torn down; `reporter` is owned by this
                // grid, so `this` (for last_applied_seq_) is valid whenever it runs.
                common::post_to_gui(reporter, [this, reporter, snap, seq] {
                    // Callbacks fire outside the reporter mutex and marshal from
                    // several threads, so an older eviction can overtake a newer
                    // recovery. Drop the stale one rather than let whole-snapshot
                    // latest-wins clobber the recovery (spec §3.3d).
                    if (seq <= last_applied_seq_) return;
                    last_applied_seq_ = seq;
                    reporter->submit(snap);
                });
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
        tiles_by_cam_[cam.id] = tile;

        // Install the static causes BEFORE the camera can publish (spec §8): the
        // tile is now in tiles_by_cam_, so the cause transition paints it. A
        // renumbered/quarantined ROI (areas_need_review) and a camera whose engine
        // is missing from disk (per-zone EngineMissing issue) both boot inhibited.
        health_->set_cause(cam.id, health::ZoneCause::AreasNeedReview,
                           cam.areas_need_review);
        for (const auto& iss : verdict_.issues) {
            if (iss.kind == health::ZoneIssue::Kind::EngineMissing &&
                iss.camera_id == cam.id) {
                health_->set_cause(cam.id, health::ZoneCause::ModelUnavailable, true);
            }
        }

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
    // Write status.json once now that every boot cause is installed, even if none
    // fired (a healthy machine still publishes a "ready" file for SSH inspection).
    refresh_status_file();
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
                    /*ZoneSink*/ review ? nullptr : reporter_.get(),
                    // Consecutive inference failures inhibit the camera. The handler
                    // fires on the INFERENCE WORKER THREAD, so marshal to the GUI
                    // before touching ZoneHealth (single-threaded by design).
                    /*WorkerFailedFn*/ [this, id = cam.id](int64_t, bool failed) {
                        common::post_to_gui(this, [this, id, failed] {
                            if (!health_) return;
                            health_->set_cause(
                                id, health::ZoneCause::InferenceWorkerFailed, failed);
                        });
                    });
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
    // The inhibit banner is driven by health_ causes (AreasNeedReview was set in
    // reload()), not set here — set_inhibited superseded set_review_paused.
    auto* stream = new CameraStream(cam, std::move(proc));
    connect(stream, &CameraStream::frame_ready, tile, &CameraTile::set_frame);
    connect(stream, &CameraStream::status_changed, tile, &CameraTile::set_status);
    // Camera-offline is a CAMERA-level cause: it drops the whole observation and
    // evicts the camera's zones immediately. Receiver is this grid (GUI thread) so
    // the AutoConnection is queued from the capture thread — the queued delivery is
    // what keeps ZoneHealth single-owner (mutex-free); a DirectConnection here
    // would break that invariant.
    connect(stream, &CameraStream::status_changed, this,
            [this, id = cam.id](int s) {
                if (!health_) return;
                health_->set_cause(
                    id, health::ZoneCause::CaptureOffline,
                    s == static_cast<int>(CameraStream::Status::Offline));
            });
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

void CameraGrid::refresh_tile_inhibit(int64_t camera_id) {
    const auto it = tiles_by_cam_.find(camera_id);
    if (it == tiles_by_cam_.end()) return;
    it->second->set_inhibited(health_ ? health_->causes(camera_id) : 0u);
}

void CameraGrid::refresh_status_file() {
    if (!health_) return;
    // Reuse the boot verdict (per-zone issues do not change at runtime); only the
    // runtime camera causes move. held/inhibited zone lists are not surfaced yet
    // (no producer this slice), so they stay empty.
    health::write_status_file(
        QDir(denso::paths::data_dir()).filePath(QStringLiteral("status.json")),
        verdict_, health_->all(), {}, {});
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
