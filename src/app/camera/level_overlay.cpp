#include "camera/level_overlay.h"

#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace denso::ui {

namespace {

// BGR, deliberately the same palette family as zone_overlay: green = a live
// reading, grey = nothing to report yet, red = stopped or wrong.
const cv::Scalar kOk(172, 239, 134);
const cv::Scalar kFaint(148, 148, 148);
const cv::Scalar kStopped(113, 113, 248);
const cv::Scalar kRect(255, 200, 90);     // measurement rectangle
const cv::Scalar kLine100(172, 239, 134); // 100% reference line
const cv::Scalar kLine0(113, 113, 248);   // 0% reference line
const cv::Scalar kBall(0, 215, 255);      // selected ball box (detection amber)
const cv::Scalar kPanel(0, 0, 0);

constexpr int kFont = cv::FONT_HERSHEY_SIMPLEX;
constexpr double kPanelOpacity = 0.55;

cv::Scalar state_colour(level::LevelState s) {
    switch (s) {
        case level::LevelState::Healthy:            return kOk;
        case level::LevelState::Acquiring:          return kFaint;
        case level::LevelState::Unconfigured:       return kFaint;
        case level::LevelState::Unavailable:        return kStopped;
        case level::LevelState::CalibrationInvalid: return kStopped;
    }
    return kFaint;
}

// Normalized [0,1] -> pixel, clamped to the frame so a calibration drawn against
// a slightly different aspect can never index outside the Mat.
int px_x(double nx, const cv::Mat& f) {
    return std::clamp(static_cast<int>(std::lround(nx * f.cols)), 0, f.cols - 1);
}
int px_y(double ny, const cv::Mat& f) {
    return std::clamp(static_cast<int>(std::lround(ny * f.rows)), 0, f.rows - 1);
}

}  // namespace

std::string level_value_text(const level::LevelRuntimeEntry& e) {
    char buf[32];
    if (e.percent) {
        std::snprintf(buf, sizeof(buf), "LEVEL %.1f%%", *e.percent);
    } else {
        std::snprintf(buf, sizeof(buf), "LEVEL --");
    }
    return std::string(buf);
}

std::string level_state_text(const level::LevelRuntimeEntry& e) {
    if (e.state == level::LevelState::Healthy) {
        return std::string();   // the number is the message
    }
    return std::string("STATE ") + level::level_state_label(e.state);
}

double level_overlay_scale(int frame_h) {
    const double scaled = frame_h * 0.0014;
    return std::clamp(scaled, kLevelOverlayMinScale, kLevelOverlayMaxScale);
}

