// Zone values BURNED INTO the displayed frame, with the same OpenCV primitives
// the detection boxes use. The operator reads the numbers off the camera image
// itself, so the annotation travels with the picture rather than sitting in a
// widget layered over it.
//
// Pure: OpenCV only. No Qt, no ZoneReporter, no backend. The caller supplies the
// rows for ONE camera — this code never resolves a projection, so it cannot mix
// two cameras that happen to share a zone number.
//
// Lives in denso_camera beside zone_assembly for the same reason: it is
// frame-side, not brazing-side, and consumed by frame_processor.
#pragma once

#include "brazing/zone_runtime.h"   // ZoneRuntimeEntry (pure types, header-only)

#include <opencv2/core.hpp>

#include <string>
#include <vector>

namespace denso::ui {

/// cv::putText scale bounds. The floor keeps the smallest grid frame legible;
/// the ceiling stops a caption competing with the video on a full-screen tile.
inline constexpr double kZoneOverlayMinScale = 0.35;
inline constexpr double kZoneOverlayMaxScale = 1.10;

/// One overlay row, e.g. `Z1   128  OK` or `Z3    --  ACQUIRING`.
/// A number appears ONLY where the projection carries one, so every state that
/// must not show a reading renders `--` by construction rather than by a rule
/// repeated here. Pure, so the exact rendered text is directly assertable.
std::string zone_row_text(const ZoneRuntimeEntry& z);

/// Text scale for a frame of this height. Scales with the image so the panel
/// stays readable on a 2x2 wall, clamped at both ends.
double zone_overlay_scale(int frame_h);

/// Draw the zone panel onto `frame` IN PLACE (BGR, CV_8UC3).
///
/// Call it AFTER the detection boxes and BEFORE the frame is converted for
/// display, so the values are part of the picture the tile shows. An empty
/// projection leaves the frame byte-identical; an empty Mat is a no-op.
///
/// `zones` must already be filtered to the camera this frame belongs to.
void draw_zone_runtime_overlay(cv::Mat& frame,
                               const std::vector<ZoneRuntimeEntry>& zones);

} // namespace denso::ui
