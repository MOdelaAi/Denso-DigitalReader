// Turn detected digit boxes into per-zone numbers. Digits inside a zone's ROI
// polygon are sorted left-to-right (by box x-center) and concatenated into an
// integer. Pure — OpenCV value types + core point-in-polygon, no Qt/network — so
// it unit-tests without a camera. Used by DetectionProcessor on the capture
// thread.
#pragma once

#include "camera/camera.h"                                 // CameraArea
#include "brazing/zone_reading.h"                   // ZoneReading
#include "detection/merge_detections.h"   // NamedDetection

#include <vector>

namespace denso::ui {

/// Interim digit-completeness constants (spec §5.2). Pitch is estimated from box
/// HEIGHT, not width: "1" is much narrower than other glyphs, but seven-segment
/// digits share a consistent height. Tuned on-device in slice (b2).
constexpr float kPitchPerHeight = 0.70f;
constexpr float kGapFactor      = 1.60f;

struct ZoneAssembly {
    ReadingKind kind  = ReadingKind::NoValue;
    int         value = 0;   // meaningful ONLY when kind == Complete
};

/// Assemble the digits in one zone. Returns Complete only when the digits form a
/// plausible contiguous number. LIMITATION (spec §5.2): this detects only an
/// obvious INTERNAL missing position. A missing leading digit, a missing
/// trailing digit, and a single-remaining-detection all return Complete with a
/// shorter value — they are geometrically indistinguishable from genuine short
/// readings and need slice (b2)'s calibrated slots.
ZoneAssembly assemble_zone_value(const std::vector<NamedDetection>& digits_in_zone);

/// For each area with a zone number, collect the kept digits whose box center
/// (normalized by frame_w/frame_h) lies inside the area polygon, assemble, and
/// emit a ZoneReading for EVERY such area (including NoValue — the aggregator
/// needs the liveness signal). `conf` is the min digit confidence in the zone
/// (0 when there were none).
std::vector<ZoneReading> group_into_zones(const std::vector<NamedDetection>& kept,
                                          const std::vector<camera::CameraArea>& areas,
                                          float frame_w, float frame_h);

} // namespace denso::ui