void draw_level_overlay(cv::Mat& frame,
                        const std::optional<level::LevelCalibration>& calib,
                        const level::LevelRuntimeEntry& entry,
                        const std::optional<level::BallBox>& ball) {
    if (frame.empty()) {
        return;
    }

    const double scale = level_overlay_scale(frame.rows);
    // cv::putText anchors at the BASELINE, so a caption placed exactly on a
    // reference line would sit above it. Drop it by a scaled nudge.
    const int kCaptionDrop = std::max(3, static_cast<int>(std::lround(scale * 5.0)));
    const int thickness = std::max(1, static_cast<int>(std::lround(scale * 2.0)));
    const int pad = std::max(4, static_cast<int>(std::lround(scale * 12.0)));

    // ── Geometry, only when there IS a calibration to draw ───────────────────
    if (calib) {
        const level::LevelCalibration& c = *calib;
        const int x0 = px_x(c.rect_x, frame);
        const int y0 = px_y(c.rect_y, frame);
        const int x1 = px_x(c.rect_x + c.rect_w, frame);
        const int y1 = px_y(c.rect_y + c.rect_h, frame);
        cv::rectangle(frame, cv::Point(x0, y0), cv::Point(x1, y1), kRect,
                      thickness, cv::LINE_AA);

        // Reference lines span the rectangle only — they are calibrated inside
        // it, and drawing them full-width would suggest they mean something
        // outside the measured region.
        const int y100 = px_y(c.y_100, frame);
        const int yzero = px_y(c.y_0, frame);
        cv::line(frame, cv::Point(x0, y100), cv::Point(x1, y100), kLine100,
                 thickness, cv::LINE_AA);
        cv::line(frame, cv::Point(x0, yzero), cv::Point(x1, yzero), kLine0,
                 thickness, cv::LINE_AA);
        cv::putText(frame, "100%", cv::Point(x1 + pad / 2, y100 + kCaptionDrop),
                    kFont, scale * 0.6, kLine100, std::max(1, thickness - 1),
                    cv::LINE_AA);
        cv::putText(frame, "0%", cv::Point(x1 + pad / 2, yzero + kCaptionDrop),
                    kFont, scale * 0.6, kLine0, std::max(1, thickness - 1),
                    cv::LINE_AA);
    }

    // ── The selected ball ────────────────────────────────────────────────────
    if (ball) {
        const int bx0 = px_x(ball->x1, frame);
        const int by0 = px_y(ball->y1, frame);
        const int bx1 = px_x(ball->x2, frame);
        const int by1 = px_y(ball->y2, frame);
        cv::rectangle(frame, cv::Point(bx0, by0), cv::Point(bx1, by1), kBall,
                      thickness, cv::LINE_AA);
        // Centre marker — the actual measurement reference point, so an operator
        // can see WHAT was measured against the reference lines, not just that
        // something was detected.
        const int cx = px_x(ball->centre_x(), frame);
        const int cy = px_y(ball->centre_y(), frame);
        const int r = std::max(3, static_cast<int>(std::lround(scale * 6.0)));
        cv::drawMarker(frame, cv::Point(cx, cy), kBall, cv::MARKER_CROSS, r * 2,
                       thickness, cv::LINE_AA);
    }

    // ── The readout panel ────────────────────────────────────────────────────
    const std::string value = level_value_text(entry);
    const std::string state = level_state_text(entry);

    int text_w = 0;
    int line_h = 0;
    int baseline = 0;
    const cv::Size vs = cv::getTextSize(value, kFont, scale, thickness, &baseline);
    text_w = vs.width;
    line_h = vs.height + baseline;
    if (!state.empty()) {
        int b2 = 0;
        const cv::Size ss = cv::getTextSize(state, kFont, scale, thickness, &b2);
        text_w = std::max(text_w, ss.width);
        line_h = std::max(line_h, ss.height + b2);
    }
    const int rows = state.empty() ? 1 : 2;
    const int step = line_h + std::max(2, pad / 2);
    const int panel_w = text_w + pad * 2;
    const int panel_h = rows * step + pad * 2 - (step - line_h);

    // Bottom-left, matching the zone panel: the tile paints the camera name top
    // left and the status dot top right over this image.
    const int margin = std::max(6, static_cast<int>(std::lround(frame.rows * 0.02)));
    const int lift = static_cast<int>(std::lround(frame.rows * 0.07));
    int x = margin;
    int y = frame.rows - lift - panel_h;
    if (y < margin) y = margin;
    if (x + panel_w > frame.cols) x = std::max(0, frame.cols - panel_w);

    cv::Rect panel(x, y, panel_w, panel_h);
    panel &= cv::Rect(0, 0, frame.cols, frame.rows);
    if (panel.width <= 0 || panel.height <= 0) {
        return;
    }
    cv::Mat roi = frame(panel);
    cv::Mat shade(roi.size(), roi.type(), kPanel);
    cv::addWeighted(shade, kPanelOpacity, roi, 1.0 - kPanelOpacity, 0.0, roi);

    const cv::Scalar colour = state_colour(entry.state);
    int baseline_y = panel.y + pad + line_h - std::max(2, pad / 2);
    cv::putText(frame, value, cv::Point(panel.x + pad, baseline_y), kFont, scale,
                colour, thickness, cv::LINE_AA);
    if (!state.empty()) {
        baseline_y += step;
        if (baseline_y <= panel.y + panel.height) {
            cv::putText(frame, state, cv::Point(panel.x + pad, baseline_y), kFont,
                        scale, colour, thickness, cv::LINE_AA);
        }
    }
}

}  // namespace denso::ui
