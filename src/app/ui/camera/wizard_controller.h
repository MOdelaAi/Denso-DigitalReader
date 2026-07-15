// Owns the Camera wizard's flow: the add/edit state, the threaded snapshot
// capture, and every DB write (camera insert/update, model attach, ROI replace).
// Extracted from CameraDialog so the dialog is a thin view over the page stack.
// The controller never touches the QStackedWidget or the stepper directly — it
// asks the view to switch pages through the injected show_page callback and
// signals request_show_list() for transitions that return to the list.
#pragma once

#include "camera/camera.h"

#include <QImage>
#include <QObject>
#include <QSqlDatabase>
#include <QString>

#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

namespace denso::ui {

class CameraAddPage;
class CameraConfigurePage;
class ModelsPage;
class CameraAreasPage;

class CameraWizardController : public QObject {
    Q_OBJECT

public:
    // The three interactive wizard pages the controller drives. Owned by the
    // dialog; the controller only reads/populates them.
    struct Pages {
        CameraAddPage* add = nullptr;
        CameraConfigurePage* configure = nullptr;
        ModelsPage* models = nullptr;
        CameraAreasPage* areas = nullptr;
    };

    CameraWizardController(QSqlDatabase db, Pages pages,
                           std::function<void(int)> show_page,
                           QObject* parent = nullptr);

signals:
    void cameras_changed();     // camera set changed → main view refreshes
    void request_show_list();   // return to the list page (view owns that switch)

public slots:
    // Source flow.
    void begin_add();              // fresh add: reset Source page, clear edit state
    void begin_edit(const camera::Camera& cam);  // edit: populate Source from cam
    void accept_source(const camera::Camera& source);  // Source Next → open Configure

    // Configure flow.
    void capture_snapshot();       // threaded grab → push frame to the pages
    void save_configured_camera(); // insert/update from draft_, then Models step
    void configure_back();         // Configure Back → Source step

    // Models flow.
    void save_models();            // persist attachments → advance to Areas step

    // Areas flow.
    void begin_areas_direct(const camera::Camera& cam);  // per-row Areas button
    void save_areas(const std::vector<camera::CameraArea>& areas);  // persist + list
    void areas_back();             // Areas Back: direct→list, wizard→Models step

private:
    void open_configure(const QString& preview_text);  // seed Configure from draft_
    void enter_models();           // load catalog + attachments → Models page
    void enter_areas(bool direct); // load areas + frame → Areas page
    void update_areas_background(); // push the oriented frame to the Areas canvas

    QSqlDatabase db_;
    Pages pages_;
    std::function<void(int)> show_page_;

    std::optional<int64_t> editing_id_;  // set in edit mode; empty when adding
    camera::Camera original_;            // pre-edit camera (for source-change diff)
    camera::Camera draft_;               // camera being added/edited
    QImage last_frame_;                  // most recent un-rotated snapshot frame
    uint64_t capture_gen_ = 0;           // newest snapshot request wins (stale-guard)
    bool entered_areas_directly_ = false;  // true: per-row Areas (Back → list)
};

} // namespace denso::ui
