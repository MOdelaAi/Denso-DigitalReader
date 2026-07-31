// The Ball Leveler measurement BURNED INTO the displayed frame, with the same
// OpenCV primitives the detection boxes and the digit zone panel use. The
// operator reads the level off the camera image itself, so the annotation
// travels with the picture rather than sitting in a widget layered over it —
// and there is no second Qt result panel to disagree with the frame.
//
// This is the SAME composition boundary zone_overlay uses (draw into the
// display-only cv::Mat immediately before mat_to_qimage), and it renders through
// the SAME state vocabulary (ui::ZoneDisplayState). What differs is only the
// Ball geometry — the measurement rectangle, the two reference lines, the
// selected ball box and its centre marker — and the percentage label. That is
// the intended difference between the modes; everything else about placement,
// scale, lifecycle and frame-clearing is shared.
//
// Pure: OpenCV + the pure level core + the pure zone runtime types. No Qt, no
// inference backend, no SQL. Lives in denso_camera beside zone_overlay for the
// same reason: it is frame-side, and it is consumed by level_processor.
//
// Geometry arrives NORMALIZED (fractions of the oriented frame) and is scaled to
// pixels here, so the same calibration draws correctly at any capture size.
#pragma once

#include "brazing/zone_runtime.h"   // ZoneDisplayState — the SHARED vocabulary
#include "level/calibration.h"
#include "level/measure.h"   // BallBox

#include <opencv2/core.hpp>

#include <optional>
#include <string>
#include <vector>

namespace denso::ui {

/// cv::putText scale bounds, matching the zone overlay so both annotations read
/// as one system on a 2x2 wall.
inline constexpr double kLevelOverlayMinScale = 0.40;
inline constexpr double kLevelOverlayMaxScale = 1.20;

/// One Ball zone as the frame should show it.
///
/// INVARIANT, established by the producer and relied on here: `percent` and
/// `ball` are engaged ONLY when `state == Healthy`. That is what makes "no state
/// but Healthy can display a number" true by construction rather than by a rule
/// each draw site must remember — the same guarantee LevelRuntimeEntry's
/// constructors used to provide for the single-zone design.
struct LevelZoneDraw {
    int zone_no = 0;
    denso::level::LevelCalibration calib;
    ZoneDisplayState state = ZoneDisplayState::Acquiring;
    std::optional<double> percent;               ///< full precision, one decimal shown
    std::optional<denso::level::BallBox> ball;   ///< the selected detection
};

/// One zone's caption, e.g. `ZONE 1   LEVEL 24.5%` or `ZONE 3   ACQUIRING`.
///
/// A number appears ONLY where `percent` is engaged, so every non-Healthy state
/// renders its state word instead — by construction, not because this function
/// knows what each state means. One decimal place: the overlay shows the
/// measurement at the precision it was taken, while the backend receives the
/// quantized integer (amendment §10.3). The two differing is deliberate and
/// documented, not a rounding bug.
std::string level_zone_text(const LevelZoneDraw& z);

/// Text scale for a frame of this height.
double level_overlay_scale(int frame_h);

/// Draw the Ball annotation for EVERY configured zone onto `frame` IN PLACE
/// (BGR, CV_8UC3).
///
/// Call it on the DISPLAY-ONLY Mat, immediately before the frame is converted
/// for display, so the measurement is part of the picture the tile shows. An
/// empty Mat is a no-op; an empty `zones` leaves the frame byte-identical.
///
/// Every zone in `zones` is drawn, including zones that are not measuring —
/// their rectangle and reference lines still appear, because the operator needs
/// to see WHERE a zone that is reporting nothing was configured. Only the ball
/// box and centre marker are conditional on a selection.
///
/// `zones` must already be filtered to the camera this frame belongs to.
void draw_level_overlay(cv::Mat& frame, const std::vector<LevelZoneDraw>& zones);

/// The state-only annotation for a camera that is not measuring at all
/// (unconfigured, calibration invalid, configuration unreadable). Draws the
/// caption and NOTHING else — no rectangle and no reference lines, because there
/// is no geometry to claim. That is the honest picture: nothing is being
/// measured, so nothing is outlined.
void draw_level_camera_state(cv::Mat& frame, const std::string& state_text);

}  // namespace denso::ui
