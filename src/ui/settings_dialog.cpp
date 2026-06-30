#include "ui/settings_dialog.h"

#include "network/backend.h"
#include "network/model.h"
#include "network/repo.h"
#include "ui/convert.h"
#include "ui/netcard.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFont>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QScrollArea>
#include <QShowEvent>
#include <QStackedWidget>
#include <QThread>
#include <QVBoxLayout>

#include <exception>
#include <optional>
#include <vector>

namespace denso::ui {
namespace {

QLabel* eyebrow(const QString& text) {
    auto* l = new QLabel(text);
    l->setProperty("faint", true);
    QFont f = l->font();
    f.setBold(true);
    f.setPointSizeF(f.pointSizeF() - 1.0);
    f.setLetterSpacing(QFont::AbsoluteSpacing, 1.5);
    l->setFont(f);
    return l;
}

QLabel* dim_label(const QString& text) {
    auto* l = new QLabel(text);
    l->setProperty("dim", true);
    return l;
}

QWidget* spec_row(const QString& label, QLabel** value_out) {
    auto* w = new QWidget;
    auto* row = new QHBoxLayout(w);
    row->setContentsMargins(0, 0, 0, 0);
    row->addWidget(dim_label(label), 1);
    auto* v = new QLabel;
    row->addWidget(v, 0);
    *value_out = v;
    return w;
}

QFrame* hline() {
    auto* line = new QFrame;
    line->setFrameShape(QFrame::HLine);
    line->setFixedHeight(1);
    return line;
}

/// Saved config for `iface`, or a DHCP default when none is stored yet — the
/// Qt port of the wiring's `load_config_or_default`.
NetConfigUi load_config_or_default(const QSqlDatabase& db, const std::string& iface) {
    std::optional<network::NetConfig> stored = network::load(db, iface);
    if (stored) return to_ui_config(*stored);
    network::NetConfig def;
    def.iface = iface;
    def.mode = "dhcp";
    return to_ui_config(def);
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

    // ── Header ──
    auto* header = new QVBoxLayout;
    header->setSpacing(10);
    auto* title_row = new QHBoxLayout;
    auto* title = new QLabel(QStringLiteral("Settings"));
    QFont tf = title->font();
    tf.setBold(true);
    tf.setPointSizeF(tf.pointSizeF() + 6.0);
    title->setFont(tf);
    auto* close_glyph = new QPushButton(QStringLiteral("✕"));
    close_glyph->setProperty("flatText", true);
    close_glyph->setFixedSize(28, 28);
    connect(close_glyph, &QPushButton::clicked, this, &QDialog::reject);
    title_row->addWidget(title, 1);
    title_row->addWidget(close_glyph, 0);
    header->addLayout(title_row);
    auto* underline = new QFrame;
    underline->setObjectName(QStringLiteral("goldUnderline"));
    underline->setFixedSize(48, 3);
    header->addWidget(underline, 0, Qt::AlignLeft);
    outer->addLayout(header);

    // ── Body: nav + content ──
    auto* body = new QHBoxLayout;
    body->setSpacing(20);

    nav_ = new QListWidget;
    nav_->setObjectName(QStringLiteral("navList"));
    nav_->setFixedWidth(160);
    nav_->addItems({QStringLiteral("Appearance"), QStringLiteral("Display"),
                    QStringLiteral("System"), QStringLiteral("Network"),
                    QStringLiteral("About")});
    body->addWidget(nav_, 0);

    stack_ = new QStackedWidget;
    stack_->addWidget(build_appearance());
    stack_->addWidget(build_display());
    stack_->addWidget(build_system());
    stack_->addWidget(build_network());
    stack_->addWidget(build_about());

    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setWidget(stack_);
    body->addWidget(scroll, 1);
    outer->addLayout(body, 1);

    connect(nav_, &QListWidget::currentRowChanged, this, [this](int row) {
        if (row < 0) return;
        stack_->setCurrentIndex(row);
        if (row == 3) {
            // Entering the Network tab re-creates the Slint NetCards, whose
            // init re-seeds the editors from the saved config — reproduce that
            // by re-seeding here, then refresh status (the Slint nav callback).
            eth_card_->set_config(eth_config_);
            wifi_card_->set_config(wifi_config_);
            refresh_network();
        }
    });

    // ── Footer ──
    outer->addWidget(hline());
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
    v->addWidget(eyebrow(QStringLiteral("APPEARANCE")));
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
    v->addWidget(eyebrow(QStringLiteral("DISPLAY")));

    auto* res_box = new QVBoxLayout;
    res_box->setSpacing(6);
    res_box->addWidget(dim_label(QStringLiteral("Resolution")));
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
    v->addWidget(eyebrow(QStringLiteral("SYSTEM")));
    v->addWidget(spec_row(QStringLiteral("OS"), &hw_os_));
    v->addWidget(spec_row(QStringLiteral("Device"), &hw_device_));
    v->addWidget(spec_row(QStringLiteral("RAM"), &hw_ram_));
    v->addWidget(spec_row(QStringLiteral("Storage"), &hw_storage_));
    v->addStretch(1);
    return page;
}

QWidget* SettingsDialog::build_network() {
    auto* page = new QWidget;
    auto* v = new QVBoxLayout(page);
    v->setSpacing(12);

    auto* head = new QHBoxLayout;
    head->addWidget(eyebrow(QStringLiteral("NETWORK")), 1);
    refresh_btn_ = new QPushButton(QStringLiteral("Refresh"));
    refresh_btn_->setProperty("flatText", true);
    connect(refresh_btn_, &QPushButton::clicked, this, [this] { refresh_network(); });
    head->addWidget(refresh_btn_, 0);
    v->addLayout(head);

    eth_config_ = load_config_or_default(db_, "ethernet");
    eth_card_ = new NetCard(QStringLiteral("Ethernet"), "ethernet", false, eth_config_);
    eth_card_->on_apply = [this](const std::string& iface, const NetConfigUi& ui) {
        apply_net_config(iface, ui);
    };
    v->addWidget(eth_card_);

    wifi_config_ = load_config_or_default(db_, "wifi");
    wifi_card_ = new NetCard(QStringLiteral("Wi-Fi"), "wifi", true, wifi_config_);
    wifi_card_->on_apply = [this](const std::string& iface, const NetConfigUi& ui) {
        apply_net_config(iface, ui);
    };
    wifi_card_->on_scan = [this] { scan_wifi(); };
    wifi_card_->on_connect = [this](const std::string& ssid, const std::string& pw) {
        connect_wifi(ssid, pw);
    };
    v->addWidget(wifi_card_);
    v->addStretch(1);
    return page;
}

QWidget* SettingsDialog::build_about() {
    auto* page = new QWidget;
    auto* v = new QVBoxLayout(page);
    v->setSpacing(6);
    v->addWidget(eyebrow(QStringLiteral("ABOUT")));
    auto* row = new QHBoxLayout;
    row->addWidget(dim_label(QStringLiteral("Denso Digital Reader")), 1);
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

// ── Network handlers ─────────────────────────────────────────────────────────

void SettingsDialog::run_async(std::function<void()> work) {
    auto* thread = QThread::create(std::move(work));
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();
}

void SettingsDialog::refresh_network() {
    refresh_btn_->setText(QStringLiteral("Loading…"));
    run_async([this] {
        const network::NetworkSnapshot snap = network::backend()->snapshot();
        QMetaObject::invokeMethod(
            this,
            [this, snap] {
                eth_card_->set_status(to_net_status(snap.ethernet));
                wifi_card_->set_status(to_net_status(snap.wifi));
                refresh_btn_->setText(QStringLiteral("Refresh"));
            },
            Qt::QueuedConnection);
    });
}

void SettingsDialog::apply_net_config(const std::string& iface, const NetConfigUi& ui) {
    const network::NetConfig cfg = from_ui_config(iface, ui);
    network::save(db_, cfg);  // app owns the truth; persist before pushing
    QString status;
    try {
        network::backend()->apply_config(cfg);
        status = QStringLiteral("Applied");
    } catch (const std::exception& e) {
        status = QStringLiteral("Error: %1").arg(QString::fromUtf8(e.what()));
    }
    const NetConfigUi canonical = to_ui_config(cfg);
    (iface == "wifi" ? wifi_config_ : eth_config_) = canonical;
    NetCard* card = iface == "wifi" ? wifi_card_ : eth_card_;
    card->set_config(canonical);
    card->set_config_status(status);
}

void SettingsDialog::scan_wifi() {
    wifi_card_->set_scanning(true);
    const std::string current_ssid = wifi_card_->current_ssid();
    run_async([this, current_ssid] {
        std::optional<std::vector<network::WifiNetwork>> nets;
        std::string err;
        try {
            nets = network::backend()->scan_wifi();
        } catch (const std::exception& e) {
            err = e.what();
        }
        QMetaObject::invokeMethod(
            this,
            [this, nets, err, current_ssid] {
                if (nets)
                    wifi_card_->set_networks(wifi_rows(*nets, current_ssid));
                else
                    wifi_card_->set_connect_status(
                        QStringLiteral("Scan failed: %1").arg(QString::fromStdString(err)));
                wifi_card_->set_scanning(false);
            },
            Qt::QueuedConnection);
    });
}

void SettingsDialog::connect_wifi(const std::string& ssid, const std::string& password) {
    wifi_card_->set_connect_status(
        QStringLiteral("Connecting to %1…").arg(QString::fromStdString(ssid)));
    run_async([this, ssid, password] {
        const std::optional<std::string> pw =
            password.empty() ? std::nullopt : std::optional<std::string>(password);
        bool ok = true;
        std::string err;
        try {
            network::backend()->connect_wifi(ssid, pw);
        } catch (const std::exception& e) {
            ok = false;
            err = e.what();
        }
        QMetaObject::invokeMethod(
            this,
            [this, ssid, ok, err] {
                wifi_card_->set_connect_status(
                    ok ? QStringLiteral("Connected to %1").arg(QString::fromStdString(ssid))
                       : QStringLiteral("Error: %1").arg(QString::fromStdString(err)));
            },
            Qt::QueuedConnection);
    });
}

} // namespace denso::ui
