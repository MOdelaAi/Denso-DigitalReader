// The live 1–4 camera grid. Reads up to the first four cameras, arranges a tile
// per camera (1 / 1×2 / 2×2 via grid_dims), and owns a CameraStream per tile.
// reload() rebuilds from the DB and starts streaming; release_streams() stops
// capture (e.g. so the Camera modal can grab the same USB device) without
// tearing down the tiles. Streaming stops on destruction.
#pragma once

#include "camera/camera.h"
#include "ui/camera/grid/warmup_gate.h"
#include "detection/engine_registry.h"

#include <QSqlDatabase>
#include <QString>
#include <QWidget>

#include <map>
#include <memory>
#include <string>
#include <vector>

class QGridLayout;

namespace denso::ui {

class CameraStream;
class CameraTile;
class BrazingReporter;
class ZoneReporter;
class WarmupState;

class CameraGrid : public QWidget {
    Q_OBJECT

public:
    explicit CameraGrid(QSqlDatabase db, std::shared_ptr<EngineRegistry> engines,
                        WarmupState* warmup, QWidget* parent = nullptr);
    ~CameraGrid() override;

    void reload();            // rebuild tiles + streams from the DB, then start
    void release_streams();   // stop capture, keep the tiles on screen
    void start_streams();     // (re)start the existing streams

protected:
    void resizeEvent(QResizeEvent* event) override;
    void paintEvent(QPaintEvent* event) override;  // black letterbox margins

private:
    void clear();             // stop + delete all streams and tiles
    void relayout_letterbox();  // centre the tile block as one 16:9-per-tile wall
    void start_one(const camera::Camera& cam, CameraTile* tile);  // build proc+stream, start
    void on_model_ready(const QString& filename);
    void on_warmup_finished();

    QSqlDatabase db_;
    QGridLayout* grid_ = nullptr;
    std::vector<CameraStream*> streams_;
    std::vector<CameraTile*> tiles_;
    std::shared_ptr<EngineRegistry> engines_;
    WarmupState* warmup_ = nullptr;   // per-model warm readiness (not owned)
    PendingStart pending_;            // detection cams waiting on their models
    // Data needed to build a pending camera's stream once its models are ready.
    struct PendingCam {
        camera::Camera cam;
        CameraTile* tile;
    };
    std::map<int64_t, PendingCam> pending_cams_;
    std::unique_ptr<BrazingReporter> brazing_reporter_;  // GUI-thread reliable sender
    std::unique_ptr<ZoneReporter> reporter_;             // shared ZoneSink (machine)
    int rows_ = 0;  // current grid dims (0 = empty); drives the letterbox aspect
    int cols_ = 0;
};

} // namespace denso::ui
