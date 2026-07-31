// The Ball Leveler measurement BURNED INTO the displayed frame, with the same
// OpenCV primitives the detection boxes and the zone panel use. The operator
// reads the level off the camera image itself, so the annotation travels with
// the picture rather than sitting in a widget layered over it — and there is no
// second Qt result panel to disagree with the frame.
//
// Pure: OpenCV + the pure level core only. No Qt, no inference backend, no SQL.
// Lives in denso_camera beside zone_overlay for the same reason: it is
// frame-side, and it is consumed by level_processor.
//
// Geometry arrives NORMALIZED (fractions of the oriented frame) and is scaled to
// pixels here, so the same calibration draws correctly at any capture size.
#pragma once

#include "level/calibration.h"
#include "level/measure.h"   // BallBox
#include "level/runtime.h"   // LevelRuntimeEntry

#include <opencv2/core.hpp>

#include <optional>
#include <string>

namespace denso::ui {

/// cv::putText scale bounds, matching the zone overlay so both annotations read
/// as one system on a 2x2 wall.
inline constexpr double kLevelOverlayMinScale = 0.40;
inline constexpr double kLevelOverlayMaxScale = 1.20;

/// The value line: `LEVEL 67.4%` when the entry carries a measurement, `LEVEL --`
/// otherwise. A number appears ONLY where `percent` is engaged, and
/// LevelRuntimeEntry engages it only for Healthy — so every non-Healthy state
/// renders `--` BY CONSTRUCTION, not because this function knows their meaning.
std::string level_value_text(const level::LevelRuntimeEntry& e);

/// The state line: empty when Healthy (the number says it), else
/// `STATE ACQUIRING` / `STATE UNAVAILABLE` / `STATE CALIBRATION INVALID` /
/// `STATE UNCONFIGURED`. The Unavailable reason code is NOT rendered here: it is
/// a diagnostic for the log and status file, and the states are what an operator
/// standing at the machine can act on.
std::string level_state_text(const level::LevelRuntimeEntry& e);

/// Text scale for a frame of this height.
double level_overlay_scale(int frame_h);

/// Draw the level annotation onto `frame` IN PLACE (BGR, CV_8UC3).
///
/// Call it on the DISPLAY-ONLY Mat, immediately before the frame is converted
/// for display, so the measurement is part of the picture the tile shows. An
/// empty Mat is a no-op.
///
/// `calib` absent (an unconfigured / unreadable camera) draws the text only — no
/// rectangle and no reference lines, because there is no geometry to claim. That
/// is the honest picture: nothing is being measured, so nothing is outlined.
///
/// `ball` is the selected detection, in the same normalized coordinates. Absent
/// means nothing was selected this frame and no box or centre marker is drawn.
void draw_level_overlay(cv::Mat& frame,
                        const std::optional<level::LevelCalibration>& calib,
                        const level::LevelRuntimeEntry& entry,
                        const std::optional<level::BallBox>& ball);

}  // namespace denso::ui
