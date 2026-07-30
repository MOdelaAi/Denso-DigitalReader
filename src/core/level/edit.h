// The calibration EDITING state machine that backs the wizard's Ball Leveler
// calibration page. PURE — no Qt, no widgets, no SQL, no OpenCV — so every
// constraint the operator can hit is unit-testable without a display.
//
// The page widget is a thin driver over this: it turns drags into set_rect /
// set_y_100 / set_y_0 calls and paints `draft()`. Keeping the rules here means a
// constraint cannot be enforced in the painter but forgotten in the keyboard
// path, which is exactly how the ROI editor once grew a "clicks gate but drags
// clamp" split.
//
// This type holds NO model/mode rule and NO persistence. Authorization is the
// central policy's job and the durable write goes through
// level::save_level_configuration — the one chokepoint. Nothing here is a second
// authority.
#pragma once

#include "level/calibration.h"

#include <optional>

namespace denso::level {

/// An in-progress calibration edit.
///
/// Invariant maintained by every mutator: `y_100 < y_0`, both lines lie within
/// the rectangle's vertical band, and the span is at least `kMinSpanNorm`.
/// A request that would break it is CLAMPED to the nearest legal position rather
/// than rejected — a drag that runs past the rectangle edge should stop at the
/// edge, not silently do nothing and leave the operator dragging a dead handle.
/// A request that cannot be clamped sensibly (a non-finite value) is IGNORED.
class CalibrationDraft {
public:
    /// A blank draft seeded with the UI defaults. The rectangle starts empty, so
    /// `ready()` is false until the operator draws one.
    CalibrationDraft();

    /// Resume editing a SAVED configuration. The round-trip must be lossless:
    /// loading and immediately reading back yields the identical calibration, so
    /// merely opening the page cannot alter what is stored.
    static CalibrationDraft from_calibration(const LevelCalibration& c);

    /// Replace the measurement rectangle. Non-finite or non-positive extents are
    /// ignored. The rectangle is clamped into the [0,1] frame; the two reference
    /// lines are then re-seated inside the new band, preserving their RELATIVE
    /// positions where possible, so moving the rectangle does not silently
    /// invalidate a calibration the operator already placed.
    void set_rect(double x, double y, double w, double h);

    /// Move the 100% line. Clamped into the rectangle and to stay at least
    /// `kMinSpanNorm` ABOVE the 0% line.
    void set_y_100(double y);

    /// Move the 0% line. Clamped into the rectangle and to stay at least
    /// `kMinSpanNorm` BELOW the 100% line.
    void set_y_0(double y);

    void set_conf(double conf);
    void set_hold_ms(int hold_ms);

    /// True once a rectangle has been drawn. Before that the reference lines have
    /// no band to live in and the draft is not measurable.
    bool has_rect() const { return has_rect_; }

    /// The current value. Only meaningful once `has_rect()`.
    const LevelCalibration& draft() const { return c_; }

    /// The same verdict the write chokepoint will reach, so the page can enable
    /// or explain Save without duplicating a single rule.
    CalibrationCheck check() const;
    bool ready() const { return has_rect_ && check().ok; }

private:
    void reseat_lines();

    LevelCalibration c_;
    bool has_rect_ = false;
};

}  // namespace denso::level
