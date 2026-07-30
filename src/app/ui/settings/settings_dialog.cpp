#include "ui/settings/settings_dialog.h"

#include "brazing/config.h"
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
#include <QStackedWidget>
#include <QVBoxLayout>

#include <vector>

namespace denso::ui {

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
    outer->addWidget(common::hline());
    auto* footer = new QHBoxLayout;
    footer->setSpacing(8);
    auto* reset = new QPushButton(QStringLiteral("Reset to defaults"));
    reset->setProperty("flatText", true);
    connect(reset, &QPushButton::clicked, this,
            [this] { emit reset_defaults_requested(); });
    auto* close_btn = new QPushButton(QStringLiteral("Close"));
    connect(close_btn, &QPushButton::clicked, this, &QDialog::reject);
    auto* apply = new QPushButton(QStringLiteral("Apply"));
    apply->setProperty("gold", true);
    connect(apply, &QPushButton::clicked, this, [this] {
        int w = 1600, h = 900;
        const int id = window_size_->currentData().toInt();
        if (id >= 0) {
            const auto [pw, ph] = settings::PRESETS[static_cast<size_t>(id)];
            w = static_cast<int>(pw);
            h = static_cast<int>(ph);
        }
        emit apply_display_requested(display_mode_->currentData().toInt(), w, h);
        accept();
    });
    footer->addWidget(reset, 0);
    footer->addStretch(1);
    footer->addWidget(close_btn, 0);
    footer->addWidget(apply, 0);
    outer->addLayout(footer);

    nav_->setCurrentRow(0);
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

    connect(display_mode_, &QComboBox::currentIndexChanged, this,
            [this](int) { sync_size_enabled(); });

    // Dark mode lives here too (the old standalone Appearance tab held only this).
    v->addWidget(common::hline());
    auto* dark_row = new QHBoxLayout;
    dark_row->addWidget(new QLabel(QStringLiteral("Dark mode")), 1);
    dark_switch_ = new QCheckBox;
    connect(dark_switch_, &QCheckBox::toggled, this, [this](bool on) {
        if (!suppress_signals_) emit theme_changed(on);
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

    const brazing::BrazingConfig cfg = brazing::load(db_);

    brazing_enabled_ = new QCheckBox(QStringLiteral("Send zone readings to server"));
    brazing_enabled_->setChecked(cfg.enabled);
    v->addWidget(brazing_enabled_);

    auto* url_box = new QVBoxLayout;
    url_box->setSpacing(6);
    url_box->addWidget(common::dim_label(QStringLiteral("Server base URL")));
    brazing_url_ = new QLineEdit(QString::fromStdString(cfg.base_url));
    brazing_url_->setPlaceholderText(QStringLiteral("http://192.168.1.50:8098"));
    url_box->addWidget(brazing_url_);
    v->addLayout(url_box);

    auto* save = new QPushButton(QStringLiteral("Save"));
    save->setProperty("gold", true);
    connect(save, &QPushButton::clicked, this, [this] {
        brazing::BrazingConfig out;
        out.enabled = brazing_enabled_->isChecked();
        out.base_url = brazing_url_->text().trimmed().toStdString();
        brazing::save(db_, out);
    });
    v->addWidget(save, 0, Qt::AlignLeft);
    v->addStretch(1);
    return page;
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
}

void SettingsDialog::set_window_size(uint32_t width, uint32_t height) {
    suppress_signals_ = true;
    rebuild_window_sizes();
    const int idx = settings::preset_index(width, height);
    const int at = window_size_->findData(idx);
    if (at >= 0) window_size_->setCurrentIndex(at);
    else if (window_size_->count() > 0) window_size_->setCurrentIndex(window_size_->count() - 1);
    suppress_signals_ = false;
}

void SettingsDialog::set_theme_dark(bool dark) {
    suppress_signals_ = true;
    dark_switch_->setChecked(dark);
    suppress_signals_ = false;
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
    rebuild_window_sizes();  // re-filter to the current screen each open
    QDialog::showEvent(event);
}

} // namespace denso::ui
