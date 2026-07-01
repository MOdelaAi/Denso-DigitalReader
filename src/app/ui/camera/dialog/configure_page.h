// The Camera dialog's "Configure" step: a snapshot preview plus resolution,
// fps, rotation, pitch and roll. The coordinator captures frames (it owns the
// source) and pushes them in via set_frame(); this page renders the preview
// with its own orientation controls and reads its values back into a draft.
#pragma once

#include "camera/camera.h"

#include <QImage>
#include <QSize>
#include <QWidget>

class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QPushButton;
class QSpinBox;

namespace denso::ui {

class CameraConfigurePage : public QWidget {
    Q_OBJECT

public:
    explicit CameraConfigurePage(QWidget* parent = nullptr);

    void populate(const camera::Camera& cam);   // fill controls from a camera
    void read_into(camera::Camera& cam) const;   // controls → camera
    QSize resolution() const;                     // selected capture resolution

    void set_frame(const QImage& raw_frame);      // newest snapshot (un-oriented)
    void set_preview_text(const QString& text);   // placeholder / status / error
    void set_capturing(bool capturing);           // Capture button busy state
    void show_error(const QString& msg);
    void clear_error();

signals:
    void back_requested();
    void next_requested();
    void capture_requested();

private:
    void render_preview();  // re-apply orientation to the stored raw frame

    QLabel* preview_label_ = nullptr;
    QPushButton* capture_btn_ = nullptr;
    QComboBox* res_combo_ = nullptr;       // itemData = QSize
    QSpinBox* fps_spin_ = nullptr;
    QComboBox* rotation_combo_ = nullptr;  // itemData = degrees (0/90/180/270)
    QDoubleSpinBox* pitch_spin_ = nullptr;
    QDoubleSpinBox* roll_spin_ = nullptr;
    QLabel* error_ = nullptr;
    QImage raw_frame_;  // most recent un-oriented snapshot
};

} // namespace denso::ui
