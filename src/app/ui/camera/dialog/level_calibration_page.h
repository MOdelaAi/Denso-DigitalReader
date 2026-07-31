// The wizard's Ball Leveler "Level calibration" step: draw the measurement
// rectangle, place the 100% and 0% ball-centre lines, set the detection
// confidence and the hold time.
//
// A thin driver over level::CalibrationDraft. The page turns canvas gestures and
// spin-box edits into draft mutations and paints `draft()` back — it holds NO
// geometry rule of its own. Every constraint (both lines inside the rectangle,
// 100% above 0%, the minimum span, the defaults) lives in the draft, and the
// enable/refuse decision for Save is the draft's `check()`, which is the SAME
// validator level::save_level_configuration runs. One rule, one place: Save can
// never be offered for something the write would refuse, and the write can never
// refuse something the page called ready.
//
// The page owns no DB access. It emits save_requested with the calibration; the
// wizard controller persists it through the one Ball write chokepoint.
#pragma once

#include "level/calibration.h"
#include "level/edit.h"

#include <QImage>
#include <QWidget>

#include <optional>

class QDoubleSpinBox;
class QLabel;
class QPushButton;
class QSpinBox;

namespace denso::ui {

class LevelCanvas;

class LevelCalibrationPage : public QWidget {
    Q_OBJECT

public:
    explicit LevelCalibrationPage(QWidget* parent = nullptr);

    /// Begin editing. `saved` resumes a stored configuration; nullopt starts a
    /// fresh one seeded with the shared defaults. Resuming is LOSSLESS — the
    /// stored value is adopted whole, so merely opening this page cannot alter
    /// what is stored.
    void load(const std::optional<denso::level::LevelCalibration>& saved);

    void set_background(const QImage& oriented);  // canvas backdrop
    void show_save_error();                       // persistence failed
    /// Name a refusal the write chokepoint reported (its stable reason code).
    void show_refusal(const QString& reason_code);

    const denso::level::CalibrationDraft& draft() const { return draft_; }

    /// Has the operator changed anything since load()?
    ///
    /// Compared against the snapshot taken at load, not against "a rectangle
    /// exists": resuming a stored calibration and touching nothing must NOT
    /// count as dirty, or every visit would warn on the way out.
    bool is_dirty() const;

    /// Ask before discarding unsaved edits, mirroring the Areas step. Returns
    /// true when it is safe to leave (nothing unsaved, or the operator
    /// confirmed). `action` names the button that triggered it, so the prompt
    /// says what is about to happen.
    bool confirm_discard(const QString& action);
    LevelCanvas* canvas() const { return canvas_; }

signals:
    void back_requested();
    void save_requested(const denso::level::LevelCalibration& calibration);

private:
    void sync();          // draft → canvas, controls, status, Save enablement
    void attempt_save();  // re-check, then emit — a disabled button is not a gate

    LevelCanvas* canvas_ = nullptr;
    QPushButton* redraw_btn_ = nullptr;
    QPushButton* save_btn_ = nullptr;
    QDoubleSpinBox* conf_ = nullptr;
    QSpinBox* hold_ = nullptr;
    QLabel* status_ = nullptr;

    denso::level::CalibrationDraft draft_;
    // The calibration as LOADED, for the dirty check. Held as an optional so a
    // fresh page (no stored configuration) is distinguishable from a resumed one
    // - drawing the first rectangle on a fresh page IS a change worth warning
    // about, while a resumed page that has not moved is not.
    std::optional<denso::level::LevelCalibration> loaded_;
    bool loaded_had_rect_ = false;
};

} // namespace denso::ui
