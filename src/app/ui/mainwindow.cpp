#include "ui/mainwindow.h"

#include "hardware/collect.h"
#include "mode/config.h"
#include "mode/reset.h"
#include "settings/repo.h"
#include "ui/camera/camera_dialog.h"
#include "ui/camera/camera_view.h"
#include "ui/settings/display_confirm_dialog.h"
#include "ui/settings/mode_confirm_dialog.h"
#include "ui/settings/settings_dialog.h"
#include "ui/theme.h"

#include <QApplication>
#include <QDebug>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QIcon>
#include <QKeySequence>
#include <QLabel>
#include <QMessageBox>
#include <QPalette>
#include <QPixmap>
#include <QPushButton>
#include <QScreen>
#include <QShortcut>
#include <QShowEvent>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <exception>
#include <utility>

#ifndef APP_VERSION
#define APP_VERSION "0.0.0"
#endif

namespace denso::ui {

namespace {

// Report a switch failure WITHOUT blocking. Deliberately non-modal: perform_switch
// runs the whole teardown/transaction/rebuild synchronously on the GUI thread, and
// entering a nested modal event loop from inside it would let queued events run
// mid-lifecycle. The box owns itself and disappears when dismissed.
void show_non_modal(QWidget* parent, QMessageBox::Icon icon, const QString& title,
                    const QString& text) {
    auto* box = new QMessageBox(icon, title, text, QMessageBox::Ok, parent);
    box->setAttribute(Qt::WA_DeleteOnClose);
    box->setModal(false);
    box->show();
    box->raise();
}

}  // namespace

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
    // Held as a member so apply_camera_button_gate() can disable it: ball_leveler
    // exposes no camera wizard at all (spec §2.1, §7.2).
    camera_btn_ = new QPushButton(QStringLiteral("Camera"));
    camera_btn_->setObjectName(QStringLiteral("cameraButton"));
    connect(camera_btn_, &QPushButton::clicked, this, &MainWindow::open_camera);
    auto* settings_btn = new QPushButton(QStringLiteral("Settings"));
    settings_btn->setObjectName(QStringLiteral("settingsButton"));
    connect(settings_btn, &QPushButton::clicked, this, &MainWindow::open_settings);
    bar->addWidget(camera_btn_, 0);
    bar->addWidget(settings_btn, 0);
    col->addWidget(top);

