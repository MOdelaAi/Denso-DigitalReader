#include "level/edit.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace denso::level {
namespace {

double clamp01(double v) { return std::max(0.0, std::min(1.0, v)); }

// Separating the two reference lines has to leave a span the VALIDATOR accepts,
// and the validator MEASURES the span as `y_0 - y_100`. Plain arithmetic does not
// guarantee that: 0.15 - 0.02 == 0.13, but 0.15 - 0.13 == 0.0199999999999999969,
// which is below kMinSpanNorm. The wizard's Save button is gated on that
// measurement, so the gap between "computed far enough apart" and "measures far
// enough apart" is an ordinary legal drag that leaves Save greyed out, blaming the
// operator for lines they placed correctly.
//
// So the two helpers below VERIFY rather than assume, stepping by the smallest
// representable amount until the measurement agrees. Each terminates after one or
// two steps — the shortfall is always within a couple of ULPs.

/// The smallest `y_0` whose measured span from `y_100` is at least kMinSpanNorm.
double lowest_y0_for(double y_100) {
    double y0 = y_100 + kMinSpanNorm;
    while (y0 - y_100 < kMinSpanNorm) {
        y0 = std::nextafter(y0, std::numeric_limits<double>::infinity());
    }
    return y0;
}

/// The largest `y_100` whose measured span to `y_0` is at least kMinSpanNorm.
double highest_y100_for(double y_0) {
    double y100 = y_0 - kMinSpanNorm;
    while (y_0 - y100 < kMinSpanNorm) {
        y100 = std::nextafter(y100, -std::numeric_limits<double>::infinity());
    }
    return y100;
}

}  // namespace

CalibrationDraft::CalibrationDraft() {
    // Seeded from the shared defaults, never from magic numbers here, so the page
    // and the persistence layer cannot drift apart.
    c_.conf = kDefaultConf;
    c_.hold_ms = kDefaultHoldMs;
}

CalibrationDraft CalibrationDraft::from_calibration(const LevelCalibration& c) {
    CalibrationDraft d;
    d.c_ = c;   // assigned WHOLE, deliberately: routing a stored value through
                // the clamping mutators could nudge it, and opening the page
                // must never alter what is stored.
    // Whether a rectangle EXISTS is read from the value, not assumed. This was
    // an unconditional `true`, which was accurate while the only caller passed a
    // calibration loaded from the database — those always carry a drawn
    // rectangle, because the write chokepoint validated one. Multi-zone added a
    // second caller: an operator adding a zone gets a DEFAULT-constructed
    // LevelCalibration, whose rectangle is 0x0 because nothing has been drawn
    // yet. Claiming a rectangle for it put the canvas into Editing mode on a new
    // zone, where a press looks for a reference line to grab instead of starting
    // a band — so the operator could not draw the rectangle at all, and got a
    // geometry error in place of the "drag out the rectangle" prompt.
    //
    // The test is deliberately only "are the extents real", not the full
    // validation: a rectangle drawn too small to measure through HAS been drawn,
    // and must reach the operator as that specific complaint rather than as a
    // blank page.
    d.has_rect_ = std::isfinite(c.rect_w) && std::isfinite(c.rect_h) &&
                  c.rect_w > 0.0 && c.rect_h > 0.0;
    return d;
}

void CalibrationDraft::set_rect(double x, double y, double w, double h) {
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(w) ||
        !std::isfinite(h)) {
        return;
    }
    if (w <= 0.0 || h <= 0.0) return;

    // Clamp the rectangle into the frame, keeping the origin ahead of the extent
    // so a drag past the edge yields a smaller rectangle rather than an inverted
    // one.
    const double x0 = clamp01(x);
    const double y0 = clamp01(y);
    const double x1 = clamp01(x + w);
    const double y1 = clamp01(y + h);
    if (x1 <= x0 || y1 <= y0) return;

    const bool first = !has_rect_;
    const double old_top = c_.rect_y;
    const double old_h = c_.rect_h;

    c_.rect_x = x0;
    c_.rect_y = y0;
    c_.rect_w = x1 - x0;
    c_.rect_h = y1 - y0;

    if (first) {
        // Seed the lines at sensible thirds so the operator starts from a legal,
        // visible calibration rather than two coincident lines on the top edge.
        c_.y_100 = c_.rect_y + c_.rect_h / 3.0;
        c_.y_0 = c_.rect_y + c_.rect_h * 2.0 / 3.0;
        has_rect_ = true;
    } else if (old_h > 0.0) {
        // Preserve the lines' RELATIVE position within the band, so resizing the
        // rectangle moves the calibration with it instead of destroying it.
        const double f100 = (c_.y_100 - old_top) / old_h;
        const double f0 = (c_.y_0 - old_top) / old_h;
        c_.y_100 = c_.rect_y + f100 * c_.rect_h;
        c_.y_0 = c_.rect_y + f0 * c_.rect_h;
    }
    reseat_lines();
}

void CalibrationDraft::reseat_lines() {
    const double top = c_.rect_y;
    const double bottom = c_.rect_y + c_.rect_h;

    c_.y_100 = std::max(top, std::min(bottom, c_.y_100));
    c_.y_0 = std::max(top, std::min(bottom, c_.y_0));

    // Restore the ordering + minimum span. If the band itself is too short to
    // hold kMinSpanNorm the lines are pushed to its extremes; check() then
    // reports calib_span_too_small, which is the honest answer — a rectangle that
    // small cannot be measured through.
    if (c_.y_0 - c_.y_100 < kMinSpanNorm) {
        if (bottom - top < kMinSpanNorm) {
            c_.y_100 = top;
            c_.y_0 = bottom;
            return;
        }
        c_.y_0 = lowest_y0_for(c_.y_100);
        if (c_.y_0 > bottom) {
            c_.y_0 = bottom;
            c_.y_100 = highest_y100_for(bottom);
            // The band is within an ULP of the minimum: pinning to its extremes
            // is then both legal and the only fit, because `bottom - top` was
            // measured above to be at least kMinSpanNorm.
            if (c_.y_100 < top) c_.y_100 = top;
        }
    }
}

void CalibrationDraft::set_y_100(double y) {
    if (!std::isfinite(y) || !has_rect_) return;
    c_.y_100 = y;
    // Push the partner line rather than refusing the drag, so the handle always
    // follows the pointer.
    if (c_.y_0 - c_.y_100 < kMinSpanNorm) c_.y_0 = lowest_y0_for(c_.y_100);
    reseat_lines();
}

void CalibrationDraft::set_y_0(double y) {
    if (!std::isfinite(y) || !has_rect_) return;
    c_.y_0 = y;
    if (c_.y_0 - c_.y_100 < kMinSpanNorm) c_.y_100 = highest_y100_for(c_.y_0);
    reseat_lines();
}

void CalibrationDraft::set_conf(double conf) {
    if (!std::isfinite(conf)) return;
    c_.conf = conf;   // NOT clamped: check() names an out-of-range confidence, and
                      // silently rewriting the operator's number would hide it.
}

void CalibrationDraft::set_hold_ms(int hold_ms) { c_.hold_ms = hold_ms; }

CalibrationCheck CalibrationDraft::check() const {
    // The SAME validator the write chokepoint uses. One rule, one place: the page
    // can never enable Save for something the save would refuse.
    return validate_calibration(c_);
}

}  // namespace denso::level
