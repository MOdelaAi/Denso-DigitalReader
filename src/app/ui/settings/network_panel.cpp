#include "ui/settings/network_panel.h"

#include "network/backend.h"
#include "network/model.h"
#include "network/repo.h"
#include "ui/common/async_runner.h"
#include "ui/common/form_widgets.h"
#include "ui/convert.h"
#include "ui/settings/netcard.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QThread>
#include <QVBoxLayout>

#include <algorithm>
#include <exception>
#include <optional>
#include <vector>

namespace denso::ui {
namespace {

/// Saved config for `iface`, or a DHCP default when none is stored yet.
NetConfigUi load_config_or_default(const QSqlDatabase& db, const std::string& iface) {
    std::optional<network::NetConfig> stored = network::load(db, iface);
    if (stored) return to_ui_config(*stored);
    network::NetConfig def;
    def.iface = iface;
    def.mode = "dhcp";
    return to_ui_config(def);
}

} // namespace

NetworkPanel::NetworkPanel(QSqlDatabase db, QWidget* parent)
    : QWidget(parent), db_(std::move(db)) {
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(12);

    auto* head = new QHBoxLayout;
    head->addWidget(common::eyebrow(QStringLiteral("NETWORK")), 1);
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
}

NetworkPanel::~NetworkPanel() {
    for (const QPointer<QThread>& w : workers_) {
        if (w && w->isRunning()) {
            w->wait();
        }
    }
}

void NetworkPanel::on_shown() {
    // Re-seed the editors from the saved config (discarding un-applied edits),
    // then refresh live status — the Slint nav callback's behavior.
    eth_card_->set_config(eth_config_);
    wifi_card_->set_config(wifi_config_);
    // Stale-while-revalidate: if we already have a snapshot from a prior open,
    // paint it immediately so the page shows real status at once instead of a
    // blank "Loading…" for the seconds the OS query (netsh/nmcli) can take. The
    // background refresh below then updates it in place.
    if (last_snapshot_) {
        eth_card_->set_status(to_net_status(last_snapshot_->ethernet));
        wifi_card_->set_status(to_net_status(last_snapshot_->wifi));
    }
    refresh_network();
}

void NetworkPanel::refresh_network() {
    if (net_busy_) return;
    net_busy_ = true;
    workers_.erase(std::remove_if(workers_.begin(), workers_.end(),
                                  [](const QPointer<QThread>& w) { return !w; }),
                   workers_.end());
    refresh_btn_->setEnabled(false);
    // With cached status already on screen this is a background revalidation,
    // not a cold load — label it so stale-but-visible status reads as provisional.
    refresh_btn_->setText(last_snapshot_ ? QStringLiteral("Refreshing…")
                                         : QStringLiteral("Loading…"));
    QPointer<NetworkPanel> self(this);
    workers_.push_back(common::run_on_worker([this, self] {
        // snapshot() is bounded but can still throw (spawn failure, parse). Never
        // let that escape the worker: it would leave net_busy_ pinned true and the
        // button stuck on "Loading…" for the rest of the session.
        std::optional<network::NetworkSnapshot> snap;
        try {
            snap = network::backend()->snapshot();
        } catch (...) {
        }
        if (!self) return;  // panel gone — skip the post
        common::post_to_gui(this, [this, snap] {
            if (snap) {
                last_snapshot_ = snap;
                eth_card_->set_status(to_net_status(snap->ethernet));
                wifi_card_->set_status(to_net_status(snap->wifi));
            }
            refresh_btn_->setText(QStringLiteral("Refresh"));
            refresh_btn_->setEnabled(true);
            net_busy_ = false;
        });
    }));
}

void NetworkPanel::apply_net_config(const std::string& iface, const NetConfigUi& ui) {
    if (net_busy_) return;  // first line: a double-click can't double-apply
    net_busy_ = true;
    workers_.erase(std::remove_if(workers_.begin(), workers_.end(),
                                  [](const QPointer<QThread>& w) { return !w; }),
                   workers_.end());

    const network::NetConfig cfg = from_ui_config(iface, ui);
    network::save(db_, cfg);  // app owns the truth; persist before pushing
    const NetConfigUi canonical = to_ui_config(cfg);
    (iface == "wifi" ? wifi_config_ : eth_config_) = canonical;
    NetCard* card = iface == "wifi" ? wifi_card_ : eth_card_;
    card->set_config(canonical);
    card->set_config_status(QStringLiteral("Applying…"));

    QPointer<NetworkPanel> self(this);
    workers_.push_back(common::run_on_worker([this, self, cfg, card] {
        QString status;
        try {
            network::backend()->apply_config(cfg);
            status = QStringLiteral("Applied");
        } catch (const std::exception& e) {
            status = QStringLiteral("Error: %1").arg(QString::fromUtf8(e.what()));
        }
        if (!self) return;  // panel gone — skip the post
        common::post_to_gui(this, [this, card, status] {
            card->set_config_status(status);
            net_busy_ = false;
        });
    }));
}

void NetworkPanel::scan_wifi() {
    if (net_busy_) return;
    net_busy_ = true;
    workers_.erase(std::remove_if(workers_.begin(), workers_.end(),
                                  [](const QPointer<QThread>& w) { return !w; }),
                   workers_.end());
    wifi_card_->set_scanning(true);
    const std::string current_ssid = wifi_card_->current_ssid();
    QPointer<NetworkPanel> self(this);
    workers_.push_back(common::run_on_worker([this, self, current_ssid] {
        std::optional<std::vector<network::WifiNetwork>> nets;
        std::string err;
        try {
            nets = network::backend()->scan_wifi();
        } catch (const std::exception& e) {
            err = e.what();
        }
        if (!self) return;  // panel gone — skip the post
        common::post_to_gui(this, [this, nets, err, current_ssid] {
            if (nets)
                wifi_card_->set_networks(wifi_rows(*nets, current_ssid));
            else
                wifi_card_->set_connect_status(
                    QStringLiteral("Scan failed: %1").arg(QString::fromStdString(err)));
            wifi_card_->set_scanning(false);
            net_busy_ = false;
        });
    }));
}

void NetworkPanel::connect_wifi(const std::string& ssid, const std::string& password) {
    if (net_busy_) return;
    net_busy_ = true;
    workers_.erase(std::remove_if(workers_.begin(), workers_.end(),
                                  [](const QPointer<QThread>& w) { return !w; }),
                   workers_.end());
    wifi_card_->set_connect_status(
        QStringLiteral("Connecting to %1…").arg(QString::fromStdString(ssid)));
    QPointer<NetworkPanel> self(this);
    workers_.push_back(common::run_on_worker([this, self, ssid, password] {
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
        if (!self) return;  // panel gone — skip the post
        common::post_to_gui(this, [this, ssid, ok, err] {
            wifi_card_->set_connect_status(
                ok ? QStringLiteral("Connected to %1").arg(QString::fromStdString(ssid))
                   : QStringLiteral("Error: %1").arg(QString::fromStdString(err)));
            net_busy_ = false;
        });
    }));
}

} // namespace denso::ui
