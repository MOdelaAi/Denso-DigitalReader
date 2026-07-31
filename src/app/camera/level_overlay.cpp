#include "camera/level_overlay.h"

#include "camera/zone_overlay.h"   // shared panel placement constants

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

/// Colour by the SHARED zone state vocabulary — the same mapping the digit zone
/// panel uses, so a 2x2 wall running either mode reads identically.
cv::Scalar state_colour(ZoneDisplayState s) {
    switch (s) {
        case ZoneDisplayState::Healthy:          return kOk;
        case ZoneDisplayState::HoldingLastValid: return kOk;
        case ZoneDisplayState::Acquiring:        return kFaint;
        case ZoneDisplayState::Inhibited:        return kStopped;
        case ZoneDisplayState::Paused:           return kStopped;
        case ZoneDisplayState::Conflict:         return kStopped;
    }
    return kFaint;
}

/// The state word for a zone that is not showing a number. Stable text: it is
/// burned into the frame and read off a machine-side screen.
const char* state_word(ZoneDisplayState s) {
    switch (s) {
        case ZoneDisplayState::Healthy:          return "";
        case ZoneDisplayState::HoldingLastValid: return "HOLDING";
        case ZoneDisplayState::Acquiring:        return "ACQUIRING";
        case ZoneDisplayState::Inhibited:        return "UNAVAILABLE";
        case ZoneDisplayState::Paused:           return "PAUSED";
        case ZoneDisplayState::Conflict:         return "CONFLICT";
    }
    return "ACQUIRING";
}

// Normalized [0,1] -> pixel, clamped to the frame so a calibration drawn against
// a slightly different aspect can never index outside the Mat.
int px_x(double nx, const cv::Mat& f) {
    return std::clamp(static_cast<int>(std::lround(nx * f.cols)), 0, f.cols - 1);
}
int px_y(double ny, const cv::Mat& f) {
    return std::clamp(static_cast<int>(std::lround(ny * f.rows)), 0, f.rows - 1);
}

/// Shade a panel rect and return the clipped rect actually shaded, or an empty
/// rect when it falls entirely outside the frame.
cv::Rect shade_panel(cv::Mat& frame, cv::Rect panel) {
    panel &= cv::Rect(0, 0, frame.cols, frame.rows);
    if (panel.width <= 0 || panel.height <= 0) return cv::Rect();
    cv::Mat roi = frame(panel);
    cv::Mat shade(roi.size(), roi.type(), kPanel);
    cv::addWeighted(shade, kPanelOpacity, roi, 1.0 - kPanelOpacity, 0.0, roi);
    return panel;
}

}  // namespace

std::string level_zone_text(const LevelZoneDraw& z) {
    char buf[64];
    if (z.percent) {
        // One decimal on the frame, an integer to the backend. The two differ by
        // design (amendment §10.3): the operator sees the measurement at the
        // precision it was taken, the payload carries what its contract allows.
        std::snprintf(buf, sizeof(buf), "ZONE %d   LEVEL %.1f%%", z.zone_no,
                      *z.percent);
    } else {
        std::snprintf(buf, sizeof(buf), "ZONE %d   %s", z.zone_no,
                      state_word(z.state));
    }
    return std::string(buf);
}

double level_overlay_scale(int frame_h) {
    const double scaled = frame_h * 0.0014;
    return std::clamp(scaled, kLevelOverlayMinScale, kLevelOverlayMaxScale);
}