    // Fullscreen shortcuts: F11 toggles Fullscreen<->Windowed, Esc leaves
    // Fullscreen. These are convenience shortcuts — the top bar (with the
    // Settings button) stays visible in every mode, so a touchscreen operator can
    // also switch mode from Settings. Route through on_apply_display so the mode
    // is persisted and the dialog stays in sync.
    auto* fs = new QShortcut(QKeySequence(Qt::Key_F11), this);
    connect(fs, &QShortcut::activated, this, [this] {
        if (display_txn_active_) return;  // don't fight a pending confirm/revert
        const auto next = isFullScreen() ? settings::DisplayMode::Windowed
                                         : settings::DisplayMode::Fullscreen;
        on_apply_display(static_cast<int>(next), static_cast<int>(state_->width),
                         static_cast<int>(state_->height));
    });
    auto* esc = new QShortcut(QKeySequence(Qt::Key_Escape), this);
    connect(esc, &QShortcut::activated, this, [this] {
        if (display_txn_active_) return;
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
    connect(settings_, &SettingsDialog::switch_mode_requested, this,
            &MainWindow::on_switch_mode);

    // Adopt the COMMITTED mode from the database. Never assume digit_reader: an
    // appliance booting with a stored ball_leveler must come up gated, exactly as
    // it would right after a switch. CameraView reads the mode itself (its ctor
    // already ran reload()), so this seeds the window-owned consequences only.
    current_mode_ = mode::load(db_);
    settings_->set_current_mode(current_mode_);
    apply_camera_button_gate();
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

void MainWindow::apply_camera_button_gate() {
    // digit_reader keeps the existing behavior; ball_leveler exposes no wizard.
    camera_btn_->setEnabled(current_mode_ != mode::TargetMode::BallLeveler);
}

void MainWindow::set_switch_observer(std::function<void(SwitchEvent)> observer) {
    switch_observer_ = std::move(observer);
}

int MainWindow::camera_view_page_index() const {
    return camera_view_->current_page_index();
}

uint64_t MainWindow::camera_view_grid_reload_invocations() const {
    return camera_view_->grid_reload_invocations();
}

void MainWindow::on_switch_mode(int target) {
    // ── Refusals, cheapest first. Each one must leave EVERYTHING untouched: no
    // preview read, no confirmation, no teardown, no transaction.
    if (display_txn_active_) return;  // a display confirm/revert owns the window
    if (switch_active_) return;       // a switch is already running (busy, §6.1-R1)

    // Validate the raw selector index through the domain: from_index() maps any
    // out-of-range value to DigitReader, so an invalid enum is never stored and
    // current_mode_ can never hold garbage that to_string() would paper over.
    const mode::TargetMode want = mode::from_index(target);
    if (want == current_mode_) return;  // no switch to the already-active mode

    // NO count preview and NO pre-dialog abort gate. Both existed solely to
    // guarantee that the destructive switch could state exactly what it was about
    // to delete; a switch now deletes nothing, so there is nothing to count, and
    // a broken count query is no longer a reason to refuse an operator a
    // non-destructive, fully reversible action.
    //
    // The dialog only asks; it reads no DB and starts no transaction.
    ModeConfirmDialog dlg(want, this);
    if (dlg.exec() != QDialog::Accepted) {
        settings_->set_current_mode(current_mode_);  // Cancel restores the selector
        return;                                      // …and changes nothing else
    }

    perform_switch(want);
}

mode::TargetMode MainWindow::perform_switch(mode::TargetMode target) {
    // ── 1. Busy state — acquired EXCLUSIVELY, not merely set.
    //
    // A plain "set true, clear in RAII" is not enough: this method is public (it is
    // the dialog-free seam), and the lifecycle runs synchronously, so a re-entrant
    // call — from the observer, or from any handler reached while the GUI thread is
    // inside the teardown join — would take its own guard and clear the shared flag
    // on the way out, re-admitting further switches while the OUTER one is still
    // mid-flight. Two interleaved teardown/transaction/reload sequences is exactly
    // what §6.1-R1 forbids. Refusing here makes exclusion a property of the
    // lifecycle itself rather than of the one caller that happens to check first.
    //
    // Policy refusals (same-mode, display transaction) stay in on_switch_mode; this
    // guard is the lifecycle invariant, not the operator policy.
    if (switch_active_) {
        qWarning().noquote() << "[mode] refusing a re-entrant switch:"
                             << "one is already in progress";
        return current_mode_;  // nothing torn down, nothing changed
    }
    switch_active_ = true;
    struct BusyGuard {
        bool* flag;
        ~BusyGuard() { *flag = false; }
    } busy{&switch_active_};

    // Observer calls are test-only instrumentation and must never be able to change
    // production behavior — so an exception thrown by an installed observer is
    // contained here rather than unwinding the lifecycle it is only watching.
    const auto fire = [this](SwitchEvent e) noexcept {
        if (!switch_observer_) return;
        try {
            switch_observer_(e);
        } catch (...) {
        }
    };

    // What the TRANSACTION did, tracked separately from whether the rest of the
    // handler succeeded. These are genuinely different facts: a commit followed by a
    // failed UI update must never be reported as "rolled back, nothing was changed"
    // — that would tell the operator their processing setup survived when it was in
    // fact destroyed.
    enum class Outcome { NotReached, Committed, RolledBack };
    Outcome outcome = Outcome::NotReached;
    QString failure;  // non-empty iff something went wrong (SQL error, or what())

    try {
        // ── 2. Teardown ONLY. CameraView::teardown_for_switch() delegates to
        // CameraGrid's one authoritative primitive (clear()) and deliberately does
        // NOT reload()/re-query runtime() — that would restart the OLD mode's
        // cameras before the reset commits (spec §6.2). Joining the capture and
        // inference threads here is what guarantees nothing can still reach the
        // ZoneSink, and destroying the BrazingReporter is what discards (and logs)
        // any undelivered snapshot.
        fire(SwitchEvent::TeardownStarted);
        camera_view_->teardown_for_switch();
        fire(SwitchEvent::TeardownCompleted);

        // ── 3. The atomic, NON-DESTRUCTIVE switch. Writes mode.target and
        // disables reporting in one transaction and deletes nothing.
        fire(SwitchEvent::TransactionStarted);
        const auto r = mode::switch_mode(db_, target);
        if (r.ok) {
            outcome = Outcome::Committed;
            fire(SwitchEvent::TransactionCommitted);
        } else {
            outcome = Outcome::RolledBack;
            fire(SwitchEvent::TransactionRolledBack);
            failure = QString::fromStdString(r.error);  // verbatim SQL error
        }
    } catch (const std::exception& e) {
        failure = QString::fromUtf8(e.what());
    } catch (...) {
        failure = QStringLiteral("unknown error");
    }
    // `outcome` is still NotReached iff the teardown or the transaction call itself
    // threw — i.e. we do not know that anything was committed.

    // ── 4/5. Adopt the mode. On a commit it becomes the target — and ONLY NOW,
    // never optimistically before (spec §12.17). On a rollback, or on a throw where
    // the transaction's fate is unknown, RE-READ the database instead of keeping the
    // value we were asked for: SQLite's atomicity cannot stop the running process
    // from disagreeing with the DB, and that disagreement is worse than the failure.
    //
    // Everything from here is best-effort recovery, so each step is individually
    // protected: a throw while recovering must not abandon the steps after it. This
    // is not an absolute guarantee (recovery allocates, so std::bad_alloc can still
    // defeat it) — it is a boundary that survives the failures actually reachable.
    // Each protected step RECORDS its failure. Swallowing one silently would be the
    // worst of both worlds: the operator would be shown the success path while the
    // window was in fact left half-updated.
    const auto note = [&failure](const QString& what) {
        if (!failure.isEmpty()) failure += QStringLiteral("; ");
        failure += what;
    };

    try {
        current_mode_ = (outcome == Outcome::Committed) ? target : mode::load(db_);
    } catch (...) {
        note(QStringLiteral("could not determine the mode now in effect"));
    }
    try {
        apply_camera_button_gate();
        settings_->set_current_mode(current_mode_);
    } catch (...) {
        note(QStringLiteral("could not update the window for the new mode"));
    }
    last_switch_error_ = failure;

    if (outcome == Outcome::Committed && failure.isEmpty()) {
        qInfo().noquote() << "[mode] switched to" << mode::to_string(current_mode_);
    } else if (outcome == Outcome::Committed) {
        qCritical().noquote() << "[mode] switch COMMITTED but the window could not"
                              << "finish updating:" << failure;
    } else if (outcome == Outcome::RolledBack) {
        qCritical().noquote() << "[mode] switch failed, rolled back:" << failure;
    } else {
        qCritical().noquote() << "[mode] switch did not complete normally; the"
                              << "transaction's fate is UNKNOWN. Recovered from DB"
                              << "state:" << failure;
    }

    // ── 6. Rebuild for whichever mode is now in effect. status.json is written by
    // this reload — CameraGrid is its single runtime owner (live refresh for
    // digit_reader, publish_idle_status() for ball_leveler) — so MainWindow never
    // writes it and can never overwrite a real verdict with a placeholder.
    // reload() is non-failing for this feature: per-camera engine failures are
    // firewalled inside the grid and surface as an Offline tile. Still protected,
    // because leaving the view torn down is the one outcome worth guarding against.
    fire(SwitchEvent::ReloadStarted);
    try {
        camera_view_->reload();
    } catch (...) {
        note(QStringLiteral("could not rebuild the camera view"));
        last_switch_error_ = failure;
        // Logged HERE, not with the branches above: those ran before the reload, so
        // a switch that committed cleanly and then failed only to rebuild would
        // otherwise leave a lone "switched to ..." info line and no critical record.
        qCritical().noquote() << "[mode] the camera view could not be rebuilt after"
                              << "the switch:" << failure;
    }

    // Report only after the pipeline is back, so the operator is not reading a
    // message box over a dead window. The three texts are genuinely different
    // claims, and saying the wrong one about a DESTRUCTIVE operation is its own
    // fault: "rolled back, nothing changed" is only ever said when the transaction
    // actually reported a rollback.
    if (!failure.isEmpty()) {
        QString body;
        switch (outcome) {
            case Outcome::Committed:
                body = QStringLiteral(
                           "The mode switch COMPLETED, but the window could not "
                           "finish updating. Restart the application.\n\n%1")
                           .arg(failure);
                break;
            case Outcome::RolledBack:
                body = QStringLiteral("The mode switch failed and was rolled back. "
                                      "Nothing was changed.\n\n%1")
                           .arg(failure);
                break;
            case Outcome::NotReached:
                // We never saw the transaction resolve, so we must NOT promise a
                // rollback. Report only what is known: the mode below was read back
                // from the database.
                body = QStringLiteral(
                           "The mode switch did not complete normally, and whether "
                           "it was applied is unknown. The application recovered "
                           "using the mode stored in the database (%1). Restart and "
                           "verify the configuration.\n\n%2")
                           .arg(QString::fromLatin1(mode::to_string(current_mode_)),
                                failure);
                break;
        }
        show_non_modal(this, QMessageBox::Critical, QStringLiteral("Switch Target Mode"),
                       body);
    }

    return current_mode_;
}

void MainWindow::open_camera() {
    // The wizard is a digit_reader concept. ball_leveler ships no processing setup
    // (spec §2.1), so the gate lives HERE as well as on the button — a disabled
    // button is a UI affordance, not an invariant.
    if (current_mode_ == mode::TargetMode::BallLeveler) return;
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
    // Canonical + deterministic: hide -> set absolute flags -> clear ALL window
    // state -> show -> re-assert geometry. Guarantees no stale frameless hint or
    // maximized/fullscreen bit survives a switch (e.g. Borderless -> Fullscreen).
    const bool frameless_now = windowFlags().testFlag(Qt::FramelessWindowHint);
    if (frameless_now != plan.frameless || isFullScreen()) {
        hide();
        setWindowFlag(Qt::FramelessWindowHint, plan.frameless);
        setWindowState(Qt::WindowNoState);  // canonical: drop fullscreen/maximized
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
    if (display_txn_active_) return;  // a request is already in flight
    // Guard from the moment of request — not inside run_apply_display — so a
    // second Apply/F11/Reset can't queue in the gap before the deferred tick runs.
    display_txn_active_ = true;
    // Defer one tick so the app-modal Settings dialog (whose Apply click we're
    // inside) fully closes before we open the confirm dialog — otherwise the two
    // modal loops stack.
    QTimer::singleShot(0, this,
                       [this, mode, width, height] { run_apply_display(mode, width, height); });
}

void MainWindow::run_apply_display(int mode, int width, int height) {
    const auto requested = static_cast<settings::DisplayMode>(mode);
    const settings::DisplayState before = current_display_state();  // == committed state_
    const settings::TransitionPlan plan = settings::plan_transition(
        before, requested, static_cast<uint32_t>(width), static_cast<uint32_t>(height),
        platform_caps());

    apply_display_mode(plan);

    if (!plan.needs_confirm) {  // same mode/size, or platform forced a no-op
        commit_display(plan);
        display_txn_active_ = false;
        return;
    }
    // Let the window-system settle (X11 re-decoration -> real frame margins) then
    // confirm above the (maybe fullscreen) window. Re-fit Windowed once margins
    // are known so a frameless->Windowed switch centers correctly on xcb.
    QTimer::singleShot(0, this, [this, plan, before] {
        if (plan.geom == settings::TransitionPlan::Geom::ResizeWithinScreen) {
            resize_within_screen(static_cast<int>(plan.width), static_cast<int>(plan.height));
        }
        DisplayConfirmDialog dlg(15, this);
        const bool kept = dlg.exec() == QDialog::Accepted;  // timeout/close -> reject
        if (kept) {
            commit_display(plan);
        } else {
            // Revert to the previous committed semantic state (state_ is untouched
            // until commit, so `before` still describes it). Re-apply, don't replay
            // raw flags.
            const settings::TransitionPlan revert = settings::plan_transition(
                before, before.mode, before.width, before.height, platform_caps());
            apply_display_mode(revert);
        }
        display_txn_active_ = false;
    });
}

void MainWindow::on_theme_changed(bool dark) {
    state_->dark = dark;
    settings::save(db_, *state_);
    apply_theme(dark);
}

void MainWindow::on_reset_defaults() {
    if (display_txn_active_) return;  // don't mutate display mid-transaction
    const settings::Settings d;  // defaults (Windowed, 1600x900, dark)

    // Reset is the recovery action — it always lands on the safe Windowed default,
    // so it applies DIRECTLY with no confirm/revert. A countdown here would be
    // backwards: a timeout would restore the very (possibly unusable) state the
    // operator reset away from. Build the plan from the CURRENT state (pre-reset)
    // so the transition is computed correctly, then apply and persist.
    const settings::TransitionPlan p = settings::plan_transition(
        current_display_state(), d.mode, d.width, d.height, platform_caps());
    apply_display_mode(p);

    *state_ = d;
    settings::save(db_, *state_);
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
