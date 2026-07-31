// The wizard's Ball Leveler "Level zones" step: define 1..kMaxBallZones
// measurement zones on one camera. Each zone is a rectangle, a 100% and a 0%
// ball-centre line, a detection confidence and a hold time.
//
// The step follows the digit reader's Areas UX deliberately and closely — a zone
// list, Add Zone, Delete Zone, select-to-edit, a reporting-zone picker that
// shows numbers other cameras already own, the same dirty-page warning and the
// same Save/Back semantics. An operator who can configure Areas can configure
// this. What differs is only the SHAPE being drawn: an axis-aligned rectangle
// with two reference lines instead of a polygon.
//
// A thin driver over level::CalibrationDraft, one draft at a time for the
// SELECTED zone. The page turns canvas gestures and spin-box edits into draft
// mutations and paints `draft()` back — it holds NO geometry rule of its own.
// Every constraint (both lines inside the rectangle, 100% above 0%, the minimum
// span, the defaults) lives in the draft, and the enable/refuse decision for
// Save is the draft's `check()`, which is the SAME validator
// level::save_level_configuration runs. One rule, one place: Save can never be
// offered for something the write would refuse, and the write can never refuse
// something the page called ready.
//
// The page owns no DB access. It emits save_requested with the complete zone
// set; the wizard controller persists it through the one Ball write chokepoint,
// which writes all zones or none.
#pragma once

#include "level/calibration.h"
#include "level/edit.h"

#include <QImage>
#include <QWidget>

#include <map>
#include <optional>
#include <string>
#include <vector>

class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QListWidget;
class QPushButton;
class QSpinBox;

namespace denso::ui {

class LevelCanvas;

class LevelCalibrationPage : public QWidget {
    Q_OBJECT

public:
    explicit LevelCalibrationPage(QWidget* parent = nullptr);

    /// Begin editing. `saved` resumes a stored zone set; an empty vector starts a
    /// fresh configuration with one zone seeded from the shared defaults.
    /// Resuming is LOSSLESS — each stored calibration is adopted whole, so merely
    /// opening this page cannot alter what is stored.
    ///
    /// `zones_taken` maps zone number → owning camera name for every number
    /// claimed by ANOTHER camera in EITHER mode (see
    /// camera::zones_owned_by_other_cameras). Taken numbers are shown disabled
    /// and named, so the operator cannot build a save the repo would reject.
    void load(std::vector<denso::level::LevelZone> saved,
              std::map<int, std::string> zones_taken);

    void set_background(const QImage& oriented);  // canvas backdrop
    void show_save_error();                       // persistence failed
    /// Name a refusal the write chokepoint reported (its stable reason code),
    /// attributed to a zone when the refusal names one.
    void show_refusal(const QString& reason_code, int zone_no = 0);

    /// The SELECTED zone's draft. Test seam and canvas source.
    const denso::level::CalibrationDraft& draft() const { return draft_; }

    /// The working zone set, with the selected zone's live draft folded in.
    std::vector<denso::level::LevelZone> zones() const;

    /// Has the operator changed anything since load()?
    ///
    /// Compared against the snapshot taken at load, not against "a rectangle
    /// exists": resuming a stored set and touching nothing must NOT count as
    /// dirty, or every visit would warn on the way out.
    bool is_dirty() const;

    /// Ask before discarding unsaved edits, mirroring the Areas step. Returns
    /// true when it is safe to leave (nothing unsaved, or the operator
    /// confirmed). `action` names the button that triggered it, so the prompt
    /// says what is about to happen.
    bool confirm_discard(const QString& action);
    LevelCanvas* canvas() const { return canvas_; }

signals:
    void back_requested();
    /// The COMPLETE zone set. Never a single zone: the write chokepoint takes
    /// the whole set in one transaction, and a per-zone signal would invite a
    /// per-zone write that could persist a set the chokepoint would have
    /// refused.
    void save_requested(const std::vector<denso::level::LevelZone>& zones);

private:
    void sync();            // draft → canvas, controls, status, Save enablement
    void refresh_list();    // rebuild the zone list rows
    void select_zone(int row);
    void commit_selected(); // fold the live draft back into zones_[selected_]
    void add_zone();
    void delete_selected();
    void rebuild_zone_choices();
    void sync_zone_combo(int zone_no);
    /// The lowest zone number free for a NEW zone — not claimed by another
    /// camera and not already used by this camera's own zones. 0 when the
    /// namespace is exhausted.
    int first_free_zone_no() const;
    void attempt_save();    // re-check, then emit — a disabled button is not a gate
    /// The first zone whose geometry does not validate, or nullopt when the whole
    /// set is ready. Save is gated on this, so the button can never offer a set
    /// the chokepoint would roll back.
    std::optional<int> first_invalid_zone() const;

    LevelCanvas* canvas_ = nullptr;
    QListWidget* list_ = nullptr;
    QComboBox* zone_combo_ = nullptr;
    QPushButton* add_btn_ = nullptr;
    QPushButton* delete_btn_ = nullptr;
    QPushButton* redraw_btn_ = nullptr;
    QPushButton* save_btn_ = nullptr;
    QDoubleSpinBox* conf_ = nullptr;
    QSpinBox* hold_ = nullptr;
    QLabel* status_ = nullptr;

    /// The working set. The SELECTED entry's calibration is authoritative only
    /// after commit_selected() folds `draft_` back into it — everywhere else,
    /// read it through zones().
    std::vector<denso::level::LevelZone> zones_;
    int selected_ = -1;
    denso::level::CalibrationDraft draft_;   // the selected zone's live geometry
    std::map<int, std::string> zones_taken_; // zone → OTHER camera's name

    /// The set as LOADED, for the dirty check. An empty loaded set is
    /// distinguishable from a resumed one — drawing the first rectangle on a
    /// fresh page IS a change worth warning about, while a resumed page that has
    /// not moved is not.
    std::vector<denso::level::LevelZone> loaded_;
};

} // namespace denso::ui
