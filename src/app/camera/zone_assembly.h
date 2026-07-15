// Turn detected digit boxes into per-zone numbers. Digits inside a zone's ROI
// polygon are sorted left-to-right (by box x-center) and concatenated into an
// integer. Pure — OpenCV value types + core point-in-polygon, no Qt/network — so
// it unit-tests without a camera. Used by DetectionProcessor on the capture
// thread.
#pragma once

#include "camera/camera.h"                                 // CameraArea
#include "brazing/zone_reading.h"                   // ZoneReading
#include "detection/merge_detections.h"   // NamedDetection

#include <optional>
#include <vector>

namespace denso::ui {

/// Concatenate the digits (each detection's `name`, a single 0-9 char) in
/// left-to-right box order and parse to int. nullopt when empty or unparseable.
std::optional<int> assemble_zone_value(const std::vector<NamedDetection>& digits_in_zone);

/// For each area with a zone number, collect the kept digits whose box center
/// (normalized by frame_w/frame_h) lies inside the area polygon, assemble, and
/// emit a ZoneReading. Areas with no zone number, or that assemble to nothing,
/// are skipped. `conf` is the min digit confidence in the zone.
std::vector<ZoneReading> group_into_zones(const std::vector<NamedDetection>& kept,
                                          const std::vector<camera::CameraArea>& areas,
                                          float frame_w, float frame_h);

} // namespace denso::ui
