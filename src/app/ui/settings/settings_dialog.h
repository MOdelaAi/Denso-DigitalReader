// The Settings modal: a left nav over five panels (Appearance, Display, System,
// Network, About), ported from `settings-modal.slint` + the `settings/*.slint`
// panels. Display/appearance/reset actions are surfaced as signals for the
// window to persist + apply; the network panel owns its own DB-backed apply and
// the threaded scan/connect/refresh (the Qt analog of the Slint window's
// network callbacks + `std::thread`/`upgrade_in_event_loop`).
#pragma once

#include "settings/display.h"

#include <QDialog>
#include <QSqlDatabase>
#include <QString>

#include <cstdint>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QStackedWidget;

namespace denso::ui {

class NetworkPanel;

class SettingsDialog : public QDialog {
    Q_OBJECT

public:
    explicit SettingsDialog(QSqlDatabase db, QWidget* parent = nullptr);

    // ── Startup seeding (read-only / persisted state pushed in by the window).
    void set_app_version(const QString& version);
    void set_hardware(const QString& os, const QString& device, const QString& ram,
                      const QString& storage);
    void set_display_mode(settings::DisplayMode mode);      // no signal emitted
    void set_window_size(uint32_t width, uint32_t height);  // no signal emitted
    void set_theme_dark(bool dark);                         // no signal emitted

protected:
    // The Slint modal is recreated on each open, so it always starts on the
    // first tab — reset the nav to match when the reused dialog is shown.
    void showEvent(QShowEvent* event) override;

signals:
    // Batched display apply: mode is static_cast<int>(DisplayMode); width/height
    // are the windowed size. The window runs the confirm/revert transaction.
    void apply_display_requested(int mode, int width, int height);
    void theme_changed(bool dark);
    void reset_defaults_requested();

private:
    QWidget* build_appearance();
    QWidget* build_display();
    QWidget* build_system();
    QWidget* build_network();
    QWidget* build_server();
    QWidget* build_about();

    void rebuild_window_sizes();  // filter PRESETS to this dialog's screen
    void sync_size_enabled();     // enable window_size_ only in Windowed
    settings::DisplayMode staged_mode() const;

    QSqlDatabase db_;
    bool suppress_signals_ = false;

    QListWidget* nav_ = nullptr;
    QStackedWidget* stack_ = nullptr;

    // Appearance / Display
    QCheckBox* dark_switch_ = nullptr;
    QComboBox* display_mode_ = nullptr;
    QComboBox* window_size_ = nullptr;
    QLabel* window_size_hint_ = nullptr;

    // System / About
    QLabel* hw_os_ = nullptr;
    QLabel* hw_device_ = nullptr;
    QLabel* hw_ram_ = nullptr;
    QLabel* hw_storage_ = nullptr;
    QLabel* about_version_ = nullptr;

    // Network page — a self-contained widget owning its cards + async handlers.
    NetworkPanel* network_panel_ = nullptr;

    // Server (brazing reporter)
    QCheckBox* brazing_enabled_ = nullptr;
    QLineEdit* brazing_url_ = nullptr;
};

} // namespace denso::ui
