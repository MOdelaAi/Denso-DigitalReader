// The main content area's camera view. With no cameras it shows the "no cameras
// yet" empty state + an Add button; with one or more it shows the live 1–4
// streaming grid. reload() re-reads the camera list and switches/refreshes
// accordingly. release_streams() stops capture (e.g. while the Camera modal is
// open, so it can grab the same USB device); the next reload() restarts.
#pragma once

#include "ui/camera/shared/detection/engine_registry.h"

#include <QSqlDatabase>
#include <QWidget>

#include <memory>

class QStackedWidget;

namespace denso::ui {

class CameraGrid;

class CameraView : public QWidget {
    Q_OBJECT

public:
    explicit CameraView(QSqlDatabase db, std::shared_ptr<EngineRegistry> engines,
                        QWidget* parent = nullptr);

    /// Re-read the camera list, switch empty-state vs grid, and (re)start streams.
    void reload();

    /// Stop capture without tearing the view down (frees devices for the modal).
    void release_streams();

signals:
    void add_camera_requested();

private:
    QSqlDatabase db_;
    QStackedWidget* stack_ = nullptr;
    CameraGrid* grid_ = nullptr;
    std::shared_ptr<EngineRegistry> engines_;
};

} // namespace denso::ui
