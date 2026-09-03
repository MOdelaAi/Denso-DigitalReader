// The Settings modal: a left nav over five panels (Appearance, Display, System,
// Network, About), ported from `settings-modal.slint` + the `settings/*.slint`
// panels. Display/appearance/reset actions are surfaced as signals for the
// window to persist + apply; the network panel owns its own DB-backed apply and
// the threaded scan/connect/refresh (the Qt analog of the Slint window's
// network callbacks + `std::thread`/`upgrade_in_event_loop`).
#pragma once

#include "brazing/url.h"   // BaseUrlParts — the address controls' shared shape
#include "mode/mode.h"
#include "settings/display.h"

#include <QDialog>

#include <functional>
#include <QSqlDatabase>
#include <QString>

#include <cstdint>
#include <string>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QStackedWidget;

namespace denso::ui {

class NetworkPanel;

class SettingsDialog : public QDialog {
    Q_OBJECT

public:
    explicit SettingsDialog(QSqlDatabase db, QWidget* parent = nullptr);

    // ── Startup seeding (read-only / persisted state pushed in by the window).
    void set_app_version(const QString& version);
    void set_hardware(const QString& os, const QString& device, const QString& ram,
                      const QString& storage);
    void set_display_mode(settings::DisplayMode mode);      // no signal emitted
    void set_window_size(uint32_t width, uint32_t height);  // no signal emitted
    void set_theme_dark(bool dark);                         // no signal emitted
    // Seed the Target Mode selector to the appliance's current mode. Updates the
    // selector + the Switch-and-Reset button's enabled state WITHOUT emitting a
    // switch request (seeding is not an intent).
    void set_current_mode(mode::TargetMode mode);

    /// Install the theme COMMITTER: persist `dark` and report whether the write
    /// landed. Called during Save changes' persistence phase, BEFORE any runtime
    /// signal, so a failed theme write cannot leave half the form applied.
    ///
    /// A functor rather than a signal because a signal cannot report failure, and
    /// the whole point of the phase split is that this one can. MainWindow owns
    /// the settings struct, so it owns the write; the dialog owns the ordering.
    /// Left unset (in tests that drive the dialog alone) it is treated as
    /// "nothing to persist", never as a failure.
    void set_theme_committer(std::function<bool(bool)> commit);

    /// Select the Server page. Called after show() (showEvent resets the nav to
    /// the first page), so the top-bar Backend indicator can take the operator
    /// straight to the settings it reports on.
    void select_server_page();

    /// PASSIVE re-sync of the Server page to the authoritative stored Backend
    /// configuration. Call it when something OUTSIDE this dialog has changed that
    /// configuration while the dialog is alive — today, a committed mode switch,
    /// which writes `brazing.enabled = 0`.
    ///
    /// Needed because the Switch button lives on this dialog's Mode page, so a
    /// switch happens with the dialog still VISIBLE: showEvent never fires again,
    /// and the Server page would keep showing the ticked box the operator opened
    /// it with. The database, the grid and the top-bar indicator would all say
    /// OFF while this checkbox said ON.
    ///
    /// PASSIVE means exactly that. It re-reads the same configuration everything
    /// else reads — it holds no state of its own — and it:
    ///   • writes NOTHING to the database (it is a reader);
    ///   • emits NO brazing_config_changed (nothing asked for a reconfiguration —
    ///     the runtime already followed the switch through its own path, and
    ///     re-emitting would make a passive redisplay look like an operator
    ///     action);
    ///   • never ARMS "Save changes" (the widgets move under suppress_signals_),
    ///     so an authoritative refresh cannot masquerade as an unsaved edit.
    ///
    /// A committed switch outranks an unsaved tick of the box: the operator's
    /// pending intent is overwritten, because the appliance has since been told
    /// something authoritative about what it is doing.
    void refresh_backend_state();

    /// Close the dialog because a mode switch COMMITTED. Call only after the
    /// switch has genuinely succeeded and the window has finished resynchronising
    /// — never on a cancel, a refusal, a rollback or an unresolved transaction.
    ///
    /// A committed switch is a DISCARD, not a Save. It has just destroyed the
    /// configured setup of both modes, so leaving the operator on a form whose
    /// staged fields describe the world before that is worse than useless; and
    /// saving those fields on their behalf would persist edits they never
    /// confirmed. So this deliberately routes through reject() — the dialog's ONE
    /// discard path — rather than accept():
    ///   • the unsaved theme PREVIEW is restored to the persisted theme (a
    ///     preview left applied would leave the running app disagreeing with the
    ///     database, and there is no dialog left to undo it from);
    ///   • the dirty baseline is re-captured, so the reused dialog reopens clean;
    ///   • nothing is written and no Save/apply signal is emitted.
    ///
    /// It asks for no discard confirmation: the operator has just confirmed a
    /// destructive switch, and a second "are you sure?" over the top of that would
    /// be asking again about a decision already made.
    ///
    /// A no-op when the dialog is not visible — there is nothing to close, and a
    /// hidden dialog holds no live preview (every path that hides it already
    /// resolved one).
    void close_after_mode_switch();

