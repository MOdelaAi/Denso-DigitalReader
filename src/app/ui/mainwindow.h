// The root window — the Qt port of `app-window.slint`'s AppWindow: a top button
// bar (Camera / Settings) over the main content area, plus the Settings and
// Camera modals. It also hosts the settings persistence handlers (resolution /
// theme / fullscreen / reset) that the Rust `wiring` module installed on the
// window, since those resize the window and restyle the app; the network
// handlers live in the SettingsDialog with the panel they drive.
#pragma once

#include "brazing/brazing_status.h"   // BrazingStatus
#include "mode/mode.h"
#include "settings/settings.h"

#include <QMainWindow>
#include <QSqlDatabase>
#include <QString>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

class QLabel;
class QPushButton;

namespace denso::ui {

class SettingsDialog;
class CameraDialog;
class CameraView;
class EngineRegistry;
class WarmupState;

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    MainWindow(QSqlDatabase db, std::shared_ptr<settings::Settings> state,
               std::shared_ptr<EngineRegistry> engines, WarmupState* warmup,
               QWidget* parent = nullptr);

    /// Populate read-only fields (version, hardware), seed the persisted
    /// settings into the window + dialog, and apply the theme — the Qt port of
    /// `wiring::apply_startup`. Call before show().
    void apply_startup();

    // ── Operating-mode switch (spec §6.1, §6.6, §6.7) ──────────────────────────

    /// The exact lifecycle boundaries of perform_switch(), in firing order. The
    /// observer below reports them so a test can assert the sequence AND sample
    /// state at each boundary; no production behavior reads them.
    enum class SwitchEvent {
        TeardownStarted,
        TeardownCompleted,
        TransactionStarted,
        TransactionCommitted,
        TransactionRolledBack,
        ReloadStarted
    };

    /// The mode currently in effect in this process. After both a commit and a
    /// rollback it equals mode::load(db_) — the running process never disagrees
    /// with the database about what the appliance is doing (spec §6.7, §12.17).
    mode::TargetMode current_mode() const { return current_mode_; }

    /// The lifecycle half of the switch: teardown-only → atomic reset → rebuild.
    /// Shows NO dialog (the confirmation lives in on_switch_mode), so it is the
    /// seam tests drive directly. Returns the mode in effect when it returns:
    /// `target` after a commit, the DB's re-read mode after a rollback or an
    /// unexpected throw.
    ///
    /// EXCLUSIVE: if a switch is already running this refuses immediately and
    /// returns the current mode, changing nothing. Exclusion is enforced here — not
    /// only by the caller — because the lifecycle is synchronous and public, so a
    /// re-entrant call must not be able to interleave a second teardown/transaction
    /// into a live one (spec §6.1-R1). The operator-policy refusals (same-mode,
    /// live display transaction) belong to on_switch_mode.
    mode::TargetMode perform_switch(mode::TargetMode target);

    /// Verbatim SQL error from the most recent failed switch; empty after a
    /// successful one. The operator-facing surface for a rollback (spec §6.7).
    QString last_switch_error() const { return last_switch_error_; }

    /// Test-only instrumentation: records the lifecycle boundaries above. Default
    /// is a no-op and NO production behavior depends on it — it observes, never
    /// decides.
    void set_switch_observer(std::function<void(SwitchEvent)> observer);

    // Test-only observers of the hosted CameraView's state, so a case can assert
    // what the window actually shows without reaching into private members.
    int camera_view_page_index() const;
    uint64_t camera_view_grid_reload_invocations() const;
    /// The grid generation. Every authoritative teardown advances it, and
    /// callback_is_current() drops any worker callback captured before — so an
    /// advance across a refresh IS the proof that stale callbacks are rejected.
    uint64_t camera_view_grid_generation() const;
    /// Live camera runtimes held by the grid — duplicates would show up here.
    size_t camera_view_stream_count() const;
    /// The status the top bar is currently REPORTING. Distinct from
    /// CameraView::brazing_status() on purpose: a test that compares the two
    /// proves the indicator is showing the authority rather than its own idea.
    BrazingStatus displayed_brazing_status() const { return shown_brazing_status_; }

public slots:
    /// The Settings intent handler: validate → refuse → real counts → confirm →
    /// perform_switch. Public because it is a signal target (SettingsDialog::
    /// switch_mode_requested) and the sole entry point for the operator's intent.
    void on_switch_mode(int target);

    /// Open the camera-management wizard. Public for the same reason (it is the
    /// top-bar Camera button's handler). It SHORT-CIRCUITS in ball_leveler, so the
    /// gate holds for every caller, not just the button (spec §2.1, §7.2).
    void open_camera();

    /// Rebuild every camera runtime from the CURRENT persisted configuration.
    ///
    /// A runtime refresh, never a reset: it writes nothing, so cameras, models,
    /// areas/zones, decimal formats, Ball calibration, the operating mode and the
    /// backend configuration are untouched by construction. It delegates to the
    /// one existing seam (CameraView::reload -> CameraGrid::reload), which bumps
    /// the grid generation — so every callback the retired workers captured is
    /// rejected — stops and joins those workers, and rebuilds from the same rows.
    ///
    /// Public because it is the top-bar button's handler AND the entry point a
    /// test drives, so both exercise the same path. Refuses while a refresh, a
    /// mode switch or a display transaction is already running.
    void refresh_cameras();

