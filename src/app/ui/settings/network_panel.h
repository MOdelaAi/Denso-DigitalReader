// The Network settings page as a self-contained widget: it owns the two
// NetCards, the Refresh button, the DB handle, and the threaded
// scan/connect/refresh/apply handlers. Extracted from SettingsDialog so the
// dialog is a thin view. on_shown() re-seeds the editors from saved config and
// refreshes live status, reproducing the "entering the Network tab reloads" of
// the Slint original.
#pragma once

#include "ui/viewmodel.h"

#include <QSqlDatabase>
#include <QWidget>

#include <string>

class QPushButton;

namespace denso::ui {

class NetCard;

class NetworkPanel : public QWidget {
    Q_OBJECT

public:
    explicit NetworkPanel(QSqlDatabase db, QWidget* parent = nullptr);

    /// Re-seed both cards from saved config, then refresh live status. Called by
    /// the settings dialog when the Network tab becomes visible.
    void on_shown();

private:
    void refresh_network();
    void apply_net_config(const std::string& iface, const NetConfigUi& ui);
    void scan_wifi();
    void connect_wifi(const std::string& ssid, const std::string& password);

    QSqlDatabase db_;
    QPushButton* refresh_btn_ = nullptr;
    NetCard* eth_card_ = nullptr;
    NetCard* wifi_card_ = nullptr;
    // Editors re-seed from these on each on_shown() (so un-applied edits are
    // discarded); apply_net_config refreshes them.
    NetConfigUi eth_config_;
    NetConfigUi wifi_config_;
};

} // namespace denso::ui