    // Test-only observers of the single-primary-action contract, so a case can
    // assert what the operator can actually do without re-deriving it from the
    // widget tree.
    bool is_dirty() const { return dirty_; }

public slots:
    /// EVERY dismissal that is not a successful save funnels through here: the
    /// Cancel button, Esc, the header's close glyph and the window manager's
    /// close box all end in QDialog::reject(). Overriding it — rather than wiring
    /// only the button — is what makes "Cancel applies nothing" true for all of
    /// them, because the live theme preview has to be undone whichever way the
    /// operator leaves. Public, like the base-class slot it replaces.
    void reject() override;

public:

protected:
    // The Slint modal is recreated on each open, so it always starts on the
    // first tab — reset the nav to match when the reused dialog is shown.
    void showEvent(QShowEvent* event) override;

signals:
    // Batched display apply: mode is static_cast<int>(DisplayMode); width/height
    // are the windowed size. The window runs the confirm/revert transaction.
    // Emitted ONLY from Save changes — never from a bare selector change.
    void apply_display_requested(int mode, int width, int height);
    // COMMIT the theme: persist it and apply it. Emitted only from Save changes.
    void theme_changed(bool dark);
    // PREVIEW the theme: apply it to the running app WITHOUT persisting.
    //
    // Split from theme_changed so the toggle can keep its immediate visual
    // feedback while Cancel stays honest — the dialog re-emits this with the
    // theme it opened on, so a cancelled edit leaves nothing behind in the
    // window or in the database. A listener MUST NOT persist on this signal.
    void theme_preview_requested(bool dark);
    void reset_defaults_requested();
    // Application-wide operating-mode switch INTENT (spec §5/§7). `target` is
    // static_cast<int>(mode::TargetMode). Emitted only when the operator clicks
    // Switch (never for a bare selector change, never for seeding). MainWindow
    // handles it (ModeConfirmDialog → teardown → switch_mode → rebuild). This
    // dialog performs none of that.
    void switch_mode_requested(int target);
    // The Server page's Backend (brazing) configuration was successfully
    // PERSISTED. Emitted only after brazing::save() reported success, and never
    // for input the dialog rejected — a listener may therefore treat it as "the
    // stored config changed, re-read it" with no payload. MainWindow forwards it
    // to CameraView → CameraGrid::apply_brazing_config(), which swaps only the
    // reporting stack. This dialog performs none of that.
    void brazing_config_changed();

private:
    QWidget* build_display();
    QWidget* build_mode();
    QWidget* build_system();
    QWidget* build_network();
    QWidget* build_server();
    QWidget* build_about();

    /// Re-seed the Server page's controls from the DATABASE and clear the status
    /// line. The dialog is created once and reused, so without this a mode switch
    /// (which writes brazing.enabled = 0 behind its back) would leave a stale
    /// ticked box the operator could Save straight back.
    void reload_server_page();
    /// Recompute the "Requests will be sent to: …" line from the two fields AS
    /// TYPED, through brazing::endpoint_url() — the same function the transport
    /// composes with, so the preview cannot promise an endpoint the client would
    /// not use. Purely derived: it reads the widgets and writes one label, never
    /// the database and never the other fields.
    void update_endpoint_preview();
    /// The ONE writer of the Server page's status line. Re-derives the endpoint
    /// preview afterwards, which is what lets the preview stand aside for a
    /// message the status line is already carrying.
    void set_server_status(const QString& message, bool warn);
    /// Write the endpoint preview, suppressing it to a dash when it would repeat
    /// the visible status line verbatim.
    void set_endpoint_preview(const QString& text);
    /// The three address controls as the shared composer takes them. One reader,
    /// so the preview and the save path can never disagree about what the form
    /// currently says.
    brazing::BaseUrlParts current_base_parts() const;
    /// Set/clear the invalid marking on BOTH address controls.
    void mark_base_fields_invalid(bool invalid);
    /// Redden whichever of the address/port controls actually caused the refusal,
    /// determined by re-asking the shared composer rather than by a second copy
    /// of the port rule.
    void mark_offending_base_field(const std::string& api_path);
    /// Validate + persist the Server page. Returns true when the configuration
    /// actually reached the database; shows an operator-facing reason and returns
    /// false otherwise. NEVER reports success for input it rejected.
    bool save_server_settings();

