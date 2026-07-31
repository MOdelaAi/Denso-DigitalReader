// Ball Leveler calibration: the value type and its validation. PURE — no Qt, no
// SQL, no OpenCV, no inference backend — so every rejection cause is unit-testable
// without a device, a display or a database.
//
// Geometry is expressed in NORMALIZED oriented-frame coordinates ([0,1] fractions
// of the oriented image width/height), exactly like camera::Point, so a
// calibration is resolution-independent and lands on the same physical spot at any
// capture or display size. Y increases DOWNWARD, so the 100% line has the SMALLER
// y value.
//
// This header holds no model/mode rule. Model authorization is the central
// policy's job (models/compatibility.h) and is applied by level/repo.
#pragma once

#include <string>

namespace denso::level {

/// Minimum separation between the reference lines, normalized. Below this the
/// percentage denominator is too small to divide by safely: a one-pixel detection
/// jitter would swing the reported level by a large fraction. Documented and
/// asserted rather than an unnamed literal.
inline constexpr double kMinSpanNorm = 0.02;

/// Defaults the calibration UI seeds a fresh configuration with.
inline constexpr double kDefaultConf = 0.5;
inline constexpr int kDefaultHoldMs = 2000;

/// How many measurement zones ONE Ball Leveler camera may own.
///
/// This is a BALL rule, not a machine rule, and the distinction matters. The
/// zone-number NAMESPACE is machine-wide and shared with the digit reader
/// (camera::kMaxZone); this is a per-camera COUNT. It is deliberately not pushed
/// into camera::area_validation, which the digit reader shares and which has no
/// such cap — a digit camera may own as many zones as there are numbers.
///
/// Enforced in level::save_level_configuration, the one Ball write chokepoint,
/// because a count is a property of the whole SET and only the chokepoint sees
/// the whole set inside one transaction. A per-row database CHECK cannot state
/// it.
inline constexpr int kMaxBallZones = 4;

/// One camera's measurement geometry. `rect_*` is the axis-aligned measurement
/// rectangle; `y_100` / `y_0` are the horizontal reference lines that define the
/// mapping. Both lines must lie inside the rectangle, and `y_100 < y_0`.
///
/// The reference lines are positions of the DETECTED BALL CENTRE, not of the
/// visible liquid surface. The calibration UI must say so: with a centre reference
/// point, lines placed at the liquid surface introduce a constant radius-sized
/// offset.
struct LevelCalibration {
    double rect_x = 0.0;
    double rect_y = 0.0;
    double rect_w = 0.0;
    double rect_h = 0.0;
    double y_100 = 0.0;   ///< 100% reference line (smaller Y — higher on screen)
    double y_0 = 0.0;     ///< 0% reference line
    double conf = kDefaultConf;
    int hold_ms = kDefaultHoldMs;
};

/// One measurement zone: its machine-wide reporting number and its geometry.
///
/// The zone carries NO model. That is the point of the v15 two-table split — the
/// model is a camera-level fact, and a zone that could name one would be a zone
/// that could disagree with its siblings.
///
/// It lives in this PURE header, not in level/repo.h, so the per-zone evaluation
/// step and the overlay can take a zone set without dragging QtSql in behind it.
struct LevelZone {
    int zone_no = 0;
    LevelCalibration calibration;
};

/// The verdict on a calibration. `reason_code` is a stable STRING code and a FILE
/// FORMAT (it reaches status.json and operator-visible refusals): never rename,
/// never reuse, only add. Empty exactly when `ok`.
struct CalibrationCheck {
    bool ok = false;   ///< fail-closed default: a path that forgets to assign rejects
    std::string reason_code;
};

/// Validate a calibration. Evaluation order is a CONTRACT — the first matching
/// failure wins, so a doubly-faulted calibration reports the most fundamental
/// cause. Do not reorder for convenience:
///
///   1. calib_not_finite         — any NaN/Inf field
///   2. calib_rect_degenerate    — non-positive width/height, or a rect leaving [0,1]
///   3. calib_conf_out_of_range  — confidence outside (0,1]
///   4. calib_hold_invalid       — negative hold
///   5. calib_lines_reversed     — y_100 >= y_0 (includes coincident lines)
///   6. calib_span_too_small     — 0 < y_0 - y_100 < kMinSpanNorm
///   7. calib_line_outside_rect  — either line outside the rectangle's Y band
CalibrationCheck validate_calibration(const LevelCalibration& c);

}  // namespace denso::level
