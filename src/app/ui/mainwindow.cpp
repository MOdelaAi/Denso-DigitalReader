#include "ui/mainwindow.h"

#include "hardware/collect.h"
#include "settings/repo.h"
#include "ui/camera/camera_dialog.h"
#include "ui/camera/camera_view.h"
#include "ui/settings/settings_dialog.h"
#include "ui/theme.h"

#include <QApplication>
#include <QHBoxLayout>
#include <QIcon>
#include <QKeySequence>
#include <QLabel>
#include <QPixmap>
#include <QPushButton>
#include <QScreen>
#include <QShortcut>
#include <QShowEvent>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>

#ifndef APP_VERSION
#define APP_VERSION "0.0.0"
#endif

namespace denso::ui {

MainWindow::MainWindow(QSqlDatabase db, std::shared_ptr<settings::Settings> state,
                       std::shared_ptr<EngineRegistry> engines, QWidget* parent)
    : QMainWindow(parent), db_(std::move(db)), state_(std::move(state)) {
    setWindowTitle(QStringLiteral("Denso Digital Reader"));
    setWindowIcon(QIcon(QStringLiteral(":/icon.png")));  // title bar + taskbar

    auto* central = new QWidget;
    auto* col = new QVBoxLayout(central);
    col->setContentsMargins(0, 0, 0, 0);
    col->setSpacing(0);

    // Top button bar (actions float to the right, as in app-window.slint).
    auto* top = new QWidget;
    top->setObjectName(QStringLiteral("topBar"));
    top->setFixedHeight(44);
    auto* bar = new QHBoxLayout(top);
    bar->setContentsMargins(12, 4, 8, 4);
    bar->setSpacing(8);
    // App logo + name on the left.
    auto* logo = new QLabel;
    logo->setPixmap(QPixmap(QStringLiteral(":/icon.png"))
                        .scaledToHeight(24, Qt::SmoothTransformation));
    bar->addWidget(logo, 0, Qt::AlignVCenter);
    auto* title = new QLabel(QStringLiteral("Denso Digital Reader"));
    title->setObjectName(QStringLiteral("appTitle"));
    bar->addWidget(title, 0, Qt::AlignVCenter);
    bar->addStretch(1);
    auto* camera_btn = new QPushButton(QStringLiteral("Camera"));
    connect(camera_btn, &QPushButton::clicked, this, &MainWindow::open_camera);
    auto* settings_btn = new QPushButton(QStringLiteral("Settings"));
    connect(settings_btn, &QPushButton::clicked, this, &MainWindow::open_settings);
    bar->addWidget(camera_btn, 0);
    bar->addWidget(settings_btn, 0);
    col->addWidget(top);

    // Fullscreen shortcuts: F11 toggles, Esc leaves. Needed because the top bar
    // (with the Settings button) is hidden while fullscreen — these keep a way
    // back to windowed mode without it.
    auto* fs = new QShortcut(QKeySequence(Qt::Key_F11), this);
    connect(fs, &QShortcut::activated, this, [this] { set_fullscreen(!isFullScreen()); });
    auto* esc = new QShortcut(QKeySequence(Qt::Key_Escape), this);
    connect(esc, &QShortcut::activated, this, [this] {
        if (isFullScreen()) set_fullscreen(false);
    });

    // Main content area: the camera view (empty state / configured count).
    camera_view_ = new CameraView(db_, engines);
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

    resize_within_screen(static_cast<int>(s.width), static_cast<int>(s.height));
    if (s.fullscreen) setWindowState(windowState() | Qt::WindowFullScreen);
    apply_theme(s.dark);
}

void MainWindow::showEvent(QShowEvent* event) {
    QMainWindow::showEvent(event);
    if (fitted_) return;
    fitted_ = true;
    // apply_startup() ran resize_within_screen() before the native window
    // existed, so the frame margins (title bar + borders) were zero and unknown.
    // Now they're real — re-fit once so the framed window truly fits the work
    // area rather than spilling its title bar / bottom edge under the taskbar.
    resize_within_screen(width(), height());
}

void MainWindow::resize_within_screen(int w, int h) {
    const QScreen* scr = screen();
    if (!scr) {
        resize(w, h);
        return;
    }
    const QRect avail = scr->availableGeometry();
    // Frame overhead is zero until the window is realized (first show); the
    // showEvent re-fit re-runs this once it's known.
    const int frame_w = frameGeometry().width() - width();
    const int frame_h = frameGeometry().height() - height();
    resize(std::min(w, avail.width() - frame_w),
           std::min(h, avail.height() - frame_h));
    // Re-centre within the work area so no edge spills off-screen.
    const QRect frame = frameGeometry();
    move(avail.left() + (avail.width() - frame.width()) / 2,
         avail.top() + (avail.height() - frame.height()) / 2);
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
        // Rebuild + restart the grid only once the modal closes — restarting
        // mid-flow would fight the modal's snapshot for the same USB device.
        connect(camera_, &QDialog::finished, camera_view_,
                [this](int) { camera_view_->reload(); });
    }
    // Free the cameras so the modal's Configure/Areas snapshot can open them.
    camera_view_->release_streams();
    camera_->show();
    camera_->raise();
    camera_->activateWindow();
}

void MainWindow::on_apply_resolution(int index) {
    const auto [w, h] = settings::PRESETS[static_cast<size_t>(index)];
    state_->width = w;
    state_->height = h;
    settings::save(db_, *state_);
    resize_within_screen(static_cast<int>(w), static_cast<int>(h));
}

void MainWindow::on_theme_changed(bool dark) {
    state_->dark = dark;
    settings::save(db_, *state_);
    apply_theme(dark);
}

void MainWindow::on_toggle_fullscreen(bool fullscreen) {
    set_fullscreen(fullscreen);
}

void MainWindow::set_fullscreen(bool on) {
    state_->fullscreen = on;
    settings::save(db_, *state_);
    if (on)
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
        resize_within_screen(static_cast<int>(d.width), static_cast<int>(d.height));
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
