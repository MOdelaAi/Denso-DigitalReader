// The Camera modal: the management hub for cameras. A thin coordinator over a
// QStackedWidget of four page widgets — a list of configured cameras (with
// delete), an "add camera" Source form (USB auto-scan or manual IP/RTSP), a
// Configure page (snapshot preview + resolution/fps/rotation/pitch/roll), and an
// Areas page (draw ROI polygons over the snapshot). The pages own their own
// widgets and emit request signals; this class is a thin view — it owns only
// the page stack, the stepper, and modal sizing. Flow-state, the threaded
// snapshot capture, and every DB write for add/edit live in
// CameraWizardController, driven via an injected show_page callback and its
// request_show_list() signal. This class emits cameras_changed() so the main
// view can refresh. The Areas step is optional: it's offered right after Save
// and reachable later per-camera.
#pragma once

#include "camera/camera.h"

#include <QDialog>
#include <QRect>
#include <QSqlDatabase>

class QStackedWidget;

namespace denso::ui {

class WizardStepper;
class CameraListPage;
class CameraAddPage;
class CameraConfigurePage;
class ModelsPage;
class CameraAreasPage;
class LevelCalibrationPage;
class CameraWizardController;

class CameraDialog : public QDialog {
    Q_OBJECT

public:
    explicit CameraDialog(QSqlDatabase db, QWidget* parent = nullptr);

signals:
    void cameras_changed();

protected:
    void showEvent(QShowEvent* e) override;  // reopen on the list, compact size
    /// Escape and the window's X both land here. The Areas step guards its own
    /// Back/Exit buttons, but those aren't the only ways out of the modal — so
    /// unsaved ROI work gets the same confirmation whichever exit is taken.
    void reject() override;

private:
    void show_page(int index);   // switch stack page + drive stepper/sizing
    void show_list();            // refresh rows + switch to the list page
    void show_add();             // reset the form + switch to the add page

    void expand_for_canvas();  // grow the modal for drawing room
    void restore_size();       // shrink back after a drawing step

    QSqlDatabase db_;
    WizardStepper* stepper_ = nullptr;
    QStackedWidget* stack_ = nullptr;

    CameraListPage* list_page_ = nullptr;            // stack index 0
    CameraAddPage* add_page_ = nullptr;              // stack index 1
    CameraConfigurePage* configure_page_ = nullptr;  // stack index 2
    ModelsPage* models_page_ = nullptr;              // stack index 3
    CameraAreasPage* areas_page_ = nullptr;          // stack index 4
    LevelCalibrationPage* level_page_ = nullptr;     // stack index 5

    CameraWizardController* controller_ = nullptr;  // owns flow-state + persistence

    // Drawing-step sizing (view-owned: only the dialog resizes). Shared by the
    // Areas and Level-calibration steps — both draw over the camera snapshot and
    // both need the room.
    bool canvas_expanded_ = false;  // modal currently grown for drawing
    QRect pre_canvas_geometry_;     // geometry to restore when leaving it
};

} // namespace denso::ui
