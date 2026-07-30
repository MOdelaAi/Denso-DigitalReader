#include "level/edit.h"

#include <algorithm>
#include <cmath>

namespace denso::level {
namespace {

double clamp01(double v) { return std::max(0.0, std::min(1.0, v)); }

}  // namespace

CalibrationDraft::CalibrationDraft() {
    // Seeded from the shared defaults, never from magic numbers here, so the page
    // and the persistence layer cannot drift apart.
    c_.conf = kDefaultConf;
    c_.hold_ms = kDefaultHoldMs;
}

CalibrationDraft CalibrationDraft::from_calibration(const LevelCalibration& c) {
    CalibrationDraft d;
    d.c_ = c;             // assigned WHOLE, deliberately: routing a stored value
    d.has_rect_ = true;   // through the clamping mutators could nudge it, and
                          // opening the page must never alter what is stored.
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
        c_.y_0 = c_.y_100 + kMinSpanNorm;
        if (c_.y_0 > bottom) {
            c_.y_0 = bottom;
            c_.y_100 = bottom - kMinSpanNorm;
        }
    }
}

void CalibrationDraft::set_y_100(double y) {
    if (!std::isfinite(y) || !has_rect_) return;
    c_.y_100 = y;
    // Push the partner line rather than refusing the drag, so the handle always
    // follows the pointer.
    if (c_.y_0 - c_.y_100 < kMinSpanNorm) c_.y_0 = c_.y_100 + kMinSpanNorm;
    reseat_lines();
}

void CalibrationDraft::set_y_0(double y) {
    if (!std::isfinite(y) || !has_rect_) return;
    c_.y_0 = y;
    if (c_.y_0 - c_.y_100 < kMinSpanNorm) c_.y_100 = c_.y_0 - kMinSpanNorm;
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
