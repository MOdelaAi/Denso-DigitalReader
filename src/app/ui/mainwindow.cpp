#include "ui/mainwindow.h"

#include "hardware/collect.h"
#include "mode/config.h"
#include "mode/reset.h"
#include "settings/repo.h"
#include "ui/camera/camera_dialog.h"
#include "ui/camera/camera_view.h"
#include "ui/engine_session.h"
#include "ui/warmup_state.h"
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

#include <algorithm>
#include <QPalette>
#include <QPixmap>
#include <QPushButton>
#include <QScreen>
#include <QShortcut>
#include <QShowEvent>
#include <QStringList>
#include <QStyle>
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
      warmup_(warmup), engines_(engines) {
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
    // Non-modal outcome of the last refresh. A message box would steal focus on a
    // panel an operator may be watching from across the cell, and per-camera
    // connection faults already surface on the tile that owns them; this line is
    // for the one thing a tile cannot say — that the REBUILD itself came up short.
    refresh_status_ = new QLabel;
    refresh_status_->setObjectName(QStringLiteral("refreshStatus"));
    refresh_status_->setProperty("faint", true);
    refresh_status_->setVisible(false);
    refresh_btn_ = new QPushButton(QStringLiteral("Refresh Cameras"));
    refresh_btn_->setObjectName(QStringLiteral("refreshCamerasButton"));
    refresh_btn_->setToolTip(
        QStringLiteral("Reconnect and reload all configured cameras"));
    refresh_btn_->setAccessibleName(QStringLiteral("Refresh cameras"));
    connect(refresh_btn_, &QPushButton::clicked, this, &MainWindow::refresh_cameras);
    auto* settings_btn = new QPushButton(QStringLiteral("Settings"));
    settings_btn->setObjectName(QStringLiteral("settingsButton"));
    connect(settings_btn, &QPushButton::clicked, this,
            [this] { open_settings(/*server_page*/ false); });
    // Backend status sits furthest right, away from the actions, because it is a
    // STATE the operator reads — not another thing to press by reflex. Clicking it
    // opens the settings it reports on; it deliberately does NOT toggle reporting,
    // so a stray touch on a production panel cannot stop delivery.
    backend_btn_ = new QPushButton;
    backend_btn_->setObjectName(QStringLiteral("backendStatus"));
    backend_btn_->setProperty("flatText", true);
    connect(backend_btn_, &QPushButton::clicked, this,
            [this] { open_settings(/*server_page*/ true); });
    bar->addWidget(refresh_status_, 0);
    bar->addWidget(refresh_btn_, 0);
    bar->addWidget(camera_btn_, 0);
    bar->addWidget(settings_btn, 0);
    bar->addWidget(backend_btn_, 0);
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
    connect(camera_view_, &CameraView::brazing_status_changed, this,
            &MainWindow::on_brazing_status_changed);
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
    // PREVIEW: repaint only. Persisting here would make the dialog's Cancel
    // unable to undo anything, which is exactly the behaviour being removed.
    connect(settings_, &SettingsDialog::theme_preview_requested, this,
            [this](bool dark) { apply_theme(dark); });
    connect(settings_, &SettingsDialog::reset_defaults_requested, this,
            &MainWindow::on_reset_defaults);
    connect(settings_, &SettingsDialog::switch_mode_requested, this,
            &MainWindow::on_switch_mode);
    connect(settings_, &SettingsDialog::brazing_config_changed, this,
            &MainWindow::on_brazing_config_changed);
    // The theme's PERSISTENCE step, handed to the dialog so it can run inside its
    // write phase and refuse to apply anything if the write fails. A signal could
    // not do this: it cannot report failure back to the sender.
    settings_->set_theme_committer([this](bool dark) {
        // Rows only: the dialog owns the transaction spanning this and the
        // brazing write, so the two land or roll back together.
        //
        // A COPY, deliberately: `state_` is not touched here. If the transaction
        // later fails to commit, the database is rolled back and the in-memory
        // struct must not be left claiming the new value. The apply step
        // (on_theme_changed) updates state_, and it only runs after the commit.
        settings::Settings staged = *state_;
        staged.dark = dark;
        if (settings::save_rows(db_, staged)) {
            return true;
        }
        qWarning().noquote()
            << "[settings] could not write the display settings; the Save was"
            << "abandoned and nothing was changed";
        return false;
    });

    // Adopt the COMMITTED mode from the database. Never assume digit_reader: an
    // appliance booting with a stored ball_leveler must come up gated, exactly as
    // it would right after a switch. CameraView reads the mode itself (its ctor
    // already ran reload()), so this seeds the window-owned consequences only.
    current_mode_ = mode::load(db_);
    settings_->set_current_mode(current_mode_);
    apply_camera_button_gate();
    // CameraView reloads inside its own constructor, so any status edge it
    // produced happened before the connect above existed. Seed from the authority
    // rather than waiting for a signal that has already been and gone.
    refresh_brazing_indicator();
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

void MainWindow::open_settings(bool server_page) {
    // Re-seed from current state, reset to the first tab, then show modally.
    settings_->set_display_mode(state_->mode);
    settings_->set_window_size(state_->width, state_->height);
    settings_->set_theme_dark(state_->dark);
    settings_->show();
    // AFTER show(): showEvent resets the nav to the first page, so selecting the
    // Server section any earlier would be undone.
    if (server_page) {
        settings_->select_server_page();
    }
    settings_->raise();
    settings_->activateWindow();
}

void MainWindow::on_brazing_status_changed(BrazingStatus status) {
    if (!backend_btn_) return;
    shown_brazing_status_ = status;

    // Deliberately NOT "Connected". The backend exposes only
    // POST /api/brazing/update — no health endpoint, no persistent connection —
    // so the strongest true claim is that the reporting stack is running. ERROR
    // is the only state here that reflects the server at all, and it does so from
    // a real delivery attempt.
    QString text;
    QString state_prop;
    QString detail;
    switch (status) {
        case BrazingStatus::Off:
            text = QStringLiteral("Backend: OFF");
            state_prop = QStringLiteral("off");
            detail = QStringLiteral("Zone reporting is off.");
            break;
        case BrazingStatus::On:
            text = QStringLiteral("Backend: ON");
            state_prop = QStringLiteral("on");
            detail = QStringLiteral("Zone reporting is enabled and running.");
            break;
        case BrazingStatus::Error:
            text = QStringLiteral("Backend: ERROR");
            state_prop = QStringLiteral("error");
            detail = QStringLiteral(
                "Zone reporting is enabled, but the last delivery failed.");
            break;
    }
    backend_btn_->setText(text);
    backend_btn_->setAccessibleName(text);
    backend_btn_->setProperty("backendState", state_prop);
    // Property-driven QSS is not re-evaluated on its own.
    backend_btn_->style()->unpolish(backend_btn_);
    backend_btn_->style()->polish(backend_btn_);

    // The canonical base URL, straight from the object that owns the sender. It
    // can carry no credentials — normalize_base_url rejects userinfo outright —
    // and no reading value goes anywhere near a tooltip.
    QStringList lines{detail};
    if (camera_view_) {
        const std::string base = camera_view_->active_brazing_base_url();
        if (!base.empty()) {
            lines << QStringLiteral("Server: %1").arg(QString::fromStdString(base));
        }
    }
    lines << QStringLiteral("Click to open Server settings.");
    backend_btn_->setToolTip(lines.join(QLatin1Char('\n')));
}

void MainWindow::refresh_brazing_indicator() {
    on_brazing_status_changed(camera_view_ ? camera_view_->brazing_status()
                                           : BrazingStatus::Off);
}

void MainWindow::refresh_cameras() {
    // One refresh at a time, and never against a pipeline another lifecycle owns:
    // a mode switch is already tearing down and rebuilding, and a display
    // transaction holds a modal confirm over the window.
    if (camera_refresh_active_ || switch_active_ || display_txn_active_) {
        return;
    }
    if (!camera_view_) {
        return;
    }
    camera_refresh_active_ = true;
    refresh_btn_->setEnabled(false);
    refresh_btn_->setText(QStringLiteral("Refreshing cameras…"));
    refresh_status_->setVisible(false);

    // Defer one tick so the busy state actually reaches the screen before the
    // rebuild — which stops and JOINS every capture/inference worker and is
    // therefore synchronous by nature. Same idiom as on_apply_display.
    QTimer::singleShot(0, this, [this] {
        // THE seam. Not a re-implementation of startup: CameraGrid::reload()
        // advances the grid generation (retiring every callback the old workers
        // captured), stops and joins those workers, deletes the tiles, and
        // rebuilds from the same persisted rows. It writes nothing.
        camera_view_->reload();

        // Both numbers come from the GRID, never re-derived here: it alone knows
        // which cameras it ADMITTED (the mode's runtime()/active() filter, then
        // the deliberate four-tile cap). Counting DB rows instead would report
        // "4 of 5 cameras started" for a five-camera appliance that is behaving
        // exactly as designed.
        const size_t admitted = camera_view_->grid_admitted_count();
        const size_t started = camera_view_->grid_stream_count();
        if (started < admitted) {
            // A camera that built no runtime at all — an unusable model binding,
            // an invalid calibration. Say so plainly; the tile carries the reason.
            refresh_status_->setText(
                QStringLiteral("%1 of %2 cameras started").arg(started).arg(admitted));
            refresh_status_->setVisible(true);
        }
        refresh_btn_->setText(QStringLiteral("Refresh Cameras"));
        refresh_btn_->setEnabled(true);
        camera_refresh_active_ = false;
    });
}

void MainWindow::apply_camera_button_gate() {
    // Both modes now expose a camera wizard — digit_reader ends at Areas,
    // ball_leveler at Level calibration — so the button is enabled in both. The
    // method is KEPT rather than deleted: it is the one place the button's
    // mode-dependence lives, and a future mode that must not open the wizard
    // should re-express itself here rather than growing a new call site.
    camera_btn_->setEnabled(true);
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

uint64_t MainWindow::camera_view_grid_generation() const {
    return camera_view_->grid_generation();
}

size_t MainWindow::camera_view_stream_count() const {
    return camera_view_->grid_stream_count();
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

    // NO count preview and NO pre-dialog abort gate. The switch destroys the
    // configured setup again, but a COUNT of the rows about to go answers no
    // question the operator can act on - the button does the same thing either
    // way - while adding a query whose failure would refuse them the action
    // entirely. The confirmation names the KINDS of thing that are cleared
    // instead, which is what the decision actually turns on.
    //
    // The dialog only asks; it reads no DB and starts no transaction.
    ModeConfirmDialog dlg(want, this);
    if (dlg.exec() != QDialog::Accepted) {
        settings_->set_current_mode(current_mode_);  // Cancel restores the selector
        return;                                      // …and changes nothing else
    }

    perform_switch(want);
}

void MainWindow::prune_retired_warmups() {
    // Reclaim only the ones whose thread has already finished, so the join in
    // ~WarmupState returns immediately and the GUI never waits.
    retired_warmups_.erase(
        std::remove_if(retired_warmups_.begin(), retired_warmups_.end(),
                       [](const std::unique_ptr<WarmupState>& w) {
                           return w && !w->worker_running();
                       }),
        retired_warmups_.end());
}

void MainWindow::fail_closed_session() {
    // The destination session could not be built, but the mode HAS committed.
    // Keep the outgoing registry attached rather than substituting nothing: it
    // is immutable and mode-pure, so its allow-list authorizes nothing for the
    // mode now in effect and every get() refuses. Each camera therefore resolves
    // to Unavailable through the firewalls that already exist, which is the
    // fail-closed outcome - a null registry would instead be dereferenced by the
    // digit build path.
    //
    // The COORDINATOR must go, though: it is retired and will never report
    // again, so a camera consulting it would wait on a readiness that can never
    // arrive. With no coordinator the grid skips the gate and lets the refusal
    // speak.
    try {
        camera_view_->set_engines(engines_, nullptr);
    } catch (...) {
        // Nothing further to fall back to; the note() above already records it.
    }
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

        // ── 3. The atomic, DESTRUCTIVE switch. Writes mode.target, disables
        // reporting and clears BOTH modes' configured processing setup in one
        // transaction. It runs strictly AFTER the teardown above: nothing may
        // still be producing readings into configuration this is about to
        // delete, and no reporter may survive to POST one afterwards.
        fire(SwitchEvent::TransactionStarted);
        const auto r = mode::switch_and_reset(db_, target);
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
    // Three INDEPENDENT recovery steps, each with its own guard. Sharing one try
    // block would let a throw in the first silently skip the others — and the
    // last of them is the fix for exactly that class of staleness, so hiding it
    // behind an unrelated failure would reintroduce the defect it exists to
    // prevent. Every one records its own note(), so a partial update is reported
    // for what it actually was.
    try {
        apply_camera_button_gate();
        settings_->set_current_mode(current_mode_);
    } catch (...) {
        note(QStringLiteral("could not update the window for the new mode"));
    }
    try {
        // A switch writes brazing.enabled = 0, so the indicator MUST move. Read
        // the authority rather than assume Off: a rolled-back switch changed
        // nothing, and a ball_leveler grid with no camera never enters
        // build_zone_reporting(), so no status signal would arrive either way.
        refresh_brazing_indicator();
    } catch (...) {
        note(QStringLiteral("could not update the backend status indicator"));
    }
    try {
        // …and so must the Settings dialog, which is very likely still on screen:
        // the Switch button lives on ITS Mode page, so showEvent will not fire
        // again and the Server checkbox would sit there ticked while the database,
        // the grid and the top bar all say OFF. Passive — it re-reads the same
        // configuration, writes nothing and emits nothing (see the header). Also
        // run for a ROLLED-BACK switch: re-reading truth is right either way, and
        // guessing the outcome here would be a second authority.
        settings_->refresh_backend_state();
    } catch (...) {
        note(QStringLiteral("could not refresh the Server settings page"));
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
    // ── 5b. Replace the inference session for the mode now in effect ─────────
    // Without this the feature cannot work at all: the boot registry's allow-list
    // is immutable and get() THROWS for anything outside it, so a live switch to
    // a configured ball_leveler could never load a Float engine (spec 3.1).
    //
    // Ordering is what makes this safe, and it is already correct above: the
    // teardown at step 2 joined every capture and inference thread, so no worker
    // holds an engine pointer from the outgoing registry. Only after a COMMIT,
    // and built from current_mode_ - the mode re-read from the database - so the
    // running process and the persisted mode cannot disagree.
    if (outcome == Outcome::Committed) {
        // Retire the OUTGOING coordinator FIRST - before anything that can throw.
        // Retiring only on the success path left a window in which the mode had
        // committed but the old coordinator was still live, so a boot-wired
        // `failed` could still reach app.exit(1) AFTER the switch and take the
        // appliance dark (spec 7.3/7.5). Retirement is a consequence of the
        // COMMIT, not of the replacement succeeding.
        if (warmup_) {
            warmup_->retire();
        }
        if (owned_warmup_) {
            // Park rather than destroy: ~WarmupState joins an uncancellable
            // warm-up thread, and doing that inline would freeze the GUI.
            prune_retired_warmups();
            retired_warmups_.push_back(std::move(owned_warmup_));
        }
        warmup_ = nullptr;
        try {
            auto engines = build_engine_registry(db_, current_mode_);
            auto warmup = std::make_unique<WarmupState>(engines);
            // A post-commit warm-up failure is a CAMERA-level fault, not a
            // mode-level one (spec 7.3-7.5): the mode does NOT revert, no
            // old-mode model is loaded, and the operator can still switch back.
            // Deliberately NOT app.exit(1) - that is boot-only semantics, and
            // applying it here would take a working appliance dark over one
            // unusable model.
            connect(warmup.get(), &WarmupState::failed, this,
                    [this](const QString& err) {
                        qCritical().noquote()
                            << "[mode] model warm-up failed after the switch;"
                            << "affected cameras report unavailable:" << err;
                        // Release the cameras still waiting on a model that will
                        // now never warm. Without this they hold
                        // "Preparing model..." forever: the worker emits `failed`
                        // and never `finished`, so the pending gate has no
                        // terminating edge of its own.
                        camera_view_->settle_pending_after_warmup();
                        show_non_modal(
                            this, QMessageBox::Warning,
                            QStringLiteral("Models unavailable"),
                            QStringLiteral(
                                "The mode switch completed, but a model for this "
                                "mode could not be loaded. Affected cameras will "
                                "show as unavailable.\n\n%1").arg(err));
                    });
            camera_view_->set_engines(engines, warmup.get());
            engines_ = std::move(engines);
            owned_warmup_ = std::move(warmup);
            warmup_ = owned_warmup_.get();
            // Started only after the grid has subscribed; is_ready/is_complete
            // cover anything that still races.
            owned_warmup_->start();
        } catch (const std::exception& e) {
            note(QStringLiteral("could not prepare models for the new mode: %1")
                     .arg(QString::fromUtf8(e.what())));
            fail_closed_session();
        } catch (...) {
            note(QStringLiteral("could not prepare models for the new mode"));
            fail_closed_session();
        }
        last_switch_error_ = failure;
    }

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

    // ── The switch is over. A COMMIT dismisses Settings ───────────────────────
    //
    // The operator pressed Switch from this dialog's Mode page, so it is still on
    // screen showing a form that describes the appliance as it was before a
    // destructive reset. Once the transaction has committed, that form is wrong —
    // both modes' configured setup is gone — so put them back on the main screen.
    //
    // The gate is the COMMIT and nothing else. A cancel, a same-mode refusal, a
    // failed transaction, a rollback and an unresolved outcome all leave the
    // dialog exactly where the operator left it, because nothing changed for them
    // to be returned to. But a commit that could not finish updating the window is
    // still a commit: the form is just as invalid, and — because this dialog is
    // application-MODAL while the warning below is deliberately not — leaving it
    // up would put a critical "restart the application" message behind something
    // the operator has to dismiss first.
    //
    // Which is also why this runs BEFORE that message box rather than after it:
    // the warning should appear over the main window, immediately usable. It
    // still says exactly what went wrong, so a degraded switch cannot be mistaken
    // for an ordinary success.
    //
    // Last among the state updates, though: the dialog is dismissed only once
    // every resynchronisation step and the camera reload have had their turn.
    if (outcome == Outcome::Committed) {
        try {
            settings_->close_after_mode_switch();
        } catch (...) {
            // Deliberately NOT note()d: `failure` has already decided what the
            // operator is about to be told, and a dialog that would not close is a
            // cosmetic leftover, not a reason to retro-report the switch as more
            // broken than it was.
            qWarning().noquote()
                << "[mode] the Settings dialog could not be closed after the switch";
        }
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
    // The wizard is available in BOTH modes. Which fourth step it presents —
    // Areas or Level calibration — is decided by CameraWizardController from the
    // COMMITTED mode, so this entry point holds no mode rule of its own.
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
    // APPLY only. The dialog persisted this through the theme committer during
    // its write phase, before emitting — so writing again here would be a second,
    // unchecked write of a value already on disk, and would put the persistence
    // back AFTER the apply, which is exactly the ordering this change removes.
    state_->dark = dark;
    apply_theme(dark);
}

void MainWindow::on_brazing_config_changed() {
    // A pure forward. The dialog already persisted and validated; the grid is the
    // single owner of the reporting stack and re-reads the config itself, so there
    // is no payload to carry and nothing for the window to decide.
    if (camera_view_) {
        camera_view_->apply_brazing_config();
    }
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