void draw_level_overlay(cv::Mat& frame, const std::vector<LevelZoneDraw>& zones) {
    if (frame.empty() || zones.empty()) {
        return;
    }

    const double scale = level_overlay_scale(frame.rows);
    // cv::putText anchors at the BASELINE, so a caption placed exactly on a
    // reference line would sit above it. Drop it by a scaled nudge.
    const int kCaptionDrop = std::max(3, static_cast<int>(std::lround(scale * 5.0)));
    const int thickness = std::max(1, static_cast<int>(std::lround(scale * 2.0)));
    const int pad = std::max(4, static_cast<int>(std::lround(scale * 12.0)));

    // ── Per-zone geometry ────────────────────────────────────────────────────
    // EVERY configured zone is outlined, including one that is measuring
    // nothing. A zone whose rectangle vanished when it stopped reading would
    // leave the operator unable to see where it was configured — exactly when
    // they need to.
    for (const LevelZoneDraw& z : zones) {
        const denso::level::LevelCalibration& c = z.calib;
        const int x0 = px_x(c.rect_x, frame);
        const int y0 = px_y(c.rect_y, frame);
        const int x1 = px_x(c.rect_x + c.rect_w, frame);
        const int y1 = px_y(c.rect_y + c.rect_h, frame);
        cv::rectangle(frame, cv::Point(x0, y0), cv::Point(x1, y1), kRect,
                      thickness, cv::LINE_AA);

        // The zone NUMBER on the rectangle itself. With up to four rectangles on
        // one frame the readout panel alone cannot say which is which, and the
        // number is the identity the backend payload and the operator both use.
        char tag[16];
        std::snprintf(tag, sizeof(tag), "Z%d", z.zone_no);
        cv::putText(frame, tag, cv::Point(x0 + pad / 2, y0 + kCaptionDrop + pad),
                    kFont, scale * 0.7, kRect, thickness, cv::LINE_AA);

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

        // ── The selected ball, per zone ──────────────────────────────────────
        if (z.ball) {
            const int bx0 = px_x(z.ball->x1, frame);
            const int by0 = px_y(z.ball->y1, frame);
            const int bx1 = px_x(z.ball->x2, frame);
            const int by1 = px_y(z.ball->y2, frame);
            cv::rectangle(frame, cv::Point(bx0, by0), cv::Point(bx1, by1), kBall,
                          thickness, cv::LINE_AA);
            // Centre marker — the actual measurement reference point, so an
            // operator can see WHAT was measured against the reference lines,
            // not just that something was detected.
            const int cx = px_x(z.ball->centre_x(), frame);
            const int cy = px_y(z.ball->centre_y(), frame);
            const int r = std::max(3, static_cast<int>(std::lround(scale * 6.0)));
            cv::drawMarker(frame, cv::Point(cx, cy), kBall, cv::MARKER_CROSS,
                           r * 2, thickness, cv::LINE_AA);
        }
    }

    // ── The readout panel: one row per zone, ascending ───────────────────────
    // Same shape, placement and lifecycle as the digit zone panel; only the row
    // text differs. Rows are built for every configured zone so a zone that
    // stops reading keeps its line and reports its state, rather than
    // disappearing and leaving three rows where the operator expects four.
    std::vector<std::string> rows;
    rows.reserve(zones.size());
    for (const LevelZoneDraw& z : zones) rows.push_back(level_zone_text(z));

    int text_w = 0;
    int line_h = 0;
    for (const std::string& r : rows) {
        int baseline = 0;
        const cv::Size s = cv::getTextSize(r, kFont, scale, thickness, &baseline);
        text_w = std::max(text_w, s.width);
        line_h = std::max(line_h, s.height + baseline);
    }
    const int step = line_h + std::max(2, pad / 2);
    const int panel_w = text_w + pad * 2;
    const int panel_h = static_cast<int>(rows.size()) * step + pad * 2 - (step - line_h);

    // Bottom-left, matching the zone panel: the tile paints the camera name top
    // left and the status dot top right over this image.
    const int margin = std::max(6, static_cast<int>(std::lround(frame.rows * 0.02)));
    const int lift = static_cast<int>(std::lround(frame.rows * 0.07));
    int x = margin;
    int y = frame.rows - lift - panel_h;
    if (y < margin) y = margin;
    if (x + panel_w > frame.cols) x = std::max(0, frame.cols - panel_w);

    const cv::Rect panel = shade_panel(frame, cv::Rect(x, y, panel_w, panel_h));
    if (panel.width <= 0) {
        return;
    }

    int baseline_y = panel.y + pad + line_h - std::max(2, pad / 2);
    for (size_t i = 0; i < rows.size(); ++i) {
        if (baseline_y > panel.y + panel.height) break;
        cv::putText(frame, rows[i], cv::Point(panel.x + pad, baseline_y), kFont,
                    scale, state_colour(zones[i].state), thickness, cv::LINE_AA);
        baseline_y += step;
    }
}

void draw_level_camera_state(cv::Mat& frame, const std::string& state_text) {
    if (frame.empty() || state_text.empty()) {
        return;
    }
    const double scale = level_overlay_scale(frame.rows);
    const int thickness = std::max(1, static_cast<int>(std::lround(scale * 2.0)));
    const int pad = std::max(4, static_cast<int>(std::lround(scale * 12.0)));

    int baseline = 0;
    const cv::Size ts =
        cv::getTextSize(state_text, kFont, scale, thickness, &baseline);
    const int line_h = ts.height + baseline;
    const int panel_w = ts.width + pad * 2;
    const int panel_h = line_h + pad * 2;

    const int margin = std::max(6, static_cast<int>(std::lround(frame.rows * 0.02)));
    const int lift = static_cast<int>(std::lround(frame.rows * 0.07));
    int x = margin;
    int y = frame.rows - lift - panel_h;
    if (y < margin) y = margin;
    if (x + panel_w > frame.cols) x = std::max(0, frame.cols - panel_w);

    const cv::Rect panel = shade_panel(frame, cv::Rect(x, y, panel_w, panel_h));
    if (panel.width <= 0) {
        return;
    }
    cv::putText(frame, state_text,
                cv::Point(panel.x + pad, panel.y + pad + line_h - std::max(2, pad / 2)),
                kFont, scale, kFaint, thickness, cv::LINE_AA);
}

}  // namespace denso::ui
