// The live 1–4 camera grid. Reads up to the first four cameras, arranges a tile
// per camera (1 / 1×2 / 2×2 via grid_dims), and owns a CameraStream per tile.
// reload() rebuilds from the DB and starts streaming; release_streams() stops
// capture (e.g. so the Camera modal can grab the same USB device) without
// tearing down the tiles. Streaming stops on destruction.
#pragma once

#include "ui/camera/shared/detection/engine_registry.h"

#include <QSqlDatabase>
#include <QWidget>

#include <memory>
#include <vector>

class QGridLayout;

namespace denso::ui {

class CameraStream;
class CameraTile;

class CameraGrid : public QWidget {
    Q_OBJECT

public:
    explicit CameraGrid(QSqlDatabase db, std::shared_ptr<EngineRegistry> engines,
                        QWidget* parent = nullptr);
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

    QSqlDatabase db_;
    QGridLayout* grid_ = nullptr;
    std::vector<CameraStream*> streams_;
    std::vector<CameraTile*> tiles_;
    std::shared_ptr<EngineRegistry> engines_;
    int rows_ = 0;  // current grid dims (0 = empty); drives the letterbox aspect
    int cols_ = 0;
};

} // namespace denso::ui
