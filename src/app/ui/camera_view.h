// The main content area's camera view. For now it renders only the empty
// ("no cameras yet") state; a later slice swaps in the 1–4 live preview grid
// when cameras exist. Independent of the rest of the app — zero cameras just
// means this empty state shows and nothing captures.
#pragma once

#include <QWidget>

namespace denso::ui {

class CameraView : public QWidget {
    Q_OBJECT

public:
    explicit CameraView(QWidget* parent = nullptr);

private:
    // The add-camera flow is defined in a later slice; this is its seam.
    void request_add_camera();
};

} // namespace denso::ui
