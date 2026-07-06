#include "ui/settings/settings_dialog.h"

#include "brazing/config.h"
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
#include <QScrollArea>
#include <QShowEvent>
#include <QStackedWidget>
#include <QVBoxLayout>

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
    nav_->addItems({QStringLiteral("Appearance"), QStringLiteral("Display"),
                    QStringLiteral("System"), QStringLiteral("Network"),
                    QStringLiteral("Server"), QStringLiteral("About")});
    body->addWidget(nav_, 0);

    stack_ = new QStackedWidget;
    stack_->addWidget(build_appearance());
    stack_->addWidget(build_display());
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
        if (row == 3) {  // Network tab: re-seed cards + refresh status
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
        emit apply_resolution_requested(resolution_index());
        accept();
    });
    footer->addWidget(reset, 0);
    footer->addStretch(1);
    footer->addWidget(close_btn, 0);
    footer->addWidget(apply, 0);
    outer->addLayout(footer);

    nav_->setCurrentRow(0);
}

QWidget* SettingsDialog::build_appearance() {
    auto* page = new QWidget;
    auto* v = new QVBoxLayout(page);
    v->setSpacing(10);
    v->addWidget(common::eyebrow(QStringLiteral("APPEARANCE")));
    auto* row = new QHBoxLayout;
    row->addWidget(new QLabel(QStringLiteral("Dark mode")), 1);
    dark_switch_ = new QCheckBox;
    connect(dark_switch_, &QCheckBox::toggled, this, [this](bool on) {
        if (!suppress_signals_) emit theme_changed(on);
    });
    row->addWidget(dark_switch_, 0);
    v->addLayout(row);
    v->addStretch(1);
    return page;
}

QWidget* SettingsDialog::build_display() {
    auto* page = new QWidget;
    auto* v = new QVBoxLayout(page);
    v->setSpacing(12);
    v->addWidget(common::eyebrow(QStringLiteral("DISPLAY")));

    auto* res_box = new QVBoxLayout;
    res_box->setSpacing(6);
    res_box->addWidget(common::dim_label(QStringLiteral("Resolution")));
    resolution_ = new QComboBox;
    resolution_->addItems({QStringLiteral("800 × 600"), QStringLiteral("1280 × 720"),
                           QStringLiteral("1600 × 900"), QStringLiteral("1920 × 1080")});
    res_box->addWidget(resolution_);
    v->addLayout(res_box);

    auto* fs_row = new QHBoxLayout;
    fs_row->addWidget(new QLabel(QStringLiteral("Fullscreen")), 1);
    fullscreen_switch_ = new QCheckBox;
    connect(fullscreen_switch_, &QCheckBox::toggled, this, [this](bool on) {
        if (!suppress_signals_) emit toggle_fullscreen_requested(on);
    });
    fs_row->addWidget(fullscreen_switch_, 0);
    v->addLayout(fs_row);
    v->addStretch(1);
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

void SettingsDialog::set_resolution_index(int index) {
    suppress_signals_ = true;
    resolution_->setCurrentIndex(index);
    suppress_signals_ = false;
}

void SettingsDialog::set_fullscreen(bool fullscreen) {
    suppress_signals_ = true;
    fullscreen_switch_->setChecked(fullscreen);
    suppress_signals_ = false;
}

void SettingsDialog::set_theme_dark(bool dark) {
    suppress_signals_ = true;
    dark_switch_->setChecked(dark);
    suppress_signals_ = false;
}

int SettingsDialog::resolution_index() const {
    return resolution_->currentIndex();
}

void SettingsDialog::showEvent(QShowEvent* event) {
    nav_->setCurrentRow(0);
    QDialog::showEvent(event);
}

} // namespace denso::ui