    /// The ONE primary action. Validates every dirty page, persists what needs
    /// persisting, emits the runtime-apply signals in that order, and closes.
    /// Keeps the dialog open (and persists/emits nothing) on a validation or
    /// write failure.
    void save_changes();
    /// Every value on this form that the operator can edit, as one comparable
    /// snapshot. Dirty is a COMPARISON against the baseline, not a sticky flag:
    /// a flag cannot express "this page was re-synced to authoritative state, so
    /// its edit no longer exists" without also discarding an unsaved edit on some
    /// other page.
    struct FormState {
        int     display_mode = -1;
        int     window_size = -1;
        bool    dark = true;
        bool    brazing_enabled = false;
        // The server address as EDITED — three controls, not the one canonical
        // string they compose to. Dirty must track what the operator can see and
        // change: comparing composed values would call "192.168.1.1" typed into
        // an empty box no change at all until it happened to become valid.
        QString brazing_scheme;
        QString brazing_host;
        QString brazing_port;
        QString brazing_api_path;

        friend bool operator==(const FormState& a, const FormState& b) {
            return a.display_mode == b.display_mode && a.window_size == b.window_size &&
                   a.dark == b.dark && a.brazing_enabled == b.brazing_enabled &&
                   a.brazing_scheme == b.brazing_scheme &&
                   a.brazing_host == b.brazing_host &&
                   a.brazing_port == b.brazing_port &&
                   a.brazing_api_path == b.brazing_api_path;
        }
        friend bool operator!=(const FormState& a, const FormState& b) {
            return !(a == b);
        }
    };
    FormState current_form() const;
    /// Adopt the form as it stands as the new "nothing to save" reference.
    void capture_baseline();
    /// Re-derive dirty_ from baseline_ and repaint the primary action. Called
    /// from every edit signal; a no-op while seeding (suppress_signals_), because
    /// the seeding path re-captures the baseline itself.
    void recompute_dirty();
    void sync_save_enabled();  // Save changes follows dirty_

    void rebuild_window_sizes();  // filter PRESETS to this dialog's screen
    void sync_size_enabled();     // enable window_size_ only in Windowed
    void sync_mode_button();      // disable Switch-and-Reset when selected == current
    settings::DisplayMode staged_mode() const;

    QSqlDatabase db_;
    bool suppress_signals_ = false;
    // Has the operator changed anything since this dialog was opened/seeded?
    // DERIVED from baseline_, so it answers the real question ("does the form
    // still differ from what is stored?") rather than "did anything ever move?".
    // Gates the single primary action, so "Save changes" cannot claim to have
    // done something when there was nothing to do.
    bool dirty_ = false;
    FormState baseline_;
    // The theme in effect when the dialog was opened. The toggle PREVIEWS live,
    // so Cancel needs the value to restore — without it, "Cancel applies nothing"
    // would be false the moment anyone flicked dark mode.
    bool entry_dark_ = true;
    QPushButton* save_btn_ = nullptr;   // the ONE primary action
    // Installed by MainWindow; see set_theme_committer. Empty in dialog-only
    // tests, where there is no settings struct to write.
    std::function<bool(bool)> theme_committer_;

    QListWidget* nav_ = nullptr;
    QStackedWidget* stack_ = nullptr;

    // Appearance / Display
    QCheckBox* dark_switch_ = nullptr;
    QComboBox* display_mode_ = nullptr;
    QComboBox* window_size_ = nullptr;
    QLabel* window_size_hint_ = nullptr;

    // Target Mode
    QComboBox* mode_select_ = nullptr;
    QPushButton* switch_mode_btn_ = nullptr;
    mode::TargetMode current_mode_ = mode::TargetMode::DigitReader;

    // System / About
    QLabel* hw_os_ = nullptr;
    QLabel* hw_device_ = nullptr;
    QLabel* hw_ram_ = nullptr;
    QLabel* hw_storage_ = nullptr;
    QLabel* about_version_ = nullptr;

    // Network page — a self-contained widget owning its cards + async handlers.
    NetworkPanel* network_panel_ = nullptr;

    // Server (brazing reporter). The address is edited as protocol + host + port
    // and PERSISTED as the single canonical brazing.base_url — see
    // brazing::split_base_url / compose_base_url. There is no widget holding the
    // whole URL, and no database column per control.
    QCheckBox* brazing_enabled_ = nullptr;
    QComboBox* brazing_scheme_ = nullptr;
    QLineEdit* brazing_host_ = nullptr;
    QLineEdit* brazing_port_ = nullptr;
    QLineEdit* brazing_api_path_ = nullptr;
    QLabel* brazing_endpoint_ = nullptr;  // live "Requests will be sent to: …"
    QLabel* brazing_status_ = nullptr;   // inline "Saved." / rejection reason
};

} // namespace denso::ui
