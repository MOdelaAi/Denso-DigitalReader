#include "ui/netcard.h"

#include <QComboBox>
#include <QEvent>
#include <QFont>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPushButton>
#include <QVariant>
#include <QVBoxLayout>

namespace denso::ui {
namespace {

// Theme-independent status colours (identical in both themes in theme.slint).
constexpr const char* kStatusOk = "#22c55e";
constexpr const char* kStatusNeutral = "#6b7280";
constexpr const char* kStatusBad = "#ef4444";

QString or_dash(const std::string& s) {
    return s.empty() ? QStringLiteral("—") : QString::fromStdString(s);
}

/// A `color:` stylesheet rule from a hex literal (avoids relying on implicit
/// const char*→QString in QString::arg).
QString color_rule(const char* hex) {
    return QStringLiteral("color: %1;").arg(QString::fromLatin1(hex));
}

QLabel* dim_label(const QString& text) {
    auto* l = new QLabel(text);
    l->setProperty("dim", true);
    return l;
}

QLabel* faint_label(const QString& text) {
    auto* l = new QLabel(text);
    l->setProperty("faint", true);
    return l;
}

// A small "?" glyph whose hover tooltip explains the field — the Qt analog of
// the Slint HelpHint popup.
QLabel* help_hint(const QString& text) {
    auto* h = new QLabel(QStringLiteral("?"));
    h->setProperty("faint", true);
    h->setToolTip(text);
    h->setFixedSize(16, 16);
    h->setAlignment(Qt::AlignCenter);
    return h;
}

// label (dim, stretch) + value (txt) — the Slint SpecRow.
QWidget* spec_row(const QString& label, QLabel** value_out) {
    auto* w = new QWidget;
    auto* row = new QHBoxLayout(w);
    row->setContentsMargins(0, 0, 0, 0);
    auto* l = dim_label(label);
    auto* v = new QLabel;
    row->addWidget(l, 1);
    row->addWidget(v, 0);
    *value_out = v;
    return w;
}

QFrame* divider() {
    auto* line = new QFrame;
    line->setFrameShape(QFrame::HLine);
    line->setFixedHeight(1);
    return line;
}

} // namespace

NetCard::NetCard(const QString& title, std::string iface, bool is_wifi,
                 const NetConfigUi& config, QWidget* parent)
    : QWidget(parent), iface_(std::move(iface)), is_wifi_(is_wifi) {
    setObjectName(QStringLiteral("card"));

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(14, 14, 14, 14);
    outer->setSpacing(10);

    // ── Status header ──
    auto* header = new QHBoxLayout;
    auto* title_lbl = new QLabel(title);
    QFont tf = title_lbl->font();
    tf.setBold(true);
    title_lbl->setFont(tf);
    status_state_ = new QLabel(QStringLiteral("Disconnected"));
    status_state_->setStyleSheet(color_rule(kStatusNeutral));
    header->addWidget(title_lbl, 1);
    header->addWidget(status_state_, 0);
    outer->addLayout(header);

    if (is_wifi_) {
        outer->addWidget(spec_row(QStringLiteral("SSID"), &ssid_value_));
        outer->addWidget(spec_row(QStringLiteral("Signal"), &signal_value_));
    }
    outer->addWidget(spec_row(QStringLiteral("IP"), &ip_value_));
    outer->addWidget(spec_row(QStringLiteral("Gateway"), &gateway_value_));

    // ── Wi-Fi scan / connect ──
    if (is_wifi_) {
        outer->addWidget(divider());
        auto* scan_head = new QHBoxLayout;
        scan_head->addWidget(dim_label(QStringLiteral("Available networks")), 0);
        scan_head->addWidget(help_hint(QStringLiteral(
            "Scan lists Wi-Fi networks in range. Click one to connect — 🔒 means a "
            "password is required.")));
        scan_head->addStretch(1);
        scan_btn_ = new QPushButton(QStringLiteral("Scan"));
        scan_btn_->setProperty("flatText", true);
        QObject::connect(scan_btn_, &QPushButton::clicked, this, [this] {
            if (on_scan) on_scan();
        });
        scan_head->addWidget(scan_btn_, 0);
        outer->addLayout(scan_head);

        auto* nets = new QWidget;
        networks_box_ = new QVBoxLayout(nets);
        networks_box_->setContentsMargins(0, 0, 0, 0);
        networks_box_->setSpacing(4);
        outer->addWidget(nets);

        connect_status_ = dim_label(QString());
        connect_status_->setVisible(false);
        outer->addWidget(connect_status_);
    }

    // ── Divider ──
    outer->addWidget(divider());

    // ── Config: mode ──
    auto* mode_row = new QHBoxLayout;
    auto* mode_lbl = dim_label(QStringLiteral("Mode"));
    mode_lbl->setFixedWidth(72);
    mode_ = new QComboBox;
    mode_->addItems({QStringLiteral("DHCP"), QStringLiteral("Static")});
    mode_->setCurrentIndex(config.mode == "static" ? 1 : 0);
    mode_row->addWidget(mode_lbl, 0);
    mode_row->addWidget(mode_, 1);
    mode_row->addWidget(help_hint(QStringLiteral(
        "DHCP gets these settings automatically from the network. Static lets you "
        "set them by hand.")));
    outer->addLayout(mode_row);

    // ── Config: static fields (collapse under DHCP) ──
    static_box_ = new QWidget;
    auto* sb = new QVBoxLayout(static_box_);
    sb->setContentsMargins(0, 0, 0, 0);
    sb->setSpacing(8);

    const auto field_row = [&](const QString& label, const QString& placeholder,
                               const QString& hint, QLineEdit** out) {
        auto* w = new QWidget;
        auto* row = new QHBoxLayout(w);
        row->setContentsMargins(0, 0, 0, 0);
        row->setSpacing(8);
        auto* l = dim_label(label);
        l->setFixedWidth(72);
        auto* edit = new QLineEdit;
        edit->setPlaceholderText(placeholder);
        row->addWidget(l, 0);
        row->addWidget(edit, 1);
        row->addWidget(help_hint(hint), 0);
        *out = edit;
        sb->addWidget(w);
    };
    field_row(QStringLiteral("IP"), QStringLiteral("192.168.1.50"),
              QStringLiteral("This device's address on the network, e.g. 192.168.1.50. "
                             "Must be unique."),
              &ip_);
    field_row(QStringLiteral("Prefix"), QStringLiteral("24"),
              QStringLiteral("Subnet size as a CIDR prefix length (0–32). 24 means "
                             "255.255.255.0."),
              &prefix_);
    field_row(QStringLiteral("Gateway"), QStringLiteral("192.168.1.1"),
              QStringLiteral("The router address that traffic to other networks is sent "
                             "through."),
              &gateway_);
    field_row(QStringLiteral("DNS 1"), QStringLiteral("8.8.8.8"),
              QStringLiteral("Primary DNS server, which resolves names to addresses, "
                             "e.g. 8.8.8.8."),
              &dns1_);
    field_row(QStringLiteral("DNS 2"), QStringLiteral("1.1.1.1"),
              QStringLiteral("Backup DNS server, used when the primary is unreachable. "
                             "Optional."),
              &dns2_);
    outer->addWidget(static_box_);

    // Seed the persistent edit state from the saved config.
    ip_->setText(QString::fromStdString(config.ip));
    prefix_->setText(QString::fromStdString(config.prefix));
    gateway_->setText(QString::fromStdString(config.gateway));
    dns1_->setText(QString::fromStdString(config.dns1));
    dns2_->setText(QString::fromStdString(config.dns2));

    // ── Apply row ──
    auto* apply_row = new QHBoxLayout;
    apply_row->setSpacing(12);
    config_status_ = new QLabel;
    auto* apply_btn = new QPushButton(QStringLiteral("Apply"));
    apply_btn->setProperty("flatText", true);
    QObject::connect(apply_btn, &QPushButton::clicked, this, [this] {
        if (on_apply) on_apply(iface_, current_config());
    });
    apply_row->addWidget(config_status_, 1);
    apply_row->addWidget(apply_btn, 0);
    outer->addLayout(apply_row);

    QObject::connect(mode_, &QComboBox::currentIndexChanged, this,
                     [this] { rebuild_static_visibility(); });
    rebuild_static_visibility();
}

void NetCard::rebuild_static_visibility() {
    static_box_->setVisible(mode_->currentIndex() == 1);
}

NetConfigUi NetCard::current_config() const {
    return NetConfigUi{
        mode_->currentIndex() == 1 ? "static" : "dhcp",
        ip_->text().toStdString(),
        prefix_->text().toStdString(),
        gateway_->text().toStdString(),
        dns1_->text().toStdString(),
        dns2_->text().toStdString(),
    };
}

void NetCard::set_status(const NetStatus& s) {
    ssid_ = s.ssid;
    status_state_->setText(s.connected ? QStringLiteral("Connected")
                                       : QStringLiteral("Disconnected"));
    status_state_->setStyleSheet(color_rule(s.connected ? kStatusOk : kStatusNeutral));
    if (is_wifi_) {
        ssid_value_->setText(or_dash(s.ssid));
        signal_value_->setText(or_dash(s.signal));
    }
    ip_value_->setText(or_dash(s.ip));
    gateway_value_->setText(or_dash(s.gateway));
}

void NetCard::set_config(const NetConfigUi& config) {
    mode_->setCurrentIndex(config.mode == "static" ? 1 : 0);
    ip_->setText(QString::fromStdString(config.ip));
    prefix_->setText(QString::fromStdString(config.prefix));
    gateway_->setText(QString::fromStdString(config.gateway));
    dns1_->setText(QString::fromStdString(config.dns1));
    dns2_->setText(QString::fromStdString(config.dns2));
}

void NetCard::set_config_status(const QString& text) {
    config_status_->setText(text);
    config_status_->setStyleSheet(
        color_rule(text == QStringLiteral("Applied") ? kStatusOk : kStatusBad));
}

void NetCard::set_scanning(bool scanning) {
    if (scan_btn_)
        scan_btn_->setText(scanning ? QStringLiteral("Scanning…") : QStringLiteral("Scan"));
}

void NetCard::set_connect_status(const QString& text) {
    if (!connect_status_) return;
    connect_status_->setText(text);
    connect_status_->setVisible(!text.isEmpty());
}

void NetCard::set_networks(const std::vector<WifiRow>& rows) {
    networks_ = rows;
    rebuild_networks();
}

void NetCard::rebuild_networks() {
    if (!networks_box_) return;
    // Clear existing rows.
    while (QLayoutItem* item = networks_box_->takeAt(0)) {
        if (QWidget* w = item->widget()) w->deleteLater();
        delete item;
    }

    for (const WifiRow& net : networks_) {
        auto* row = new QWidget;
        auto* rl = new QVBoxLayout(row);
        rl->setContentsMargins(0, 0, 0, 0);
        rl->setSpacing(4);

        // Header (click toggles the expand area).
        auto* head = new QWidget;
        auto* hl = new QHBoxLayout(head);
        hl->setContentsMargins(6, 4, 6, 4);
        hl->setSpacing(8);
        auto* ssid_lbl = new QLabel(QString::fromStdString(net.ssid));
        if (net.connected) {
            QFont f = ssid_lbl->font();
            f.setBold(true);
            ssid_lbl->setFont(f);
            ssid_lbl->setStyleSheet(color_rule(kStatusOk));
        }
        hl->addWidget(ssid_lbl, 1);
        if (net.connected) {
            auto* badge = new QLabel(QStringLiteral("Connected"));
            badge->setStyleSheet(color_rule(kStatusOk));
            hl->addWidget(badge, 0);
        }
        hl->addWidget(faint_label(net.secured ? QStringLiteral("🔒") : QString()), 0);
        hl->addWidget(faint_label(QString::fromStdString(net.signal)), 0);

        // Expandable connect area (hidden until the header is clicked).
        auto* expand = new QWidget;
        auto* el = new QHBoxLayout(expand);
        el->setContentsMargins(6, 0, 0, 0);
        el->setSpacing(8);
        QLineEdit* pw = nullptr;
        if (net.secured) {
            pw = new QLineEdit;
            pw->setPlaceholderText(QStringLiteral("Password"));
            pw->setEchoMode(QLineEdit::Password);
            el->addWidget(pw, 1);
        }
        auto* connect_btn = new QPushButton(QStringLiteral("Connect"));
        connect_btn->setProperty("flatText", true);
        el->addWidget(connect_btn, 0);
        expand->setVisible(false);

        const std::string ssid = net.ssid;
        const bool secured = net.secured;
        QObject::connect(connect_btn, &QPushButton::clicked, this, [this, ssid, secured, pw] {
            if (on_connect)
                on_connect(ssid, secured && pw ? pw->text().toStdString() : std::string());
        });

        // Toggle expand on header click via an event filter on the header.
        head->installEventFilter(this);
        head->setProperty("expandTarget", QVariant::fromValue(static_cast<QObject*>(expand)));
        head->setCursor(Qt::PointingHandCursor);

        rl->addWidget(head);
        rl->addWidget(expand);
        networks_box_->addWidget(row);
    }
}

bool NetCard::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() == QEvent::MouseButtonRelease) {
        const QVariant target = watched->property("expandTarget");
        if (target.isValid()) {
            if (auto* expand = qobject_cast<QWidget*>(target.value<QObject*>())) {
                expand->setVisible(!expand->isVisible());
                return true;
            }
        }
    }
    return QWidget::eventFilter(watched, event);
}

} // namespace denso::ui
