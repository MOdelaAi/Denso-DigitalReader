#include "ui/settings/settings_dialog.h"

#include "brazing/config.h"
#include "brazing/url.h"
#include "settings/settings.h"
#include "ui/common/dialog_chrome.h"
#include "ui/common/form_widgets.h"
#include "ui/settings/network_panel.h"

#include <QCheckBox>
#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QShowEvent>
#include <QSqlDriver>
#include <QStackedWidget>
#include <QStyle>
#include <QVBoxLayout>

#include <vector>

namespace denso::ui {
namespace {

/// Row of "Server" in the left nav. Named so the Backend indicator's "open
/// Server settings" and the validation jump both point at ONE place; the
/// addItems() call below is written against it so the two cannot drift.
constexpr int kServerNavRow = 4;

/// Put one line of feedback on an inline status label. `warn` selects the theme's
/// `QLabel[warn="true"]` rule — colour is never the only carrier, the text always
/// says what happened. An empty message hides the label so a page the operator has
/// not acted on shows nothing.
void set_status(QLabel* label, const QString& message, bool warn) {
    if (!label) return;
    label->setText(message);
    label->setVisible(!message.isEmpty());
    label->setProperty("warn", warn);
    label->setProperty("faint", !warn);
    // Property-driven QSS is not re-evaluated on its own.
    label->style()->unpolish(label);
    label->style()->polish(label);
}

} // namespace

SettingsDialog::SettingsDialog(QSqlDatabase db, QWidget* parent)
    : QDialog(parent), db_(std::move(db)) {
    setWindowTitle(QStringLiteral("Settings"));
    setObjectName(QStringLiteral("dialogPanel"));
    resize(900, 640);

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(24, 24, 24, 24);
    outer->setSpacing(22);

    outer->addLayout(common::dialog_header(this, QStringLiteral("Settings")));

    // ── Body: nav + content ──
    auto* body = new QHBoxLayout;
    body->setSpacing(20);

    nav_ = new QListWidget;
    nav_->setObjectName(QStringLiteral("navList"));
    nav_->setFixedWidth(160);
    nav_->addItems({QStringLiteral("Display"), QStringLiteral("Mode"),
                    QStringLiteral("System"), QStringLiteral("Network"),
                    QStringLiteral("Server"), QStringLiteral("About")});
    Q_ASSERT(nav_->item(kServerNavRow)->text() == QStringLiteral("Server"));
    body->addWidget(nav_, 0);

    stack_ = new QStackedWidget;
    stack_->addWidget(build_display());
    stack_->addWidget(build_mode());
    stack_->addWidget(build_system());
    stack_->addWidget(build_network());
    stack_->addWidget(build_server());
    stack_->addWidget(build_about());

    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setWidget(stack_);
    body->addWidget(scroll, 1);
    outer->addLayout(body, 1);

    connect(nav_, &QListWidget::currentRowChanged, this, [this](int row) {
        if (row < 0) return;
        stack_->setCurrentIndex(row);
        // Network tab: re-seed cards + refresh status. Match by widget identity so
        // the new Mode page (which shifted the numeric indices) can't misfire it.
        if (stack_->widget(row) == network_panel_) {
            network_panel_->on_shown();
        }
    });

    // ── Footer ──
    //
    // EXACTLY ONE primary action. The dialog used to carry a gold "Apply" that
    // committed the display page only, next to a gold "Save" on the Server page
    // that committed brazing only — two primaries with disjoint, invisible
    // scopes. There is now one "Save changes" that owns every page, and a
    // "Cancel" that owns none. "Reset to defaults" stays a flat tertiary action:
    // it is a recovery path, not a competing way to commit the form.
    outer->addWidget(common::hline());
    auto* footer = new QHBoxLayout;
    footer->setSpacing(8);
    auto* reset = new QPushButton(QStringLiteral("Reset to defaults"));
    reset->setObjectName(QStringLiteral("resetDefaultsButton"));
    reset->setProperty("flatText", true);
    connect(reset, &QPushButton::clicked, this,
            [this] { emit reset_defaults_requested(); });
    auto* cancel_btn = new QPushButton(QStringLiteral("Cancel"));
    cancel_btn->setObjectName(QStringLiteral("cancelButton"));
    // reject(), not a bespoke handler: the override below is the ONE discard
    // path, shared with Esc, the header close glyph and the window manager.
    connect(cancel_btn, &QPushButton::clicked, this, &QDialog::reject);
    save_btn_ = new QPushButton(QStringLiteral("Save changes"));
    save_btn_->setObjectName(QStringLiteral("saveChangesButton"));
    save_btn_->setProperty("gold", true);
    connect(save_btn_, &QPushButton::clicked, this, &SettingsDialog::save_changes);
    footer->addWidget(reset, 0);
    footer->addStretch(1);
    footer->addWidget(cancel_btn, 0);
    footer->addWidget(save_btn_, 0);
    outer->addLayout(footer);

    capture_baseline();    // nothing edited yet
    nav_->setCurrentRow(0);
}

SettingsDialog::FormState SettingsDialog::current_form() const {
    FormState f;
    f.display_mode = display_mode_ ? display_mode_->currentData().toInt() : -1;
    f.window_size = window_size_ ? window_size_->currentData().toInt() : -1;
    f.dark = dark_switch_ && dark_switch_->isChecked();
    f.brazing_enabled = brazing_enabled_ && brazing_enabled_->isChecked();
    f.brazing_scheme = brazing_scheme_ ? brazing_scheme_->currentData().toString()
                                       : QString();
    f.brazing_host = brazing_host_ ? brazing_host_->text() : QString();
    f.brazing_port = brazing_port_ ? brazing_port_->text() : QString();
    f.brazing_api_path = brazing_api_path_ ? brazing_api_path_->text() : QString();
    return f;
}

void SettingsDialog::capture_baseline() {
    baseline_ = current_form();
    dirty_ = false;
    sync_save_enabled();
}

void SettingsDialog::recompute_dirty() {
    if (suppress_signals_) {
        return;   // seeding is not an edit; the seeding path re-captures instead
    }
    dirty_ = current_form() != baseline_;
    sync_save_enabled();
}

void SettingsDialog::sync_save_enabled() {
    if (save_btn_) save_btn_->setEnabled(dirty_);
}

void SettingsDialog::set_theme_committer(std::function<bool(bool)> commit) {
    theme_committer_ = std::move(commit);
}

void SettingsDialog::select_server_page() {
    const int row = nav_->count() > kServerNavRow ? kServerNavRow : 0;
    nav_->setCurrentRow(row);
}

void SettingsDialog::save_changes() {
    // ── PHASE 1: validate, then persist EVERYTHING in ONE transaction ────────
    //
    // Every write happens before any apply, and all of them land or none do. Two
    // separate checked saves would leave a window where the Server page is on
    // disk, the display page is not, and the operator has been told the Save
    // failed — a restart would then come up half-configured. So this owns the
    // transaction and the two modules contribute rows to it (SQLite has no
    // nested transactions, which is why *_rows exists at all).
    QSqlDatabase conn = db_;
    const QSqlDriver* driver = conn.driver();
    const bool txn_supported =
        driver != nullptr && driver->hasFeature(QSqlDriver::Transactions);
    if (txn_supported && !conn.transaction()) {
        // The connection is in an unknown state; writing anyway is how a caller
        // ends up told "failed" after some rows have already landed.
        select_server_page();
        set_server_status(QStringLiteral("Could not save: the database is busy. "
                                         "Nothing was changed."),
                          true);
        return;
    }
    const auto abandon = [&](const QString& why) {
        if (txn_supported) conn.rollback();
        select_server_page();   // put the operator where the problem is
        set_server_status(why, true);
    };

    // The Server page is the only one with input that can be wrong. Its own
    // message is more specific than anything here, so on a validation failure it
    // has already written the status line — do not overwrite it.
    if (!save_server_settings()) {
        if (txn_supported) conn.rollback();
        select_server_page();
        return;
    }
    // The theme has been PREVIEWED live since the toggle moved; this persists it.
    // A functor, not a signal, because a signal cannot tell us it failed.
    const bool dark = dark_switch_->isChecked();
    if (theme_committer_ && !theme_committer_(dark)) {
        abandon(QStringLiteral("Could not save the display settings to the "
                               "database. Nothing was changed."));
        return;
    }
    if (txn_supported && !conn.commit()) {
        // SQLite can leave the transaction open when a commit fails on a busy
        // connection, and this handle is shared — close it out explicitly.
        abandon(QStringLiteral("Could not save the settings to the database. "
                               "Nothing was changed."));
        return;
    }

    // ── PHASE 2: runtime effects, only now that every write landed ───────────
    // Backend first: it is the cheapest and the one the operator most likely
    // came here for, and the display transaction below can open a modal.
    emit brazing_config_changed();

    // Apply the theme that was just persisted.
    emit theme_changed(dark);

    // Display last. MainWindow defers this a tick precisely so this dialog is
    // closed before its confirm/revert dialog opens, and it persists the mode
    // only once the operator confirms the window is still usable — apply-then-
    // confirm-then-persist is deliberate here and is NOT the Save/Apply
    // ambiguity this change removes: the operator sees one action.
    int w = 1600, h = 900;
    const int id = window_size_->currentData().toInt();
    if (id >= 0) {
        const auto [pw, ph] = settings::PRESETS[static_cast<size_t>(id)];
        w = static_cast<int>(pw);
        h = static_cast<int>(ph);
    }
    emit apply_display_requested(display_mode_->currentData().toInt(), w, h);

    // The form now IS what is stored, so it becomes the new reference.
    capture_baseline();
    accept();
}

void SettingsDialog::reject() {
    // The ONE discard path. Cancel, Esc, the header's close glyph and the window
    // manager's close box all arrive here, which is why the restoration lives in
    // the override and not in the button's handler: wiring only the button would
    // leave an Esc-dismissed dialog with its theme preview still applied and
    // nothing persisted — the running app disagreeing with the database.
    //
    // Undo that preview; everything else on this dialog is staged in widgets and
    // dies with the edit. Nothing is written and no commit signal is emitted.
    if (dark_switch_ && dark_switch_->isChecked() != entry_dark_) {
        emit theme_preview_requested(entry_dark_);
        // Put the widget back in step with the theme now in effect, so re-opening
        // the dialog cannot show a toggle that disagrees with the running app.
        const bool prev = suppress_signals_;
        suppress_signals_ = true;
        dark_switch_->setChecked(entry_dark_);
        suppress_signals_ = prev;
    }
    // Discarded: whatever is on screen is no longer an unsaved edit. The next
    // open re-seeds from the database and re-captures anyway.
    capture_baseline();
    QDialog::reject();
}

void SettingsDialog::close_after_mode_switch() {
    if (!isVisible()) {
        return;   // nothing to close; see the header
    }
    // reject(), NOT a second discard implementation. Everything this needs — undo
    // the theme preview, re-capture the baseline, write nothing, emit no commit —
    // is already the contract of the dialog's one discard path, and a private copy
    // of it here would be a second place for those rules to drift. The NAME at the
    // call site carries the intent that reject() alone would not.
    reject();
}

QWidget* SettingsDialog::build_display() {
    auto* page = new QWidget;
    auto* v = new QVBoxLayout(page);
    v->setSpacing(12);
    v->addWidget(common::eyebrow(QStringLiteral("DISPLAY")));

    auto* mode_box = new QVBoxLayout;
    mode_box->setSpacing(6);
    mode_box->addWidget(common::dim_label(QStringLiteral("Display mode")));
    display_mode_ = new QComboBox;
    display_mode_->addItem(QStringLiteral("Windowed"),
                           static_cast<int>(settings::DisplayMode::Windowed));
    display_mode_->addItem(QStringLiteral("Borderless"),
                           static_cast<int>(settings::DisplayMode::Borderless));
    display_mode_->addItem(QStringLiteral("Fullscreen"),
                           static_cast<int>(settings::DisplayMode::Fullscreen));
    mode_box->addWidget(display_mode_);
    v->addLayout(mode_box);

    auto* size_box = new QVBoxLayout;
    size_box->setSpacing(6);
    size_box->addWidget(common::dim_label(QStringLiteral("Window size")));
    window_size_ = new QComboBox;
    size_box->addWidget(window_size_);
    window_size_hint_ = common::dim_label(
        QStringLiteral("Does not change the monitor resolution."));
    window_size_hint_->setProperty("faint", true);
    size_box->addWidget(window_size_hint_);
    v->addLayout(size_box);

    connect(display_mode_, &QComboBox::currentIndexChanged, this, [this](int) {
        sync_size_enabled();
        recompute_dirty();
    });
    connect(window_size_, &QComboBox::currentIndexChanged, this,
            [this](int) { recompute_dirty(); });

    // Dark mode lives here too (the old standalone Appearance tab held only this).
    v->addWidget(common::hline());
    auto* dark_row = new QHBoxLayout;
    dark_row->addWidget(new QLabel(QStringLiteral("Dark mode")), 1);
    dark_switch_ = new QCheckBox;
    dark_switch_->setObjectName(QStringLiteral("darkModeSwitch"));
    connect(dark_switch_, &QCheckBox::toggled, this, [this](bool on) {
        if (suppress_signals_) return;
        // PREVIEW only. Committing here is what made the old dialog's Cancel a
        // lie: the theme was persisted the instant the switch moved, so there was
        // nothing left to cancel. Save changes commits it; Cancel restores
        // entry_dark_.
        emit theme_preview_requested(on);
        recompute_dirty();
    });
    dark_row->addWidget(dark_switch_, 0);
    v->addLayout(dark_row);

    v->addStretch(1);
    return page;
}

QWidget* SettingsDialog::build_mode() {
    auto* page = new QWidget;
    auto* v = new QVBoxLayout(page);
    v->setSpacing(12);
    v->addWidget(common::eyebrow(QStringLiteral("TARGET MODE")));

    auto* box = new QVBoxLayout;
    box->setSpacing(6);
    box->addWidget(common::dim_label(QStringLiteral("Operating mode")));
    mode_select_ = new QComboBox;
    mode_select_->setObjectName(QStringLiteral("modeSelect"));
    mode_select_->addItem(QStringLiteral("Digital Number Reader"),
                          static_cast<int>(mode::TargetMode::DigitReader));
    mode_select_->addItem(QStringLiteral("Floating Ball Leveler"),
                          static_cast<int>(mode::TargetMode::BallLeveler));
    box->addWidget(mode_select_);
    v->addLayout(box);

    auto* note = common::dim_label(QStringLiteral(
        "Switching keeps your camera connections and both modes' setup, and "
        "turns off server reporting. Processing stops while the new mode is "
        "prepared."));
    note->setWordWrap(true);
    note->setProperty("faint", true);
    v->addWidget(note);

    switch_mode_btn_ = new QPushButton(QStringLiteral("Switch"));
    switch_mode_btn_->setObjectName(QStringLiteral("switchModeButton"));
    // Intent ONLY (spec §5/§7). MainWindow owns ModeConfirmDialog + the
    // transaction. Enabled only when the selected mode differs from current,
    // so a click can never request a switch to the already-active mode.
    connect(switch_mode_btn_, &QPushButton::clicked, this, [this] {
        const mode::TargetMode sel =
            mode::from_index(mode_select_->currentData().toInt());
        emit switch_mode_requested(static_cast<int>(sel));
    });
    v->addWidget(switch_mode_btn_, 0, Qt::AlignLeft);

    // A bare selector change never emits a switch — it only re-evaluates the
    // button's enabled state.
    connect(mode_select_, &QComboBox::currentIndexChanged, this,
            [this](int) { sync_mode_button(); });

    v->addStretch(1);
    sync_mode_button();  // seed disabled until a different mode is selected
    return page;
}

QWidget* SettingsDialog::build_system() {
    auto* page = new QWidget;
    auto* v = new QVBoxLayout(page);
    v->setSpacing(8);
    v->addWidget(common::eyebrow(QStringLiteral("SYSTEM")));
    v->addWidget(common::spec_row(QStringLiteral("OS"), &hw_os_));
    v->addWidget(common::spec_row(QStringLiteral("Device"), &hw_device_));
    v->addWidget(common::spec_row(QStringLiteral("RAM"), &hw_ram_));
    v->addWidget(common::spec_row(QStringLiteral("Storage"), &hw_storage_));
    v->addStretch(1);
    return page;
}

QWidget* SettingsDialog::build_network() {
    network_panel_ = new NetworkPanel(db_);
    return network_panel_;
}

QWidget* SettingsDialog::build_server() {
    auto* page = new QWidget;
    auto* v = new QVBoxLayout(page);
    v->setSpacing(12);
    v->addWidget(common::eyebrow(QStringLiteral("SERVER")));

    brazing_enabled_ = new QCheckBox(QStringLiteral("Send zone readings to server"));
    brazing_enabled_->setObjectName(QStringLiteral("brazingEnabled"));
    connect(brazing_enabled_, &QCheckBox::toggled, this,
            [this](bool) { recompute_dirty(); });
    v->addWidget(brazing_enabled_);

    // The server address is edited as the three things an operator is actually
    // told — protocol, address, port — rather than as one URL they have to
    // assemble. The DATABASE is unchanged: these three compose back into the one
    // canonical `brazing.base_url` on save (brazing::compose_base_url) and are
    // split out of it on load (brazing::split_base_url). No new column, no
    // migration, and no second place the server address lives.
    //
    // Every edit here runs the same two slots: re-derive dirty, and re-derive the
    // effective endpoint. textChanged, not textEdited: setText() during seeding
    // runs under suppress_signals_, which recompute_dirty() already ignores, and
    // this way a programmatic edit from anywhere else still arms the primary
    // action.
    const auto on_base_edit = [this] {
        recompute_dirty();
        update_endpoint_preview();
    };

    auto* proto_box = new QVBoxLayout;
    proto_box->setSpacing(6);
    proto_box->addWidget(common::dim_label(QStringLiteral("Protocol")));
    brazing_scheme_ = new QComboBox;
    brazing_scheme_->setObjectName(QStringLiteral("brazingScheme"));
    // The DATA is the canonical lower-case scheme; the visible text is the
    // upper-case spelling an operator reads on a datasheet. Nothing downstream
    // reads the label.
    brazing_scheme_->addItem(QStringLiteral("HTTP"), QStringLiteral("http"));
    brazing_scheme_->addItem(QStringLiteral("HTTPS"), QStringLiteral("https"));
    connect(brazing_scheme_, &QComboBox::currentIndexChanged, this,
            [on_base_edit](int) { on_base_edit(); });
    proto_box->addWidget(brazing_scheme_);
    v->addLayout(proto_box);

    auto* host_box = new QVBoxLayout;
    host_box->setSpacing(6);
    host_box->addWidget(common::dim_label(QStringLiteral("Server address")));
    brazing_host_ = new QLineEdit;
    brazing_host_->setObjectName(QStringLiteral("brazingHost"));
    // "e.g." ON PURPOSE — see update_endpoint_preview(). A placeholder that is
    // itself a usable value reads as a filled field on the panel, which is
    // exactly how an empty address came to look configured.
    // Names BOTH accepted shapes, so an operator given a host name rather than an
    // IP can see this field takes it. Still unmistakably an example — the "e.g."
    // and the two alternatives mean it can never read as a filled-in value, and
    // it is not one: the spaces alone make it something compose_base_url refuses.
    brazing_host_->setPlaceholderText(
        QStringLiteral("e.g. 192.168.1.112 or server.local"));
    connect(brazing_host_, &QLineEdit::textChanged, this,
            [on_base_edit](const QString&) { on_base_edit(); });
    host_box->addWidget(brazing_host_);
    v->addLayout(host_box);

    auto* port_box = new QVBoxLayout;
    port_box->setSpacing(6);
    port_box->addWidget(common::dim_label(QStringLiteral("Port")));
    brazing_port_ = new QLineEdit;
    brazing_port_->setObjectName(QStringLiteral("brazingPort"));
    // Blank is legitimate — it means the protocol's default port, and a stored
    // "https://server.example.com" must give itself back unchanged. The "e.g."
    // and the "(optional)" together carry both halves of that: what a port looks
    // like, and that it may be left out. Still not a value — "e.g. 8080
    // (optional)" is not digits, so it is something compose_base_url refuses.
    brazing_port_->setPlaceholderText(QStringLiteral("e.g. 8080 (optional)"));
    connect(brazing_port_, &QLineEdit::textChanged, this,
            [on_base_edit](const QString&) { on_base_edit(); });
    port_box->addWidget(brazing_port_);
    // A placeholder disappears the moment the operator types, so what blank
    // actually DOES is stated in a line that stays put.
    auto* port_hint = common::dim_label(
        QStringLiteral("Leave blank for HTTP 80 or HTTPS 443."));
    port_hint->setObjectName(QStringLiteral("brazingPortHint"));
    port_hint->setProperty("faint", true);
    port_box->addWidget(port_hint);
    v->addLayout(port_box);

    // The reporting API path. Deliberately NOT called the "brazing" path: the
    // same endpoint carries Digital Number Reader and Floating Ball Leveler
    // readings, so the label names the job, not one of the two modes.
    auto* path_box = new QVBoxLayout;
    path_box->setSpacing(6);
    path_box->addWidget(common::dim_label(QStringLiteral("Reporting API path")));
    brazing_api_path_ = new QLineEdit;
    brazing_api_path_->setObjectName(QStringLiteral("brazingApiPath"));
    // This placeholder IS a usable value, and that is honest here and only here:
    // leaving this field blank really does resolve to exactly this path, so what
    // is shown is what would be used. Contrast the address field above.
    brazing_api_path_->setPlaceholderText(
        QString::fromLatin1(brazing::kDefaultApiPath));
    connect(brazing_api_path_, &QLineEdit::textChanged, this,
            [this](const QString&) {
                recompute_dirty();
                update_endpoint_preview();
            });
    path_box->addWidget(brazing_api_path_);
    v->addLayout(path_box);

    // Replaces the old fixed sentence naming /api/brazing/update. The endpoint is
    // no longer fixed, so stating it as a constant would be a lie the moment the
    // path is edited; this shows what the application will ACTUALLY post to,
    // recomputed on every keystroke in either field through the same function the
    // transport composes with.
    auto* endpoint_box = new QVBoxLayout;
    endpoint_box->setSpacing(6);
    endpoint_box->addWidget(common::dim_label(QStringLiteral("Effective endpoint")));
    brazing_endpoint_ = new QLabel;
    brazing_endpoint_->setObjectName(QStringLiteral("brazingEndpoint"));
    brazing_endpoint_->setWordWrap(true);
    brazing_endpoint_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    brazing_endpoint_->setProperty("faint", true);
    endpoint_box->addWidget(brazing_endpoint_);
    v->addLayout(endpoint_box);

    // NO page-level Save button. This page used to carry its own gold Save while
    // the footer carried a gold Apply that meant something else entirely; the
    // footer's "Save changes" now owns this page too. What stays is the inline
    // status line below, because a validation message belongs beside the field
    // that failed, not in a dialog footer.
    brazing_status_ = new QLabel;
    brazing_status_->setObjectName(QStringLiteral("brazingStatus"));
    brazing_status_->setWordWrap(true);
    v->addWidget(brazing_status_);

    v->addStretch(1);
    reload_server_page();   // seed from the database
    return page;
}

void SettingsDialog::reload_server_page() {
    // The ONE place the Server page is filled in, used by construction, by every
    // open (showEvent) and by refresh_backend_state(). It READS the authoritative
    // configuration and writes nothing back: the checkbox and the field are an
    // editor view of that state, never a second copy of it.
    //
    // Seeding, not editing: without the guard, merely opening Settings — or a
    // passive re-sync — would arm "Save changes" via the checkbox/line-edit
    // change signals.
    const bool prev = suppress_signals_;
    suppress_signals_ = true;
    const brazing::BrazingConfig cfg = brazing::load(db_);
    brazing_enabled_->setChecked(cfg.enabled);
    // The base URL is preserved by a mode switch on purpose, so re-seeding shows
    // the operator the address they still have — only the enabled flag moved.
    // Split into the three controls by the shared authority, so what the form
    // shows is a VIEW of the one stored string rather than a second copy of it.
    const brazing::BaseUrlParts parts = brazing::split_base_url(cfg.base_url);
    const int scheme_row =
        brazing_scheme_->findData(parts.https ? QStringLiteral("https")
                                              : QStringLiteral("http"));
    brazing_scheme_->setCurrentIndex(scheme_row >= 0 ? scheme_row : 0);
    brazing_host_->setText(QString::fromStdString(parts.host));
    brazing_port_->setText(QString::fromStdString(parts.port));
    // Never blank: brazing::load() resolves a missing or blank row to the shipped
    // default, so an installation that predates this setting opens the page
    // showing exactly the endpoint it has always posted to.
    brazing_api_path_->setText(QString::fromStdString(cfg.api_path));
    suppress_signals_ = prev;
    common::mark_invalid(brazing_host_, false);
    common::mark_invalid(brazing_port_, false);
    common::mark_invalid(brazing_api_path_, false);
    // set_server_status re-derives the endpoint preview, so clearing the status
    // here is also what repaints the preview for the freshly seeded form.
    set_server_status(QString(), false);
}

void SettingsDialog::mark_base_fields_invalid(bool invalid) {
    common::mark_invalid(brazing_host_, invalid);
    common::mark_invalid(brazing_port_, invalid);
}

void SettingsDialog::mark_offending_base_field(const std::string& api_path) {
    // Redden the field that is actually wrong, WITHOUT restating the port rule
    // here: ask the shared composer the same question again with the port
    // removed. If it then succeeds, the port was the problem; otherwise the
    // address was. One authority, asked twice, instead of two copies of a rule
    // that could drift apart.
    //
    // The empty-host case must not go through that probe: clearing the port
    // turns "a port with no address" into the legitimate unset state, which
    // would blame the port for a missing address.
    brazing::BaseUrlParts parts = current_base_parts();
    if (parts.host.empty()) {
        common::mark_invalid(brazing_host_, true);
        common::mark_invalid(brazing_port_, false);
        return;
    }
    parts.port.clear();
    const bool port_is_the_problem = brazing::compose_base_url(parts, api_path).ok;
    common::mark_invalid(brazing_port_, port_is_the_problem);
    common::mark_invalid(brazing_host_, !port_is_the_problem);
}

brazing::BaseUrlParts SettingsDialog::current_base_parts() const {
    brazing::BaseUrlParts parts;
    parts.https = brazing_scheme_ && brazing_scheme_->currentData().toString() ==
                                         QStringLiteral("https");
    parts.host = brazing_host_ ? brazing_host_->text().toStdString() : std::string();
    parts.port = brazing_port_ ? brazing_port_->text().toStdString() : std::string();
    return parts;
}

void SettingsDialog::set_server_status(const QString& message, bool warn) {
    // The ONE writer of the Server page's status line, so the endpoint preview is
    // re-derived every time that line moves — which is what lets the preview
    // suppress a message the status line is already carrying.
    set_status(brazing_status_, message, warn);
    update_endpoint_preview();
}

void SettingsDialog::set_endpoint_preview(const QString& text) {
    if (!brazing_endpoint_) {
        return;
    }
    // Never say the same thing twice. A refused Save puts the reason on the
    // status line, and the preview has been showing that same reason live since
    // the keystroke that caused it — rendering both stacked one above the other
    // reads as two problems. The preview steps aside for the duration; the
    // actionable line stays, because that is the one attached to the action the
    // operator just took.
    //
    // Compared against the status TEXT rather than tracked with a flag: the two
    // labels are derived from the same validation result, so equality is the
    // honest test of "this is a duplicate" and it cannot fall out of sync.
    //
    // Non-empty text, NOT isVisible(): set_status hides the label exactly when
    // its message is empty, so the text alone already says whether the line is
    // showing — while isVisible() also reports false for every child of a dialog
    // that has not been shown yet, which would silently disable the suppression
    // in precisely the case a test constructs.
    if (brazing_status_ && !brazing_status_->text().isEmpty() &&
        brazing_status_->text() == text) {
        brazing_endpoint_->setText(QStringLiteral("—"));
        return;
    }
    brazing_endpoint_->setText(text);
}

void SettingsDialog::update_endpoint_preview() {
    if (!brazing_endpoint_) {
        return;
    }
    const QString path = brazing_api_path_ ? brazing_api_path_->text() : QString();
    const brazing::BaseUrlParts parts = current_base_parts();

    // Composed by the SAME functions the save path and the transport use, so the
    // preview cannot promise an endpoint the client would not build. The three
    // controls are joined by brazing::compose_base_url — never by string
    // concatenation here, which is how a preview starts drifting from the runtime.
    const brazing::ApiPathResult api = brazing::normalize_api_path(path.toStdString());
    const brazing::BaseUrlResult base =
        brazing::compose_base_url(parts, api.ok ? api.api_path
                                                : std::string(brazing::kDefaultApiPath));
    if (api.ok && base.ok && !base.base_url.empty()) {
        set_endpoint_preview(
            QString::fromStdString(brazing::endpoint_url(base.base_url, api.api_path)));
        return;
    }

    // No endpoint. Say WHICH field is missing or wrong, because a dash on its own
    // leaves the operator guessing. Order matters: report the address before the
    // path, because an unusable address is the more common and more confusing
    // state, and the address is the field they were most likely just editing.
    if (!base.ok) {
        set_endpoint_preview(QString::fromStdString(base.error));
        return;
    }
    if (!api.ok) {
        set_endpoint_preview(QString::fromStdString(api.error));
        return;
    }
    // base.ok with an empty base_url is the "unset" state — the out-of-the-box
    // condition, not an error. Naming the FIELD is what the old wording failed to
    // do: "No server base URL" pointed at a control the operator could not see,
    // while a greyed placeholder made the address box look filled in.
    set_endpoint_preview(
        QStringLiteral("Enter the server address — nothing will be sent yet."));
}

void SettingsDialog::refresh_backend_state() {
    // Re-seed the widgets from the authority — see the header for why this stays
    // passive.
    reload_server_page();

    // …then RETIRE the Server page's contribution to the dirty comparison, and
    // only that. The operator's unsaved tick has just been overwritten by a
    // committed switch, so it no longer exists and must not keep "Save changes"
    // armed: pressing it would run the whole persist-and-apply sequence — up to
    // and including a display confirm/revert — for a form that already matches
    // what is stored.
    //
    // Rebasing ONLY these two fields is what keeps an unsaved edit on another
    // page (a display mode, the theme) armed, which a blanket `dirty_ = false`
    // would silently throw away. This is precisely why dirty is a comparison
    // against baseline_ rather than a sticky flag.
    baseline_.brazing_enabled = brazing_enabled_->isChecked();
    baseline_.brazing_scheme = brazing_scheme_->currentData().toString();
    baseline_.brazing_host = brazing_host_->text();
    baseline_.brazing_port = brazing_port_->text();
    baseline_.brazing_api_path = brazing_api_path_->text();
    dirty_ = current_form() != baseline_;
    sync_save_enabled();
}

bool SettingsDialog::save_server_settings() {
    // ONE normalization authority, shared with the transport and the grid gate —
    // so the dialog can never accept an address the client would treat as
    // something else. Whitespace, a trailing slash and a pasted full endpoint are
    // canonicalized; any other path is REJECTED rather than quietly rewritten.
    //
    // The PATH first, because the base URL's paste tolerance is defined against
    // the configured endpoint: an operator who pastes the address they were
    // actually given must get it stripped, not a refusal naming a path their
    // server does not expose.
    const brazing::ApiPathResult api =
        brazing::normalize_api_path(brazing_api_path_->text().toStdString());
    if (!api.ok) {
        mark_base_fields_invalid(false);
        common::mark_invalid(brazing_api_path_, true);
        set_server_status(QString::fromStdString(api.error), true);
        return false;
    }
    // The three controls are joined by the shared composer, which ends in
    // normalize_base_url — so the decomposed editor persists exactly the
    // canonical string the free-text field used to, and cannot accept an address
    // that field would have refused.
    const brazing::BaseUrlResult url =
        brazing::compose_base_url(current_base_parts(), api.api_path);
    if (!url.ok) {
        common::mark_invalid(brazing_api_path_, false);
        mark_offending_base_field(api.api_path);
        set_server_status(QString::fromStdString(url.error), true);
        return false;
    }

    brazing::BrazingConfig out;
    out.enabled = brazing_enabled_->isChecked();
    out.base_url = url.base_url;
    out.api_path = api.api_path;

    // Reporting cannot be turned ON without somewhere to report to. Refusing here
    // is what keeps "Saved" honest — the runtime would silently build no sender.
    if (out.enabled && out.base_url.empty()) {
        common::mark_invalid(brazing_api_path_, false);
        // Mark the ADDRESS specifically, and name it in the message. The old
        // wording said "base URL" — a control that no longer exists and, when it
        // did, showed a greyed placeholder that read as a filled-in value. An
        // operator cannot act on a message that names neither the field nor the
        // thing that is missing.
        common::mark_invalid(brazing_host_, true);
        set_server_status(QStringLiteral("Enter the server address before enabling "
                                  "reporting."),
                   true);
        return false;
    }

    // save_ROWS: save_changes() owns the transaction, so this must not open one
    // of its own — SQLite has no nested transactions, and a second BEGIN here
    // would fail and take an otherwise-good Save down with it.
    if (!brazing::save_rows(db_, out)) {
        mark_base_fields_invalid(false);
        common::mark_invalid(brazing_api_path_, false);
        set_server_status(QStringLiteral("Could not save the settings to the database. "
                                  "Reporting was not reconfigured."),
                   true);
        return false;
    }

    // Show the operator what was actually stored. Silently persisting something
    // other than what the field displays is how the doubled-path defect stayed
    // invisible for so long.
    mark_base_fields_invalid(false);
    common::mark_invalid(brazing_api_path_, false);
    const bool prev = suppress_signals_;
    suppress_signals_ = true;
    // Re-split what was actually STORED back into the three controls, rather than
    // leaving whatever was typed. Same reason the free-text field used to re-show
    // the canonical base: silently persisting something other than what the form
    // displays is how the doubled-path defect stayed invisible for so long. Here
    // it also shows the operator that "192.168.001.112" or "EXAMPLE.com" was
    // canonicalized on the way in.
    const brazing::BaseUrlParts stored = brazing::split_base_url(out.base_url);
    const int scheme_row =
        brazing_scheme_->findData(stored.https ? QStringLiteral("https")
                                               : QStringLiteral("http"));
    brazing_scheme_->setCurrentIndex(scheme_row >= 0 ? scheme_row : 0);
    brazing_host_->setText(QString::fromStdString(stored.host));
    brazing_port_->setText(QString::fromStdString(stored.port));
    // Same reason as the base URL, and the one that makes "a blank path means the
    // default" honest rather than silent: a cleared field comes back showing
    // /api/brazing/update, which is what was stored and what the preview above
    // has been showing since the moment it was cleared.
    brazing_api_path_->setText(QString::fromStdString(out.api_path));
    suppress_signals_ = prev;
    update_endpoint_preview();
    set_server_status(out.enabled ? QStringLiteral("Saved. Reporting is active.")
                           : QStringLiteral("Saved. Reporting is off."),
               false);
    return true;
}

QWidget* SettingsDialog::build_about() {
    auto* page = new QWidget;
    auto* v = new QVBoxLayout(page);
    v->setSpacing(6);
    v->addWidget(common::eyebrow(QStringLiteral("ABOUT")));
    auto* row = new QHBoxLayout;
    row->addWidget(common::dim_label(QStringLiteral("Denso Digital Reader")), 1);
    about_version_ = new QLabel;
    about_version_->setProperty("faint", true);
    row->addWidget(about_version_, 0);
    v->addLayout(row);
    v->addStretch(1);
    return page;
}

// ── Startup seeding ──────────────────────────────────────────────────────────

void SettingsDialog::set_app_version(const QString& version) {
    about_version_->setText(QStringLiteral("v%1").arg(version));
}

void SettingsDialog::set_hardware(const QString& os, const QString& device,
                                  const QString& ram, const QString& storage) {
    hw_os_->setText(os);
    hw_device_->setText(device);
    hw_ram_->setText(ram);
    hw_storage_->setText(storage);
}

void SettingsDialog::rebuild_window_sizes() {
    const QScreen* scr = screen();
    const QRect avail = scr ? scr->availableGeometry() : QRect(0, 0, 1920, 1080);
    // Rough frame overhead; MainWindow::resize_within_screen does the final clamp.
    const std::vector<int> fit = settings::fitting_presets(
        static_cast<uint32_t>(avail.width()), static_cast<uint32_t>(avail.height()), 40, 80);

    const int prev_id = window_size_->count() ? window_size_->currentData().toInt() : -1;
    window_size_->clear();
    for (int idx : fit) {
        const auto [w, h] = settings::PRESETS[static_cast<size_t>(idx)];
        window_size_->addItem(QStringLiteral("%1 × %2").arg(w).arg(h), idx);
    }
    if (window_size_->count() == 0) {  // nothing fits — show a disabled placeholder
        window_size_->addItem(QStringLiteral("(screen too small)"), -1);
    }
    const int restore = window_size_->findData(prev_id);
    if (restore >= 0) window_size_->setCurrentIndex(restore);
}

void SettingsDialog::sync_size_enabled() {
    window_size_->setEnabled(staged_mode() == settings::DisplayMode::Windowed);
}

settings::DisplayMode SettingsDialog::staged_mode() const {
    return static_cast<settings::DisplayMode>(display_mode_->currentData().toInt());
}

void SettingsDialog::set_display_mode(settings::DisplayMode mode) {
    suppress_signals_ = true;
    const int i = display_mode_->findData(static_cast<int>(mode));
    if (i >= 0) display_mode_->setCurrentIndex(i);
    sync_size_enabled();
    suppress_signals_ = false;
    // Seeded values ARE the reference: what the window just pushed in is, by
    // definition, what is stored.
    capture_baseline();
}

void SettingsDialog::set_window_size(uint32_t width, uint32_t height) {
    suppress_signals_ = true;
    rebuild_window_sizes();
    const int idx = settings::preset_index(width, height);
    const int at = window_size_->findData(idx);
    if (at >= 0) window_size_->setCurrentIndex(at);
    else if (window_size_->count() > 0) window_size_->setCurrentIndex(window_size_->count() - 1);
    suppress_signals_ = false;
    capture_baseline();
}

void SettingsDialog::set_theme_dark(bool dark) {
    suppress_signals_ = true;
    dark_switch_->setChecked(dark);
    suppress_signals_ = false;
    capture_baseline();
    // The theme Cancel restores. Recorded HERE because this is the one place
    // the window tells the dialog what theme is actually committed — deriving it
    // from the checkbox later would just re-read whatever the operator changed it
    // to.
    entry_dark_ = dark;
}

void SettingsDialog::set_current_mode(mode::TargetMode mode) {
    current_mode_ = mode;
    // Seed the selector to the current mode. currentIndexChanged only re-syncs the
    // button (no emit), and switch_mode_requested is fired solely by the button's
    // clicked handler — so seeding never emits a switch intent.
    const int i = mode_select_->findData(static_cast<int>(mode));
    if (i >= 0) mode_select_->setCurrentIndex(i);
    sync_mode_button();  // selected now equals current → button disabled
}

void SettingsDialog::sync_mode_button() {
    if (!mode_select_ || !switch_mode_btn_) return;
    const mode::TargetMode selected =
        mode::from_index(mode_select_->currentData().toInt());
    switch_mode_btn_->setEnabled(selected != current_mode_);
}

void SettingsDialog::showEvent(QShowEvent* event) {
    nav_->setCurrentRow(0);
    const bool prev = suppress_signals_;
    suppress_signals_ = true;
    rebuild_window_sizes();  // re-filter to the current screen each open
    suppress_signals_ = prev;
    // The dialog is created once and reused, so the Server page must re-read the
    // database rather than keep whatever was on screen last time. A mode switch
    // writes brazing.enabled = 0 behind this dialog's back; without this re-seed
    // the operator would reopen Settings to a stale ticked box.
    reload_server_page();
    // A freshly opened dialog has no unsaved edits, whatever the last visit left
    // behind — so the primary action starts disabled and only the operator can
    // arm it.
    capture_baseline();
    QDialog::showEvent(event);
}

} // namespace denso::ui
