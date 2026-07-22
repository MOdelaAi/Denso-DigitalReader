// The main content area's camera view. With no cameras it shows the "no cameras
// yet" empty state + an Add button; with one or more it shows the live 1–4
// streaming grid. reload() re-reads the camera list and switches/refreshes
// accordingly. release_streams() stops capture (e.g. while the Camera modal is
// open, so it can grab the same USB device); the next reload() restarts.
#pragma once

#include "detection/engine_registry.h"

#include <QSqlDatabase>
#include <QWidget>

#include <memory>

class QStackedWidget;

namespace denso::ui {

class CameraGrid;
class WarmupState;

class CameraView : public QWidget {
    Q_OBJECT

public:
    explicit CameraView(QSqlDatabase db, std::shared_ptr<EngineRegistry> engines,
                        WarmupState* warmup, QWidget* parent = nullptr);

    /// Re-read the camera list, switch empty-state vs grid, and (re)start streams.
    void reload();

    /// Stop capture without tearing the view down (frees devices for the modal).
    void release_streams();

    /// Pre-mode-switch teardown: tear the live grid down via CameraGrid's ONE
    /// authoritative primitive and park on the neutral, non-live page for the
    /// duration of the reset transaction. Deliberately does NOT call reload() or
    /// query camera::runtime() — nothing must restart the old-mode pipeline before
    /// the reset commits (spec §6.2). The rebuild happens later, after the switch.
    void teardown_for_switch();

    // Test-only observers of the view's non-live state after teardown_for_switch().
    int current_page_index() const;      ///< 0 = neutral/empty page, 1 = live grid
    bool grid_has_live_streams() const;  ///< whether the grid still holds a stream

signals:
    void add_camera_requested();

private:
    QSqlDatabase db_;
    QStackedWidget* stack_ = nullptr;
    CameraGrid* grid_ = nullptr;
    std::shared_ptr<EngineRegistry> engines_;
    WarmupState* warmup_ = nullptr;
};

} // namespace denso::ui
