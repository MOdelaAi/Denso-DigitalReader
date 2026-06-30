// One interface's network card: live status on top, editable IP/DNS config
// below, and (for Wi-Fi) a scan list with per-row connect. A faithful port of
// the `NetCard`/`WifiRowItem`/`FieldRow` components in `settings/network.slint`
// to Qt Widgets. Plain QWidget (no own signals) — the dialog wires behaviour
// through the std::function hooks, mirroring how the Slint card forwards its
// `apply`/`scan`/`connect` callbacks up to the window.
#pragma once

#include "ui/viewmodel.h"

#include <QWidget>

#include <functional>
#include <string>
#include <vector>

class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QVBoxLayout;
class QWidget;

namespace denso::ui {

class NetCard : public QWidget {
public:
    NetCard(const QString& title, std::string iface, bool is_wifi,
            const NetConfigUi& config, QWidget* parent = nullptr);

    // Live status / config-apply outcome (set from the refresh + apply paths).
    void set_status(const NetStatus& s);
    void set_config(const NetConfigUi& config);  // re-seed editor after apply
    void set_config_status(const QString& text);

    // Wi-Fi scan/connect surface (only wired for the Wi-Fi card).
    void set_networks(const std::vector<WifiRow>& rows);
    void set_scanning(bool scanning);
    void set_connect_status(const QString& text);

    /// The editable config as shown (mode + the persistent static fields).
    NetConfigUi current_config() const;

    /// Last status SSID — used to float the connected row to the top on scan.
    std::string current_ssid() const { return ssid_; }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

    // Behaviour hooks the dialog installs (the card's Slint callbacks).
    std::function<void(const std::string&, const NetConfigUi&)> on_apply;
    std::function<void()> on_scan;
    std::function<void(const std::string&, const std::string&)> on_connect;

private:
    void rebuild_static_visibility();
    void rebuild_networks();

    std::string iface_;
    bool is_wifi_ = false;
    std::string ssid_;  // from the last status, for scan floating

    QComboBox* mode_ = nullptr;       // 0=DHCP, 1=Static
    QWidget* static_box_ = nullptr;   // collapses when DHCP
    QLineEdit* ip_ = nullptr;
    QLineEdit* prefix_ = nullptr;
    QLineEdit* gateway_ = nullptr;
    QLineEdit* dns1_ = nullptr;
    QLineEdit* dns2_ = nullptr;
    QLabel* config_status_ = nullptr;

    // status rows
    QLabel* status_state_ = nullptr;
    QLabel* ssid_value_ = nullptr;   // wifi only
    QLabel* signal_value_ = nullptr; // wifi only
    QLabel* ip_value_ = nullptr;
    QLabel* gateway_value_ = nullptr;

    // wifi scan
    QPushButton* scan_btn_ = nullptr;
    QVBoxLayout* networks_box_ = nullptr;
    QLabel* connect_status_ = nullptr;
    std::vector<WifiRow> networks_;
};

} // namespace denso::ui
