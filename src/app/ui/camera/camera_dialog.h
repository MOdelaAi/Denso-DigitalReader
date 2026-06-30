// The Camera modal: the management hub for cameras. Three views in a stack — a
// list of configured cameras (with delete), an "add camera" form (USB
// auto-scan via QMediaDevices, or manual IP/RTSP entry), and a Configure page
// (snapshot preview + resolution/fps/rotation/pitch/roll). Persists through the
// camera repo and emits cameras_changed() so the main view can refresh. The
// draw-ROI wizard step and live preview come in later slices.
#pragma once

#include "camera/model.h"

#include <QDialog>
#include <QImage>
#include <QSqlDatabase>
#include <cstdint>
#include <optional>

class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QRadioButton;
class QSpinBox;
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

    void build_configure_page();                         // construct the 3rd stack page
    void populate_configure(const camera::Camera& cam);  // fill controls from a camera
    void read_configure_into_draft();                    // controls → draft_
    void capture_snapshot();                             // threaded grab + preview (Task 4)
    void render_preview();                               // re-apply rotation to last_frame_ (Task 4)
    void save_configured_camera();                       // insert/update from draft_ (Task 5)

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
    QSpinBox* channel_spin_ = nullptr;   // NVR/DVR channel (1-based)
    QLineEdit* ip_edit_ = nullptr;
    QLineEdit* user_edit_ = nullptr;
    QLineEdit* pass_edit_ = nullptr;
    QLabel* rtsp_preview_ = nullptr;
    QLabel* add_error_ = nullptr;

    // Configure page
    QWidget* config_page_ = nullptr;
    QLabel* preview_label_ = nullptr;
    QPushButton* capture_btn_ = nullptr;
    QComboBox* res_combo_ = nullptr;       // itemData = QSize
    QSpinBox* fps_spin_ = nullptr;
    QComboBox* rotation_combo_ = nullptr;  // itemData = degrees (0/90/180/270)
    QDoubleSpinBox* pitch_spin_ = nullptr;
    QDoubleSpinBox* roll_spin_ = nullptr;

    // Add/edit mode state
    std::optional<int64_t> editing_id_;    // set in edit mode; empty when adding
    camera::Camera draft_;                 // camera being added/edited
    QImage last_frame_;                    // most recent un-rotated snapshot frame
};

} // namespace denso::ui
