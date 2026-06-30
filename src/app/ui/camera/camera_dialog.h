// The Camera modal: the management hub for cameras. Two views in a stack — a
// list of configured cameras (with delete) and an "add camera" form (USB
// auto-scan via QMediaDevices, or manual IP/RTSP entry). Persists through the
// camera repo and emits cameras_changed() so the main view can refresh. The
// configure/draw wizard steps and live preview come in later slices.
#pragma once

#include <QDialog>
#include <QSqlDatabase>

class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QRadioButton;
class QStackedWidget;
class QVBoxLayout;

namespace denso::ui {

class CameraDialog : public QDialog {
    Q_OBJECT

public:
    explicit CameraDialog(QSqlDatabase db, QWidget* parent = nullptr);

signals:
    void cameras_changed();

private:
    void show_list();              // refresh rows + switch to the list page
    void show_add();               // reset the form + switch to the add page
    void rebuild_list();           // populate camera rows from the DB
    void scan_usb();               // (re)enumerate USB cameras into the results list
    void scan_ip();                // probe the subnet for open RTSP hosts (threaded)
    void update_rtsp_preview();    // rebuild the constructed RTSP URL preview
    void save_new_camera();        // validate + insert + back to the list
    void update_source_fields();   // show USB vs IP inputs

    QSqlDatabase db_;
    QStackedWidget* stack_ = nullptr;

    // List page
    QVBoxLayout* rows_box_ = nullptr;
    QLabel* empty_label_ = nullptr;

    // Add page
    QRadioButton* usb_radio_ = nullptr;
    QRadioButton* ip_radio_ = nullptr;
    QLineEdit* name_edit_ = nullptr;
    QWidget* usb_box_ = nullptr;
    QPushButton* scan_btn_ = nullptr;
    QListWidget* usb_list_ = nullptr;  // each item's UserRole = device index
    QWidget* ip_box_ = nullptr;
    QPushButton* ip_scan_btn_ = nullptr;
    QListWidget* ip_list_ = nullptr;   // each item's UserRole = host IP string
    QComboBox* mfr_combo_ = nullptr;   // manufacturer (itemData = index into rtsp_manufacturers)
    QComboBox* stream_combo_ = nullptr;  // 0 = main, 1 = sub
    QLineEdit* ip_edit_ = nullptr;
    QLineEdit* user_edit_ = nullptr;
    QLineEdit* pass_edit_ = nullptr;
    QLabel* rtsp_preview_ = nullptr;
    QLabel* add_error_ = nullptr;
};

} // namespace denso::ui
