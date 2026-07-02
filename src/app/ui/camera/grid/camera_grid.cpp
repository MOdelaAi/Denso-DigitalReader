#include "ui/camera/grid/camera_grid.h"

#include "camera/repo.h"
#include "detection/repo.h"
#include "ui/camera/grid/camera_stream.h"
#include "ui/camera/grid/camera_tile.h"
#include "ui/camera/grid/frame_processor.h"
#include "ui/camera/grid/grid_layout.h"
#include "ui/camera/shared/detection/engine_registry.h"

#include <QCoreApplication>
#include <QGridLayout>
#include <QString>

#include <algorithm>
#include <memory>

namespace denso::ui {

namespace {
constexpr int kMaxTiles = 4;
}

CameraGrid::CameraGrid(QSqlDatabase db, std::shared_ptr<EngineRegistry> engines,
                       QWidget* parent)
    : QWidget(parent), db_(std::move(db)), engines_(std::move(engines)) {
    grid_ = new QGridLayout(this);
    grid_->setContentsMargins(0, 0, 0, 0);
    grid_->setSpacing(12);
}

CameraGrid::~CameraGrid() { clear(); }

void CameraGrid::clear() {
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

    const GridDims dims = grid_dims(static_cast<int>(cams.size()));
    for (int i = 0; i < static_cast<int>(cams.size()); ++i) {
        const camera::Camera& cam = cams[static_cast<size_t>(i)];

        auto* tile = new CameraTile(QString::fromStdString(cam.name));
        std::vector<camera::CameraArea> areas = camera::areas_for(db_, cam.id);
        tile->set_areas(areas);  // ROI overlay (if any)

        const detection::CameraDetection det = detection::detection_for(db_, cam.id);
        std::unique_ptr<FrameProcessor> proc;
        if (det.models.empty()) {
            proc = std::make_unique<OrientationProcessor>(
                static_cast<int>(cam.rotation), cam.pitch, cam.roll);
        } else {
            std::vector<DetectionProcessor::ModelRun> runs;
            for (const detection::ResolvedModel& rm : det.models) {
                InferenceEngine* eng = engines_->get(rm.filename);
                if (!eng) continue;  // model failed to load — skip it
                runs.push_back({eng, rm.class_names, rm.classes});
            }
            if (runs.empty()) {
                proc = std::make_unique<OrientationProcessor>(
                    static_cast<int>(cam.rotation), cam.pitch, cam.roll);
            } else {
                proc = std::make_unique<DetectionProcessor>(
                    static_cast<int>(cam.rotation), cam.pitch, cam.roll,
                    std::move(runs), std::move(areas));
            }
        }
        auto* stream = new CameraStream(cam, std::move(proc));
        connect(stream, &CameraStream::frame_ready, tile, &CameraTile::set_frame);
        connect(stream, &CameraStream::status_changed, tile, &CameraTile::set_status);

        grid_->addWidget(tile, i / dims.cols, i % dims.cols);
        tiles_.push_back(tile);
        streams_.push_back(stream);
    }
    for (int r = 0; r < dims.rows; ++r) grid_->setRowStretch(r, 1);
    for (int c = 0; c < dims.cols; ++c) grid_->setColumnStretch(c, 1);

    start_streams();
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

} // namespace denso::ui
