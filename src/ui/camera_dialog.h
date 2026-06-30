// Placeholder camera modal — the Qt port of `camera-modal.slint`: same chrome
// (header + gold underline, a preview placeholder, Close / Add Camera footer)
// with the preview to be filled in later. No own signals; "Add Camera" is a
// no-op, exactly as the Slint placeholder's `add-camera` TODO.
#pragma once

#include <QDialog>

namespace denso::ui {

class CameraDialog : public QDialog {
public:
    explicit CameraDialog(QWidget* parent = nullptr);
};

} // namespace denso::ui
