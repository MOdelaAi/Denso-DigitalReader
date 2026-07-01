// The Camera dialog's "Source" step: pick USB (auto-scan) or IP/RTSP (subnet
// scan + manufacturer/stream/credentials with a live RTSP-URL preview), name
// the camera, and continue. Validates on Next and emits the assembled draft;
// the coordinator owns what happens next.
#pragma once

#include "camera/camera.h"

#include <QWidget>

class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QRadioButton;
class QSpinBox;

namespace denso::ui {

class CameraAddPage : public QWidget {
    Q_OBJECT

public:
    explicit CameraAddPage(QWidget* parent = nullptr);

    /// Clear the form and re-run the USB scan, ready for a fresh add.
    void reset();

signals:
    void cancel_requested();
    void next_requested(const camera::Camera& draft);

private:
    void scan_usb();
    void scan_ip();
    void update_rtsp_preview();
    void update_source_fields();  // show USB vs IP inputs
    void validate_and_emit();     // build a draft from the form or show an error

    QRadioButton* usb_radio_ = nullptr;
    QRadioButton* ip_radio_ = nullptr;
    QLineEdit* name_edit_ = nullptr;
    QWidget* usb_box_ = nullptr;
    QListWidget* usb_list_ = nullptr;  // each item's UserRole = device index
    QWidget* ip_box_ = nullptr;
    QPushButton* ip_scan_btn_ = nullptr;
    QListWidget* ip_list_ = nullptr;   // each item's UserRole = host IP string
    QComboBox* mfr_combo_ = nullptr;
    QComboBox* stream_combo_ = nullptr;  // 0 = main, 1 = sub
    QSpinBox* channel_spin_ = nullptr;   // NVR/DVR channel (1-based)
    QLineEdit* ip_edit_ = nullptr;
    QLineEdit* user_edit_ = nullptr;
    QLineEdit* pass_edit_ = nullptr;
    QLabel* rtsp_preview_ = nullptr;
    QLabel* add_error_ = nullptr;
};

} // namespace denso::ui
