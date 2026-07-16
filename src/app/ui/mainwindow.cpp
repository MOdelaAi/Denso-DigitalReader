#include "ui/mainwindow.h"

#include "hardware/collect.h"
#include "settings/repo.h"
#include "ui/camera/camera_dialog.h"
#include "ui/camera/camera_view.h"
#include "ui/settings/display_confirm_dialog.h"
#include "ui/settings/settings_dialog.h"
#include "ui/theme.h"

#include <QApplication>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QIcon>
#include <QKeySequence>
#include <QLabel>
#include <QPalette>
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
                       std::shared_ptr<EngineRegistry> engines, WarmupState* warmup,
                       QWidget* parent)
    : QMainWindow(parent), db_(std::move(db)), state_(std::move(state)),
      warmup_(warmup) {
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

    // Fullscreen shortcuts: F11 toggles Fullscreen<->Windowed, Esc leaves
    // Fullscreen. These are convenience shortcuts — the top bar (with the
    // Settings button) stays visible in every mode, so a touchscreen operator can
    // also switch mode from Settings. Route through on_apply_display so the mode
    // is persisted and the dialog stays in sync.
    auto* fs = new QShortcut(QKeySequence(Qt::Key_F11), this);
    connect(fs, &QShortcut::activated, this, [this] {
        const auto next = isFullScreen() ? settings::DisplayMode::Windowed
                                         : settings::DisplayMode::Fullscreen;
        on_apply_display(static_cast<int>(next), static_cast<int>(state_->width),
                         static_cast<int>(state_->height));
    });
    auto* esc = new QShortcut(QKeySequence(Qt::Key_Escape), this);
    connect(esc, &QShortcut::activated, this, [this] {
        if (isFullScreen())
            on_apply_display(static_cast<int>(settings::DisplayMode::Windowed),
                             static_cast<int>(state_->width),
                             static_cast<int>(state_->height));
    });

    // Main content area: the camera view (empty state / configured count).
    camera_view_ = new CameraView(db_, engines, warmup_);
    connect(camera_view_, &CameraView::add_camera_requested, this, &MainWindow::open_camera);
    col->addWidget(camera_view_, 1);

    setCentralWidget(central);

    // The Settings modal is created once and reused; it lives for the app's
    // lifetime, so the network worker threads always have a valid target.
    settings_ = new SettingsDialog(db_, this);
    settings_->setModal(true);
    connect(settings_, &SettingsDialog::apply_display_requested, this,
            &MainWindow::on_apply_display);
    connect(settings_, &SettingsDialog::theme_changed, this,
            &MainWindow::on_theme_changed);
    connect(settings_, &SettingsDialog::reset_defaults_requested, this,
            &MainWindow::on_reset_defaults);
}

