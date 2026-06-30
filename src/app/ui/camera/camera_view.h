// The main content area's camera view. Reflects the camera count: zero shows
// the "no cameras yet" empty state; one or more shows a "configured" placeholder
// (the real 1–4 live preview grid lands in a later slice). The "Add Camera"
// button asks the window to open the Camera modal. Independent of the rest of
// the app — zero cameras just shows the empty state and nothing captures.
#pragma once

#include <QSqlDatabase>
#include <QWidget>

class QLabel;
class QPushButton;

namespace denso::ui {

class CameraView : public QWidget {
    Q_OBJECT

public:
    explicit CameraView(QSqlDatabase db, QWidget* parent = nullptr);

    /// Re-read the camera count and update the displayed state.
    void reload();

signals:
    void add_camera_requested();

private:
    QSqlDatabase db_;
    QLabel* title_ = nullptr;
    QLabel* subtitle_ = nullptr;
};

} // namespace denso::ui
