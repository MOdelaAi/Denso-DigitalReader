// The wizard's "Models" step, class-centric: pick which models form the
// detection ensemble, then pick classes *once* (union of the chosen models'
// classes, by name) with one confidence each. Selections fan back out to the
// per-model schema by class name in selections(). Reads the SELECTABLE list +
// current attachments from detection::repo; the coordinator saves via
// detection::set_camera_models on Finish. Pure widget — owns its controls,
// emits requests, holds no business logic.
//
// COMPATIBILITY IS NOT THIS PAGE'S DECISION (spec §6.1). The list it renders comes
// from detection::selectable_models, which asks the one central policy. This page
// holds no model/mode rule, no matrix and no special case: it renders what it is
// given. That is why load_for takes the mode, the manifest view and the measured
// platform instead of reading them itself — the authorization inputs arrive from
// the controller, and the page cannot quietly choose different ones.
#pragma once

#include "detection/detection.h"
#include "mode/mode.h"              // TargetMode
#include "models/model_identity.h"  // ManifestView, PlatformInfo

#include <QSqlDatabase>
#include <QString>
#include <QWidget>

#include <cstdint>
#include <map>
#include <utility>
#include <vector>

class QVBoxLayout;
class QCheckBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;

namespace denso::ui {

class ModelsPage : public QWidget {
    Q_OBJECT
public:
    explicit ModelsPage(QWidget* parent = nullptr);
    ~ModelsPage() override;

    void set_db(QSqlDatabase db) { db_ = std::move(db); }

    /// Populate the step for `camera_id`. The offered models are exactly
    /// `detection::selectable_models(db, mode, view, platform)` — nothing more and
    /// nothing else. `mode` must be the COMMITTED database mode, never a settings
    /// selector's unconfirmed value (spec §6.3); the controller supplies it. None
    /// of the three has a default: a caller that forgets one must fail to compile
    /// rather than silently render an unfiltered list.
    void load_for(int64_t camera_id, denso::mode::TargetMode mode,
                  const denso::models::ManifestView& view,
                  const denso::models::PlatformInfo& platform);
    std::vector<denso::detection::CameraModel> selections(int64_t camera_id) const;

signals:
    void back_requested();
    /// Persist selections and go on to the Areas step. NOT a finish — the camera
    /// stays unfinished until areas are saved or whole-frame is chosen.
    void next_requested();
    /// Terminal: persist selections, finish setup, and detect on the whole frame
    /// (no ROI areas). Named for what it DOES, unlike the old finish_requested,
    /// which only advanced a step.
    void finish_whole_frame_requested();

private:
    void rebuild_class_list();  // union of the checked models' class names
    void apply_filter();        // show/hide class rows by the search text
    void set_visible_checked(bool on);  // check/uncheck every filtered-visible row

    QSqlDatabase db_;
    QVBoxLayout* models_layout_ = nullptr;  // ensemble model checkboxes
    QLineEdit* search_ = nullptr;
    QVBoxLayout* class_layout_ = nullptr;   // class rows
    // Shown INSTEAD of an unexplained blank page when the policy offers nothing.
    // Its text is built by the pure model_empty_state unit from the verdicts the
    // policy itself returned — this page still decides nothing about eligibility.
    QLabel* empty_state_ = nullptr;

    // The models this page may OFFER — the policy's answer for the committed mode,
    // cached for the class-list rebuilds. NOT the catalog: a rejected model is
    // absent from here, so it can be neither shown nor returned by selections().
    std::vector<denso::detection::DetectionModel> offered_;

    struct ModelCheck {
        int64_t model_id = 0;
        QCheckBox* on = nullptr;
    };
    std::vector<ModelCheck> model_checks_;

    struct ClassRow {
        QString name;
        QCheckBox* on = nullptr;
        QDoubleSpinBox* conf = nullptr;
        QWidget* row = nullptr;
    };
    std::vector<ClassRow> class_rows_;

    // Remembered class selections (name → {selected, conf}) so toggling a model
    // in/out of the ensemble preserves what the user set. Seeded from the DB in
    // load_for, folded from the live widgets on every rebuild.
    std::map<QString, std::pair<bool, double>> selected_state_;
};

} // namespace denso::ui
