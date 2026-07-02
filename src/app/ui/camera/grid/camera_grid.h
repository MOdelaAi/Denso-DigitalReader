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

private:
    void clear();             // stop + delete all streams and tiles

    QSqlDatabase db_;
    QGridLayout* grid_ = nullptr;
    std::vector<CameraStream*> streams_;
    std::vector<CameraTile*> tiles_;
    std::shared_ptr<EngineRegistry> engines_;
};

} // namespace denso::ui
