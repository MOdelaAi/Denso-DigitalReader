// The Settings modal: a left nav over five panels (Appearance, Display, System,
// Network, About), ported from `settings-modal.slint` + the `settings/*.slint`
// panels. Display/appearance/reset actions are surfaced as signals for the
// window to persist + apply; the network panel owns its own DB-backed apply and
// the threaded scan/connect/refresh (the Qt analog of the Slint window's
// network callbacks + `std::thread`/`upgrade_in_event_loop`).
#pragma once

#include "ui/viewmodel.h"

#include <QDialog>
#include <QSqlDatabase>
#include <QString>

#include <functional>

class QCheckBox;
class QComboBox;
class QLabel;
class QListWidget;
class QPushButton;
class QStackedWidget;

namespace denso::ui {

class NetCard;

class SettingsDialog : public QDialog {
    Q_OBJECT

public:
    explicit SettingsDialog(QSqlDatabase db, QWidget* parent = nullptr);

    // ── Startup seeding (read-only / persisted state pushed in by the window).
    void set_app_version(const QString& version);
    void set_hardware(const QString& os, const QString& device, const QString& ram,
                      const QString& storage);
    void set_resolution_index(int index);  // no signal emitted
    void set_fullscreen(bool fullscreen);   // no signal emitted
    void set_theme_dark(bool dark);          // no signal emitted

    int resolution_index() const;

protected:
    // The Slint modal is recreated on each open, so it always starts on the
    // first tab — reset the nav to match when the reused dialog is shown.
    void showEvent(QShowEvent* event) override;

signals:
    void apply_resolution_requested(int index);
    void theme_changed(bool dark);
    void toggle_fullscreen_requested(bool fullscreen);
    void reset_defaults_requested();

private:
    QWidget* build_appearance();
    QWidget* build_display();
    QWidget* build_system();
    QWidget* build_network();
    QWidget* build_about();

    // Network handlers (own the DB + the per-call backend, like the Rust wiring).
    void refresh_network();
    void apply_net_config(const std::string& iface, const NetConfigUi& ui);
    void scan_wifi();
    void connect_wifi(const std::string& ssid, const std::string& password);

    // Run blocking OS work on a worker thread, posting results back to the GUI
    // thread (the Qt analog of `std::thread` + `upgrade_in_event_loop`). A real
    // QThread is used so QProcess in the platform backends has an event
    // dispatcher.
    void run_async(std::function<void()> work);

    QSqlDatabase db_;
    bool suppress_signals_ = false;

    QListWidget* nav_ = nullptr;
    QStackedWidget* stack_ = nullptr;

    // Appearance / Display
    QCheckBox* dark_switch_ = nullptr;
    QComboBox* resolution_ = nullptr;
    QCheckBox* fullscreen_switch_ = nullptr;

    // System / About
    QLabel* hw_os_ = nullptr;
    QLabel* hw_device_ = nullptr;
    QLabel* hw_ram_ = nullptr;
    QLabel* hw_storage_ = nullptr;
    QLabel* about_version_ = nullptr;

    // Network
    QPushButton* refresh_btn_ = nullptr;
    NetCard* eth_card_ = nullptr;
    NetCard* wifi_card_ = nullptr;
};

} // namespace denso::ui
