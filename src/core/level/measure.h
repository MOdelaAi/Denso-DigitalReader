// Ball Leveler measurement: percentage mapping and detection selection. PURE —
// no Qt, no SQL, no OpenCV, no inference backend — so both the arithmetic and the
// choice of "which ball" are unit-testable without a device, a display or a
// database. The processor (Phase B) supplies candidates and consumes the result;
// it owns all the framework types, this unit owns none of them.
//
// Everything here is in NORMALIZED oriented-frame coordinates ([0,1] fractions of
// the oriented image width/height), matching level::LevelCalibration and
// camera::Point. Y increases DOWNWARD, so the 100% line has the SMALLER y.
#pragma once

#include "level/calibration.h"

#include <cstddef>
#include <optional>
#include <vector>

namespace denso::level {

/// One detection offered to the selector. Deliberately a plain box + confidence
/// rather than the backend's detection type: this unit must not depend on the
/// inference stack, and the selector needs nothing else. Class filtering is NOT
/// done here — the bound class comes from the durable configuration
/// (`ball_level_calibration.class_id`) and the caller has already narrowed to it.
struct BallBox {
    double x1 = 0.0;
    double y1 = 0.0;
    double x2 = 0.0;
    double y2 = 0.0;
    double conf = 0.0;

    /// The measurement reference point: the bbox's VERTICAL CENTRE. The reference
    /// lines are calibrated against the ball centre, not the liquid surface, so
    /// the two must agree or every reading carries a constant radius-sized offset.
    constexpr double centre_y() const { return (y1 + y2) / 2.0; }
    constexpr double centre_x() const { return (x1 + x2) / 2.0; }
};

/// The selected detection and where it came from in the input.
struct BallChoice {
    BallBox box;
    std::size_t index = 0;   ///< index into the candidate vector, for diagnostics
};

/// Map a ball-centre Y to a level percentage in [0, 100].
///
/// `y_0` reads 0% and `y_100` reads 100%; between them the mapping is linear, and
/// OUTSIDE them it is CLAMPED. Clamping is deliberate and not a papering-over: a
/// ball can genuinely sit above the 100% line or below the 0% line, and reporting
/// 103% or -4% would be a physically meaningless number that downstream code and
/// operators would both have to special-case.
///
/// Returns nullopt when the calibration does not validate or `centre_y` is not
/// finite — fail-closed, because every caller displays or reports this number and
/// there is no safe fallback value to invent. NEVER substitute 0.
std::optional<double> level_percent(const LevelCalibration& c, double centre_y);

/// Choose the ball to measure: the HIGHEST-CONFIDENCE candidate whose CENTRE lies
/// inside the measurement rectangle and whose confidence meets the calibration's
/// threshold.
///
/// Centre-inside rather than any-overlap, so a large box clipping the corner of
/// the rectangle cannot hijack a reading from the ball actually in the tank.
///
/// Tie-break: on equal confidence the EARLIEST candidate wins. That is a simple,
/// stable, deterministic rule — enough to make the result reproducible in a test
/// without an elaborate ordering hierarchy (Lean V1 amendment).
///
/// Returns nullopt when the calibration does not validate or no candidate
/// qualifies. "No ball" is a legitimate, common answer, not an error.
std::optional<BallChoice> select_ball(const std::vector<BallBox>& candidates,
                                      const LevelCalibration& c);

}  // namespace denso::level
