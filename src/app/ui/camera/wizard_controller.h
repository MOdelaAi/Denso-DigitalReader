// Owns the Camera wizard's flow: the add/edit state, the threaded snapshot
// capture, and every DB write (camera insert/update, model attach, ROI replace,
// level calibration). Extracted from CameraDialog so the dialog is a thin view
// over the page stack. The controller never touches the QStackedWidget or the
// stepper directly — it asks the view to switch pages through the injected
// show_page callback and signals request_show_list() for transitions that return
// to the list.
//
// The wizard's fourth step depends on the COMMITTED operating mode: ROI areas for
// the digit reader, level calibration for the ball leveler. That branch is a FLOW
// decision, not an authorization one — which models may be offered is still asked
// of the one central compatibility policy, through detection::evaluated_models.
#pragma once

#include "camera/camera.h"
#include "camera/preview_gate.h"
#include "level/calibration.h"
#include "level/repo.h"  // LevelBinding

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
class LevelCalibrationPage;

class CameraWizardController : public QObject {
    Q_OBJECT

public:
    // The interactive wizard pages the controller drives. Owned by the dialog;
    // the controller only reads/populates them. `level` may be null for a host
    // that does not build the ball-leveler step.
    struct Pages {
        CameraAddPage* add = nullptr;
        CameraConfigurePage* configure = nullptr;
        ModelsPage* models = nullptr;
        CameraAreasPage* areas = nullptr;
        LevelCalibrationPage* level = nullptr;
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
    void save_models();            // step 3 → step 4 (Areas or Level calibration)
    void finish_whole_frame();     // terminal: no ROIs = whole-frame detection

    // Areas flow.
    void begin_areas_direct(const camera::Camera& cam);  // per-row Areas button
    void save_areas(const std::vector<camera::CameraArea>& areas);  // persist + list
    void areas_back();             // Areas Back: direct→list, wizard→Models step

    // Level-calibration flow (ball leveler).
    /// Persist the camera's complete Ball Leveler configuration — the model
    /// binding chosen at step 3 AND this geometry — through the ONE Ball write
    /// chokepoint, then finish setup.
    void save_level_calibration(const std::vector<denso::level::LevelZone>& zones);
    void level_back();             // Level Back → Models step

private:
    void push_used_sources();      // tag scan results already owned by other cams
    void open_configure(const QString& preview_text);  // seed Configure from draft_
    void enter_models();           // load catalog + attachments → Models page
    void enter_areas(bool direct); // load areas + frame → Areas page
    void enter_level(bool direct = false);            // load calibration + frame → Level page
    /// True when the COMMITTED mode is the ball leveler, i.e. step 4 is level
    /// calibration and the binding belongs to the level repository.
    bool ball_mode() const;
    QImage oriented_frame() const;  // last_frame_ with the draft's orientation
    void update_areas_background(); // push the oriented frame to the Areas canvas
    void update_level_background(); // …and to the Level canvas
    /// True only when a live frame exists AND it still matches draft_'s aspect —
    /// the two conditions for confirming quarantined ROIs against the view.
    bool preview_verifies_draft() const;
    bool save_models_only();       // persist model attachments; false = failed
    /// Mark the add wizard finished. ONLY after the write it completes has
    /// succeeded — completing first would activate a camera whose setup didn't
    /// land. No-op when editing an already-finished camera.
    bool finish_setup(QWidget* parent);
    /// Complete the setup, notify, and return to the list ONLY if completion
    /// succeeded. The single owner of that sequence — call it after a terminal
    /// action's own write has succeeded.
    void finish_and_leave(QWidget* parent);
    /// Forget any per-run wizard state. Called by every entry point so a second
    /// camera can never inherit the first one's in-progress choices.
    void reset_run_state();

    QSqlDatabase db_;
    Pages pages_;
    std::function<void(int)> show_page_;

    std::optional<int64_t> editing_id_;  // set in edit mode; empty when adding
    camera::Camera original_;            // pre-edit camera (for source-change diff)
    camera::Camera draft_;               // camera being added/edited
    QImage last_frame_;                  // most recent un-rotated snapshot frame
    // Owns BOTH "newest request wins" and whether last_frame_ is a live view of
    // the current source. `last_frame_` alone cannot answer the latter: it holds
    // the previous success after a FAILED refresh, which used to let a
    // quarantined camera's ROIs be "verified" against a stale image.
    PreviewGate preview_;
    // The resolution the live frame was captured at. Only width/height are used
    // (via camera::aspect_changed) — a Camera purely to reuse that tested pure
    // predicate rather than re-deriving aspect comparison here.
    camera::Camera captured_;
    bool entered_areas_directly_ = false;  // true: per-row Areas (Back → list)
    bool entered_level_directly_ = false;  // true: per-row Level (Back -> list)

    // The model binding chosen at step 3, held in memory until the calibration is
    // saved. Deliberately NOT written when it is chosen: the Ball chokepoint takes
    // the binding and the geometry together in one transaction, so persisting the
    // model early would create a second, partial authority and could leave a
    // camera bound to a model it has no calibration for.
    std::optional<denso::level::LevelBinding> ball_binding_;
    // The stored calibration is read into the page ONCE per wizard run. Re-reading
    // it on every visit would silently discard an in-progress calibration the
    // moment the operator stepped back to change the model.
    bool level_loaded_ = false;
};

} // namespace denso::ui
