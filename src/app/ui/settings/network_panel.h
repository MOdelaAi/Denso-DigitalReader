// The Network settings page as a self-contained widget: it owns the two
// NetCards, the Refresh button, the DB handle, and the threaded
// scan/connect/refresh/apply handlers. Extracted from SettingsDialog so the
// dialog is a thin view. on_shown() re-seeds the editors from saved config and
// refreshes live status, reproducing the "entering the Network tab reloads" of
// the Slint original.
#pragma once

#include "network/model.h"
#include "ui/viewmodel.h"

#include <QPointer>
#include <QSqlDatabase>
#include <QWidget>

#include <optional>
#include <string>
#include <vector>

class QPushButton;
class QThread;

namespace denso::ui {

class NetCard;

class NetworkPanel : public QWidget {
    Q_OBJECT

public:
    explicit NetworkPanel(QSqlDatabase db, QWidget* parent = nullptr);
    ~NetworkPanel() override;

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
    std::vector<QPointer<QThread>> workers_;  // outstanding async workers
    bool net_busy_ = false;  // an action is in flight — ignore overlapping clicks
    // Last live status, kept so on_shown() can paint it instantly (stale-while-
    // revalidate) instead of blanking to "Loading…" during the slow OS query.
    // The panel outlives each modal open (the dialog is built once), so this
    // survives reopens.
    std::optional<network::NetworkSnapshot> last_snapshot_;
};

} // namespace denso::ui
