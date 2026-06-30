#include "ui/mainwindow.h"

#include "hardware/collect.h"
#include "settings/repo.h"
#include "ui/camera/camera_dialog.h"
#include "ui/camera/camera_view.h"
#include "ui/settings/settings_dialog.h"
#include "ui/theme.h"

#include <QApplication>
#include <QHBoxLayout>
#include <QPushButton>
#include <QVBoxLayout>
#include <QWidget>

#ifndef APP_VERSION
#define APP_VERSION "0.0.0"
#endif

namespace denso::ui {

MainWindow::MainWindow(QSqlDatabase db, std::shared_ptr<settings::Settings> state,
                       QWidget* parent)
    : QMainWindow(parent), db_(std::move(db)), state_(std::move(state)) {
    setWindowTitle(QStringLiteral("Denso Digital Reader"));

    auto* central = new QWidget;
    auto* col = new QVBoxLayout(central);
    col->setContentsMargins(0, 0, 0, 0);
    col->setSpacing(0);

    // Top button bar (actions float to the right, as in app-window.slint).
    auto* top = new QWidget;
    top->setObjectName(QStringLiteral("topBar"));
    top->setFixedHeight(40);
    auto* bar = new QHBoxLayout(top);
    bar->setContentsMargins(4, 4, 4, 4);
    bar->addStretch(1);
    auto* camera_btn = new QPushButton(QStringLiteral("Camera"));
    connect(camera_btn, &QPushButton::clicked, this, &MainWindow::open_camera);
    auto* settings_btn = new QPushButton(QStringLiteral("Settings"));
    connect(settings_btn, &QPushButton::clicked, this, &MainWindow::open_settings);
    bar->addWidget(camera_btn, 0);
    bar->addWidget(settings_btn, 0);
    col->addWidget(top);

    // Main content area: the camera view (empty state / configured count).
    camera_view_ = new CameraView(db_);
    connect(camera_view_, &CameraView::add_camera_requested, this, &MainWindow::open_camera);
    col->addWidget(camera_view_, 1);

    setCentralWidget(central);

    // The Settings modal is created once and reused; it lives for the app's
    // lifetime, so the network worker threads always have a valid target.
    settings_ = new SettingsDialog(db_, this);
    settings_->setModal(true);
    connect(settings_, &SettingsDialog::apply_resolution_requested, this,
            &MainWindow::on_apply_resolution);
    connect(settings_, &SettingsDialog::theme_changed, this,
            &MainWindow::on_theme_changed);
    connect(settings_, &SettingsDialog::toggle_fullscreen_requested, this,
            &MainWindow::on_toggle_fullscreen);
    connect(settings_, &SettingsDialog::reset_defaults_requested, this,
            &MainWindow::on_reset_defaults);
}

void MainWindow::apply_startup() {
    settings_->set_app_version(QStringLiteral(APP_VERSION));

    const hardware::HardwareSpec hw = hardware::collect();
    settings_->set_hardware(QString::fromStdString(hw.os), QString::fromStdString(hw.device),
                            QString::fromStdString(hw.ram), QString::fromStdString(hw.storage));

    const settings::Settings& s = *state_;
    settings_->set_resolution_index(settings::preset_index(s.width, s.height));
    settings_->set_fullscreen(s.fullscreen);
    settings_->set_theme_dark(s.dark);

    resize(static_cast<int>(s.width), static_cast<int>(s.height));
    if (s.fullscreen) setWindowState(windowState() | Qt::WindowFullScreen);
    apply_theme(s.dark);
}

void MainWindow::open_settings() {
    // Re-seed from current state, reset to the first tab, then show modally.
    settings_->set_resolution_index(settings::preset_index(state_->width, state_->height));
    settings_->set_fullscreen(state_->fullscreen);
    settings_->set_theme_dark(state_->dark);
    settings_->show();
    settings_->raise();
    settings_->activateWindow();
}

void MainWindow::open_camera() {
    if (!camera_) {
        camera_ = new CameraDialog(db_, this);
        camera_->setModal(true);
        connect(camera_, &CameraDialog::cameras_changed, camera_view_, &CameraView::reload);
    }
    camera_->show();
    camera_->raise();
    camera_->activateWindow();
}

void MainWindow::on_apply_resolution(int index) {
    const auto [w, h] = settings::PRESETS[static_cast<size_t>(index)];
    state_->width = w;
    state_->height = h;
    settings::save(db_, *state_);
    resize(static_cast<int>(w), static_cast<int>(h));
}

void MainWindow::on_theme_changed(bool dark) {
    state_->dark = dark;
    settings::save(db_, *state_);
    apply_theme(dark);
}

void MainWindow::on_toggle_fullscreen(bool fullscreen) {
    state_->fullscreen = fullscreen;
    settings::save(db_, *state_);
    if (fullscreen)
        showFullScreen();
    else
        showNormal();
}

void MainWindow::on_reset_defaults() {
    const settings::Settings d;  // defaults
    settings::save(db_, d);

    if (d.fullscreen) {
        showFullScreen();
    } else {
        showNormal();
        resize(static_cast<int>(d.width), static_cast<int>(d.height));
    }
    settings_->set_resolution_index(settings::preset_index(d.width, d.height));
    settings_->set_fullscreen(d.fullscreen);
    settings_->set_theme_dark(d.dark);
    apply_theme(d.dark);

    *state_ = d;
}

void MainWindow::apply_theme(bool dark) {
    // Qualify: unqualified `palette` would resolve to the inherited
    // QWidget::palette() member, hiding the free theme function.
    qApp->setStyleSheet(style_sheet(denso::ui::palette(dark)));
}

} // namespace denso::ui