void MainWindow::apply_startup() {
    settings_->set_app_version(QStringLiteral(APP_VERSION));

    const hardware::HardwareSpec hw = hardware::collect();
    settings_->set_hardware(QString::fromStdString(hw.os), QString::fromStdString(hw.device),
                            QString::fromStdString(hw.ram), QString::fromStdString(hw.storage));

    const settings::Settings& s = *state_;
    settings_->set_display_mode(s.mode);
    settings_->set_window_size(s.width, s.height);
    settings_->set_theme_dark(s.dark);

    // Apply the persisted mode through the same planner, but with no confirm
    // dialog — persisted state is already trusted.
    const settings::TransitionPlan boot = settings::plan_transition(
        settings::DisplayState{settings::DisplayMode::Windowed, s.width, s.height, {}},
        s.mode, s.width, s.height, platform_caps());
    apply_display_mode(boot);
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
    settings_->set_display_mode(state_->mode);
    settings_->set_window_size(state_->width, state_->height);
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

settings::PlatformCaps MainWindow::platform_caps() const {
    const QString plat = QGuiApplication::platformName();
    // eglfs/linuxfb have no window manager -> windowed/borderless are fictional.
    const bool windowing = plat != QStringLiteral("eglfs") &&
                           plat != QStringLiteral("linuxfb");
    return settings::PlatformCaps{windowing};
}

settings::DisplayState MainWindow::current_display_state() const {
    const QScreen* scr = screen();
    return settings::DisplayState{state_->mode, state_->width, state_->height,
                                  scr ? scr->name().toStdString() : std::string{}};
}

void MainWindow::apply_display_mode(const settings::TransitionPlan& plan) {
    // Canonical + deterministic: hide -> set absolute flags -> clear fullscreen
    // state -> show -> re-assert geometry. Guarantees no stale frameless hint
    // survives a switch (e.g. Borderless -> Fullscreen).
    const bool frameless_now = windowFlags().testFlag(Qt::FramelessWindowHint);
    if (frameless_now != plan.frameless || isFullScreen()) {
        hide();
        setWindowFlag(Qt::FramelessWindowHint, plan.frameless);
        setWindowState(windowState() & ~Qt::WindowFullScreen);
        show();
    }
    switch (plan.geom) {
        case settings::TransitionPlan::Geom::ResizeWithinScreen:
            showNormal();
            resize_within_screen(static_cast<int>(plan.width),
                                 static_cast<int>(plan.height));
            break;
        case settings::TransitionPlan::Geom::FullScreenRect: {
            showNormal();
            const QScreen* scr = screen();
            if (scr) setGeometry(scr->geometry());
            break;
        }
        case settings::TransitionPlan::Geom::NativeFullscreen:
            showFullScreen();
            break;
    }
}

void MainWindow::commit_display(const settings::TransitionPlan& plan) {
    state_->mode = plan.mode;
    if (plan.mode == settings::DisplayMode::Windowed) {  // only Windowed carries a size
        state_->width = plan.width;
        state_->height = plan.height;
    }
    settings::save(db_, *state_);
    settings_->set_display_mode(state_->mode);
    settings_->set_window_size(state_->width, state_->height);
}

void MainWindow::on_apply_display(int mode, int width, int height) {
    const auto requested = static_cast<settings::DisplayMode>(mode);
    const settings::DisplayState before = current_display_state();
    const settings::TransitionPlan plan = settings::plan_transition(
        before, requested, static_cast<uint32_t>(width), static_cast<uint32_t>(height),
        platform_caps());

    apply_display_mode(plan);

    if (!plan.needs_confirm) {  // same mode/size, or platform forced a no-op
        commit_display(plan);
        return;
    }
    // Real display change -> confirm above the (maybe fullscreen) window.
    DisplayConfirmDialog dlg(15, this);
    dlg.raise();
    dlg.activateWindow();
    if (dlg.exec() == QDialog::Accepted) {
        commit_display(plan);
    } else {
        // Revert by re-applying the previous semantic state (no raw-flag replay).
        const settings::TransitionPlan revert = settings::plan_transition(
            current_display_state(), before.mode, before.width, before.height,
            platform_caps());
        apply_display_mode(revert);
    }
}

void MainWindow::on_theme_changed(bool dark) {
    state_->dark = dark;
    settings::save(db_, *state_);
    apply_theme(dark);
}

void MainWindow::on_reset_defaults() {
    const settings::Settings d;  // defaults (Windowed, 1600x900, dark)
    state_->mode = d.mode;
    state_->width = d.width;
    state_->height = d.height;
    state_->dark = d.dark;
    settings::save(db_, *state_);

    const settings::TransitionPlan p = settings::plan_transition(
        current_display_state(), d.mode, d.width, d.height, platform_caps());
    apply_display_mode(p);
    settings_->set_display_mode(d.mode);
    settings_->set_window_size(d.width, d.height);
    settings_->set_theme_dark(d.dark);
    apply_theme(d.dark);
}

void MainWindow::apply_theme(bool dark) {
    // Qualify: unqualified `palette` would resolve to the inherited
    // QWidget::palette() member, hiding the free theme function.
    const Palette p = denso::ui::palette(dark);

    // QLineEdit foreground — both the text AND the placeholder ("ghost") text —
    // is owned by the PALETTE, not the stylesheet. Qt 6.2's QSS engine, when a
    // QLineEdit stylesheet sets `color`, derives the placeholder from that colour
    // and IGNORES QPalette::PlaceholderText — which made the ghost text render
    // near-white (unreadable). So the theme's QSS no longer sets a foreground on
    // QLineEdit or the universal QWidget rule; the palette provides it, and the
    // placeholder role finally takes effect. (QSS `placeholder-text-color` only
    // exists in Qt 6.5+, so it isn't an option on the Jetson's 6.2.4.)
    QPalette pal = qApp->palette();
    for (QPalette::ColorGroup g : {QPalette::Active, QPalette::Inactive}) {
        pal.setColor(g, QPalette::WindowText, p.txt);
        pal.setColor(g, QPalette::Text, p.txt);
        pal.setColor(g, QPalette::ButtonText, p.txt);
        pal.setColor(g, QPalette::PlaceholderText, p.txt_faint);
    }
    pal.setColor(QPalette::Disabled, QPalette::Text, p.txt_faint);
    pal.setColor(QPalette::Disabled, QPalette::PlaceholderText, p.txt_faint);
    qApp->setPalette(pal);

    qApp->setStyleSheet(style_sheet(p));
}

} // namespace denso::ui