    /// Batched display apply from the Settings dialog. Deferred to the next event
    /// tick (run_apply_display) so the Settings modal closes before the confirm
    /// dialog opens, and ignored while a transaction is already pending.
    void on_apply_display(int mode, int width, int height);

protected:
    void showEvent(QShowEvent* event) override;

private:
    /// Show the Settings modal. `server_page` sends the operator straight to the
    /// Server section — used by the Backend indicator, which reports on exactly
    /// those settings.
    void open_settings(bool server_page = false);

    /// Repaint the top-bar Backend indicator from the AUTHORITY (the grid, via
    /// CameraView). Called on every status signal, once after construction (the
    /// view reloads inside its own constructor, before this window can connect)
    /// and after a mode switch (a ball_leveler grid with no camera never enters
    /// build_zone_reporting, so no signal would arrive).
    void refresh_brazing_indicator();
    void on_brazing_status_changed(BrazingStatus status);

    /// Enable/disable the top-bar Camera button for the current mode. Called from
    /// the ctor (so a booted ball_leveler appliance is gated too) and after both
    /// switch outcomes.
    void apply_camera_button_gate();

    /// Resize + centre the window so the whole frame fits the screen's work
    /// area. A preset as tall as the screen (1920×1080 on a 1080p monitor)
    /// would otherwise push the title bar + bottom rows under the taskbar.
    void resize_within_screen(int width, int height);

    void run_apply_display(int mode, int width, int height);
    void on_theme_changed(bool dark);
    void on_reset_defaults();
    /// The Server settings page persisted a new Backend configuration. Forwards
    /// to CameraView, which swaps ONLY the reporting stack — no teardown, no
    /// camera reload, no engine work. Deliberately not routed through the mode
    /// switch machinery: this changes where readings are sent, nothing else.
    void on_brazing_config_changed();

    /// Execute a planned transition against the real window (canonical, ordered:
    /// hide → set absolute flags → clear state → show → re-assert geometry).
    void apply_display_mode(const settings::TransitionPlan& plan);
    /// Persist the applied plan + re-seed the dialog (called only on Keep / no-op).
    void commit_display(const settings::TransitionPlan& plan);
    settings::DisplayState current_display_state() const;
    settings::PlatformCaps platform_caps() const;

    void apply_theme(bool dark);

    QSqlDatabase db_;
    std::shared_ptr<settings::Settings> state_;
    // The inference session currently in effect. An EngineRegistry is immutable
    // and mode-pure for its life, so a committed mode switch REPLACES both of
    // these rather than widening the allow-list (spec 3.1 / 9).
    //
    // owned_warmup_ holds only the coordinators THIS window created. The boot
    // one is owned by ui::launch and must not be destroyed here; after a switch
    // we simply stop pointing at it.
    std::shared_ptr<EngineRegistry> engines_;
    std::unique_ptr<WarmupState> owned_warmup_;
    // Coordinators retired by an earlier switch whose warm-up thread has not
    // drained yet. ~WarmupState joins that thread, so destroying one inline would
    // block the GUI for an uncancellable deserialize - a second switch during
    // warm-up would freeze the window. They are inert (retire() severed every
    // connection and released the registry), so parking them costs nothing;
    // prune_retired_warmups() reclaims each once its thread has finished.
    std::vector<std::unique_ptr<WarmupState>> retired_warmups_;
    void prune_retired_warmups();
    /// Attach a fail-closed session when the destination one cannot be built.
    void fail_closed_session();
    SettingsDialog* settings_ = nullptr;
    CameraDialog* camera_ = nullptr;
    CameraView* camera_view_ = nullptr;
    WarmupState* warmup_ = nullptr;
    QPushButton* camera_btn_ = nullptr;  // top-bar Camera; gated off in ball_leveler
    QPushButton* refresh_btn_ = nullptr; // top-bar Refresh Cameras
    QPushButton* backend_btn_ = nullptr; // top-bar Backend status (opens Server settings)
    QLabel* refresh_status_ = nullptr;   // non-modal outcome of the last refresh
    bool fitted_ = false;  // first-show re-fit has run
    bool display_txn_active_ = false;  // a confirm/revert transaction is pending
    bool camera_refresh_active_ = false;  // a Refresh Cameras run is in flight
    // What the indicator currently SHOWS. Held so the widget is not re-styled on
    // every repeated status report, and so a test can read it back.
    BrazingStatus shown_brazing_status_ = BrazingStatus::Off;

    // The committed operating mode, adopted from the DB at construction and
    // updated ONLY after a transaction resolves — never optimistically.
    mode::TargetMode current_mode_ = mode::TargetMode::DigitReader;
    bool switch_active_ = false;  // a switch lifecycle is running (busy guard)
    QString last_switch_error_;   // verbatim SQL error from the last failed switch
    std::function<void(SwitchEvent)> switch_observer_;  // test-only; default no-op
};

} // namespace denso::ui
