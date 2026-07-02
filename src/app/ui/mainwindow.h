// The root window — the Qt port of `app-window.slint`'s AppWindow: a top button
// bar (Camera / Settings) over the main content area, plus the Settings and
// Camera modals. It also hosts the settings persistence handlers (resolution /
// theme / fullscreen / reset) that the Rust `wiring` module installed on the
// window, since those resize the window and restyle the app; the network
// handlers live in the SettingsDialog with the panel they drive.
#pragma once

#include "settings/settings.h"

#include <QMainWindow>
#include <QSqlDatabase>

#include <memory>

namespace denso::ui {

class SettingsDialog;
class CameraDialog;
class CameraView;
class EngineRegistry;

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    MainWindow(QSqlDatabase db, std::shared_ptr<settings::Settings> state,
               std::shared_ptr<EngineRegistry> engines, QWidget* parent = nullptr);

    /// Populate read-only fields (version, hardware), seed the persisted
    /// settings into the window + dialog, and apply the theme — the Qt port of
    /// `wiring::apply_startup`. Call before show().
    void apply_startup();

protected:
    void showEvent(QShowEvent* event) override;

private:
    void open_settings();
    void open_camera();

    /// Resize + centre the window so the whole frame fits the screen's work
    /// area. A preset as tall as the screen (1920×1080 on a 1080p monitor)
    /// would otherwise push the title bar + bottom rows under the taskbar.
    void resize_within_screen(int width, int height);

    void on_apply_resolution(int index);
    void on_theme_changed(bool dark);
    void on_toggle_fullscreen(bool fullscreen);
    void on_reset_defaults();

    /// Enter/leave fullscreen and persist the choice. F11 toggles it; Esc leaves
    /// it — convenience shortcuts so fullscreen isn't only reachable via Settings.
    void set_fullscreen(bool on);

    void apply_theme(bool dark);

    QSqlDatabase db_;
    std::shared_ptr<settings::Settings> state_;
    SettingsDialog* settings_ = nullptr;
    CameraDialog* camera_ = nullptr;
    CameraView* camera_view_ = nullptr;
    bool fitted_ = false;  // first-show re-fit has run
};

} // namespace denso::ui
