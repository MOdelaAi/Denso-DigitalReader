#include "level/calibration.h"

#include <cmath>

namespace denso::level {
namespace {

CalibrationCheck reject(const char* code) {
    CalibrationCheck r;
    r.reason_code = code;
    return r;   // ok stays false
}

bool all_finite(const LevelCalibration& c) {
    return std::isfinite(c.rect_x) && std::isfinite(c.rect_y) &&
           std::isfinite(c.rect_w) && std::isfinite(c.rect_h) &&
           std::isfinite(c.y_100) && std::isfinite(c.y_0) && std::isfinite(c.conf);
}

}  // namespace

CalibrationCheck validate_calibration(const LevelCalibration& c) {
    // 1. Non-finite first: every comparison below is meaningless on a NaN, and a
    //    NaN comparison is false, so an unchecked NaN would slip past the ordering
    //    checks and be stored.
    if (!all_finite(c)) return reject("calib_not_finite");

    // 2. A degenerate or out-of-frame rectangle. Normalized coordinates must stay
    //    inside the frame, or the rectangle addresses pixels that do not exist.
    if (c.rect_w <= 0.0 || c.rect_h <= 0.0) return reject("calib_rect_degenerate");
    if (c.rect_x < 0.0 || c.rect_y < 0.0 || c.rect_x + c.rect_w > 1.0 ||
        c.rect_y + c.rect_h > 1.0) {
        return reject("calib_rect_degenerate");
    }

    // 3. Confidence. Zero would keep every detection including pure noise; above
    //    one can never match, which would silently disable the camera.
    if (c.conf <= 0.0 || c.conf > 1.0) return reject("calib_conf_out_of_range");

    // 4. Hold.
    if (c.hold_ms < 0) return reject("calib_hold_invalid");

    // 5. Ordering. Y increases downward, so 100% must be strictly ABOVE 0%, i.e.
    //    y_100 < y_0. Coincident lines land here too — they are the zero-span case
    //    and the reversed check is the more fundamental diagnosis.
    if (c.y_100 >= c.y_0) return reject("calib_lines_reversed");

    // 6. Span. Ordering is already known good, so this is purely "too close".
    if (c.y_0 - c.y_100 < kMinSpanNorm) return reject("calib_span_too_small");

    // 7. Both lines inside the rectangle's vertical band.
    const double top = c.rect_y;
    const double bottom = c.rect_y + c.rect_h;
    if (c.y_100 < top || c.y_100 > bottom || c.y_0 < top || c.y_0 > bottom) {
        return reject("calib_line_outside_rect");
    }

    CalibrationCheck ok;
    ok.ok = true;
    return ok;
}

}  // namespace denso::level
