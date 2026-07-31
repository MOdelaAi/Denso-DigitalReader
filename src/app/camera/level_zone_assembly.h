// Turn ONE camera's detection set into per-zone level values — the Ball Leveler
// counterpart of zone_assembly's group_into_zones, and the ONLY step that
// differs from the digit reader in kind rather than in geometry:
//
//     digit zone processor: detections -> digit assembly     -> numeric value
//     ball  zone processor: detections -> ball selection
//                                      -> percentage mapping -> numeric value
//
// Pure — the level core plus std, no Qt, no OpenCV, no SQL, no inference backend
// — so "one detection set, evaluated independently per zone" is directly
// assertable without a camera, a display or a database.
//
// The SAME candidate vector is handed to every zone. That is what makes one
// inference per camera frame structurally true here: this function has no way to
// run a model, so a caller cannot accidentally make it per-zone.
#pragma once

#include "brazing/zone_reading.h"   // ZoneReading, ReadingKind
#include "level/calibration.h"      // LevelZone, LevelCalibration
#include "level/measure.h"          // BallBox

#include <optional>
#include <vector>

namespace denso::ui {

/// One zone's evaluation of the shared detection set.
///
/// INVARIANT: `percent` and `ball` are either BOTH engaged or BOTH absent. A
/// percentage exists exactly when a ball was selected and its centre mapped
/// through a valid calibration, so the overlay cannot draw a box without a
/// number or a number without a box.
struct LevelZoneResult {
    int zone_no = 0;
    std::optional<double> percent;          ///< full precision, for the overlay
    std::optional<denso::level::BallBox> ball;  ///< the selected detection
};

/// Evaluate `candidates` INDEPENDENTLY for every zone in `zones`.
///
/// Independence is the point: a zone that selects nothing yields a result with
/// no percentage and does not touch, clear or suppress any sibling's. There is
/// exactly one result per configured zone, in the order given, so a caller can
/// never lose a zone by having it produce nothing.
///
/// Callers must have narrowed `candidates` to the bound class already —
/// level::select_ball does not filter by class, because the class comes from the
/// durable configuration and belongs to the caller.
std::vector<LevelZoneResult> evaluate_level_zones(
    const std::vector<denso::level::BallBox>& candidates,
    const std::vector<denso::level::LevelZone>& zones);

/// Quantize a percentage for the reporting seam: clamp to [0,100] and round to
/// the nearest whole percent (amendment §10.3).
///
/// Clamping is belt and braces — level_percent already clamps — but this is the
/// last point before an integer reaches the backend, and a NaN or an
/// out-of-range double arriving here must become a number the contract allows
/// rather than undefined behaviour in the cast. A non-finite input yields 0 and
/// must never be emitted as a reading; callers gate on ReadingKind, not on this.
int quantize_level_percent(double percent);

/// Build the readings for ONE camera from its per-zone results.
///
/// Every configured zone produces a reading — including zones that selected
/// nothing, which emit ReadingKind::NoValue. Emitting the empty ones is
/// load-bearing, exactly as it is for the digit reader: the aggregator uses a
/// zone's continued presence as its liveness signal, and a zone that simply
/// stopped appearing would EXPIRE (a different, slower path) instead of being
/// recognised as present-but-not-reading.
std::vector<ZoneReading> level_zone_readings(
    const std::vector<LevelZoneResult>& results);

} // namespace denso::ui
