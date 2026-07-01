// The Camera modal: the management hub for cameras. A thin coordinator over a
// QStackedWidget of four page widgets — a list of configured cameras (with
// delete), an "add camera" Source form (USB auto-scan or manual IP/RTSP), a
// Configure page (snapshot preview + resolution/fps/rotation/pitch/roll), and an
// Areas page (draw ROI polygons over the snapshot). The pages own their own
// widgets and emit request signals; this class owns the camera source (snapshot
// capture), the DB writes for add/edit, wizard navigation and modal sizing, and
// emits cameras_changed() so the main view can refresh. The Areas step is
// optional: it's offered right after Save and reachable later per-camera.
#pragma once

#include "camera/model.h"

#include <QDialog>
#include <QImage>
#include <QRect>
#include <QSqlDatabase>
#include <cstdint>
#include <optional>
#include <vector>

class QStackedWidget;

namespace denso::ui {

class WizardStepper;
class CameraListPage;
class CameraAddPage;
class CameraConfigurePage;
class CameraAreasPage;

class CameraDialog : public QDialog {
    Q_OBJECT

public:
    explicit CameraDialog(QSqlDatabase db, QWidget* parent = nullptr);

signals:
    void cameras_changed();

protected:
    void showEvent(QShowEvent* e) override;  // reopen on the list, compact size

private:
    void show_page(int index);   // switch stack page + drive stepper/sizing
    void show_list();            // refresh rows + switch to the list page
    void show_add();             // reset the form + switch to the add page

    // Configure flow (the coordinator owns the camera source + DB writes).
    void begin_configure(const camera::Camera& cam, std::optional<int64_t> id,
                         const QString& preview_text);  // seed draft + open Configure
    void capture_snapshot();       // threaded grab → push frame to the pages
    void save_configured_camera(); // insert/update from draft_, then Areas step

    // Areas flow.
    void enter_areas(bool direct);  // load areas + frame → Areas page
    void update_areas_background();  // push the oriented frame to the Areas canvas
    void save_areas(const std::vector<camera::CameraArea>& areas);  // persist + list

    void expand_for_areas();  // grow the modal for drawing room
    void restore_size();      // shrink back after the Areas step

    QSqlDatabase db_;
    WizardStepper* stepper_ = nullptr;
    QStackedWidget* stack_ = nullptr;

    CameraListPage* list_page_ = nullptr;            // stack index 0
    CameraAddPage* add_page_ = nullptr;              // stack index 1
    CameraConfigurePage* configure_page_ = nullptr;  // stack index 2
    CameraAreasPage* areas_page_ = nullptr;          // stack index 3

    // Add/edit mode state.
    std::optional<int64_t> editing_id_;  // set in edit mode; empty when adding
    camera::Camera draft_;               // camera being added/edited
    QImage last_frame_;                  // most recent un-rotated snapshot frame

    // Areas-step sizing + Back routing.
    bool areas_expanded_ = false;          // modal currently grown for drawing
    QRect pre_areas_geometry_;             // geometry to restore when leaving Areas
    bool entered_areas_directly_ = false;  // true: per-row Areas button (Back → list)
};

} // namespace denso::ui
