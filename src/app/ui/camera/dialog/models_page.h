// The wizard's "Models" step. It renders in one of two SHAPES, because the two
// jobs the appliance does bind models differently:
//
//  * Ensemble — pick which models form the detection ensemble, then pick classes
//    *once* (union of the chosen models' classes, by name) with one confidence
//    each. Selections fan back out to the per-model schema by class name in
//    selections().
//  * Single — bind exactly ONE model and ONE class. Multiple models, multiple
//    classes and zero classes are not representable: the widgets are exclusive,
//    so an invalid binding cannot be built here and then refused by the write.
//
// The shape is the CALLER's decision (set_selection_mode), never this page's. It
// is a rendering of the binding CARDINALITY the owning domain already enforces at
// its own write chokepoint — not a rule, and not a second authority.
//
// Reads the SELECTABLE list + current attachments from detection::repo; the
// coordinator saves via detection::set_camera_models on Finish. Pure widget —
// owns its controls, emits requests, holds no business logic.
//
// COMPATIBILITY IS NOT THIS PAGE'S DECISION (spec §6.1). The list it renders comes
// from detection::evaluated_models, which asks the one central policy. This page
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

class QAbstractButton;
class QVBoxLayout;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QPushButton;

namespace denso::ui {

class ModelsPage : public QWidget {
    Q_OBJECT
public:
    /// How many models and classes a binding may carry. See the file comment:
    /// this is cardinality, not authorization.
    enum class SelectionMode {
        Ensemble,  ///< N models, N classes, a confidence per class
        Single,    ///< exactly one model and one class; no confidence here
    };

    explicit ModelsPage(QWidget* parent = nullptr);
    ~ModelsPage() override;

    void set_db(QSqlDatabase db) { db_ = std::move(db); }

    /// Choose the binding shape. Takes effect on the NEXT load_for — the page is
    /// created once and reused for the application's lifetime, so the shape is
    /// applied where every other per-camera decision is applied, and a stale
    /// half-rebuilt page can never be shown.
    void set_selection_mode(SelectionMode mode) { selection_mode_ = mode; }
    SelectionMode selection_mode() const { return selection_mode_; }

    /// Populate the step for `camera_id`. The offered models are exactly the rows
    /// `detection::evaluated_models(db, mode, view, platform)` reports as allowed
    /// — nothing more and nothing else. `mode` must be the COMMITTED database
    /// mode, never a settings selector's unconfirmed value (spec §6.3); the
    /// controller supplies it. None of the three has a default: a caller that
    /// forgets one must fail to compile rather than silently render an unfiltered
    /// list.
    ///
    /// In Single mode the per-camera `camera_model` attachments are deliberately
    /// NOT read: that table is the ensemble domain's authority, and a page that
    /// seeded one domain's step from the other's rows would be a second binding
    /// authority. The caller seeds the stored choice with select_single().
    void load_for(int64_t camera_id, denso::mode::TargetMode mode,
                  const denso::models::ManifestView& view,
                  const denso::models::PlatformInfo& platform);

    /// Single mode: show `model_id`/`class_id` as the current choice. A model that
    /// is not offered, or a class the model does not declare, selects NOTHING —
    /// silently pre-selecting a substitute would be a worse lie than an empty step.
    /// No-op in Ensemble mode. Call after load_for.
    void select_single(int64_t model_id, int class_id);

    std::vector<denso::detection::CameraModel> selections(int64_t camera_id) const;

signals:
    void back_requested();
    /// Persist selections and go on to the next step. NOT a finish — the camera
    /// stays unfinished until that step's own terminal action.
    void next_requested();
    /// Terminal: persist selections, finish setup, and detect on the whole frame
    /// (no ROI areas). Named for what it DOES, unlike the old finish_requested,
    /// which only advanced a step. Offered in Ensemble mode only.
    void finish_whole_frame_requested();

private:
    void rebuild_class_list();  // union of the checked models' class names
    void apply_filter();        // show/hide class rows by the search text
    void set_visible_checked(bool on);  // check/uncheck every filtered-visible row
    void apply_selection_mode();        // show/label the controls for the shape
    bool single() const { return selection_mode_ == SelectionMode::Single; }

    QSqlDatabase db_;
    SelectionMode selection_mode_ = SelectionMode::Ensemble;
    QLabel* models_heading_ = nullptr;
    QVBoxLayout* models_layout_ = nullptr;  // model checkboxes / radio buttons
    QLabel* classes_heading_ = nullptr;
    QWidget* filter_row_ = nullptr;         // search + Select all + Clear
    QLineEdit* search_ = nullptr;
    QVBoxLayout* class_layout_ = nullptr;   // class rows
    QPushButton* whole_frame_btn_ = nullptr;
    QPushButton* next_btn_ = nullptr;
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
        QAbstractButton* on = nullptr;  // QCheckBox (Ensemble) / QRadioButton (Single)
    };
    std::vector<ModelCheck> model_checks_;

    struct ClassRow {
        QString name;
        QAbstractButton* on = nullptr;
        QDoubleSpinBox* conf = nullptr;  ///< null in Single mode — see selections()
        QWidget* row = nullptr;
    };
    std::vector<ClassRow> class_rows_;

    // Remembered class selections (name → {selected, conf}) so toggling a model
    // in/out of the ensemble preserves what the user set. Seeded from the DB in
    // load_for, folded from the live widgets on every rebuild.
    std::map<QString, std::pair<bool, double>> selected_state_;
};

} // namespace denso::ui
