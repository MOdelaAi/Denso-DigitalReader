#include "camera/zone_overlay.h"

#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cstdio>

namespace denso::ui {

namespace {

// BGR, matching the palette the tile used before the annotation moved into the
// frame: green = accepted, amber = holding, red = stopped, grey = not yet known.
const cv::Scalar kOk(172, 239, 134);
const cv::Scalar kHold(21, 204, 250);
const cv::Scalar kStopped(113, 113, 248);
const cv::Scalar kFaint(148, 148, 148);
const cv::Scalar kPanel(0, 0, 0);

constexpr int kFont = cv::FONT_HERSHEY_SIMPLEX;
constexpr double kPanelOpacity = 0.55;

const char* state_label(ZoneDisplayState s) {
    switch (s) {
        case ZoneDisplayState::Healthy:          return "OK";
        case ZoneDisplayState::HoldingLastValid: return "HOLD";
        case ZoneDisplayState::Acquiring:        return "ACQUIRING";
        case ZoneDisplayState::Inhibited:        return "INHIBITED";
        case ZoneDisplayState::Paused:           return "PAUSED";
        case ZoneDisplayState::Conflict:         return "CONFLICT";
    }
    return "ACQUIRING";
}

cv::Scalar state_colour(ZoneDisplayState s) {
    switch (s) {
        case ZoneDisplayState::Healthy:          return kOk;
        // A hold is NOT an inhibit: it keeps its own colour so an operator can
        // tell "still reading, value retained" from "stopped".
        case ZoneDisplayState::HoldingLastValid: return kHold;
        case ZoneDisplayState::Acquiring:        return kFaint;
        case ZoneDisplayState::Inhibited:
        case ZoneDisplayState::Paused:
        case ZoneDisplayState::Conflict:         return kStopped;
    }
    return kFaint;
}

} // namespace

std::string zone_row_text(const ZoneRuntimeEntry& z) {
    // A number ONLY where the projection carries one — Acquiring, Inhibited,
    // Paused and Conflict all render "--" because `value` is nullopt for every
    // one of them, not because this function knows their meaning.
    char num[16];
    if (z.value) {
        std::snprintf(num, sizeof(num), "%d", *z.value);
    } else {
        std::snprintf(num, sizeof(num), "--");
    }
    char row[64];
    // Fixed columns so the digits line up down the panel.
    std::snprintf(row, sizeof(row), "Z%-2d %5s  %s", z.zone_no, num,
                  state_label(z.state));
    return std::string(row);
}

double zone_overlay_scale(int frame_h) {
    // ~0.5 at 480p, ~0.8 at 720p. Read off a video wall at machine-side
    // distance, so it tracks the image rather than staying a fixed size.
    const double scaled = frame_h * 0.0011;
    return std::clamp(scaled, kZoneOverlayMinScale, kZoneOverlayMaxScale);
}

void draw_zone_runtime_overlay(cv::Mat& frame,
                               const std::vector<ZoneRuntimeEntry>& zones) {
    if (frame.empty() || zones.empty()) {
        return;   // nothing to say — the frame is left byte-identical
    }

    const double scale = zone_overlay_scale(frame.rows);
    const int thickness = std::max(1, static_cast<int>(std::lround(scale * 2.0)));
    const int pad = std::max(4, static_cast<int>(std::lround(scale * 12.0)));

    // Measure first, so the panel is sized from the actual text and a long state
    // word can never be clipped.
    int text_w = 0;
    int line_h = 0;
    for (const ZoneRuntimeEntry& z : zones) {
        int baseline = 0;
        const cv::Size s =
            cv::getTextSize(zone_row_text(z), kFont, scale, thickness, &baseline);
        text_w = std::max(text_w, s.width);
        line_h = std::max(line_h, s.height + baseline);
    }
    const int step = line_h + std::max(2, pad / 2);
    const int panel_w = text_w + pad * 2;
    const int panel_h = static_cast<int>(zones.size()) * step + pad * 2 - (step - line_h);

    // Bottom-left. NOT top-left: the tile paints the camera name there and the
    // status dot top-right, and those are drawn over this image — an annotation
    // in either top corner would be sat on. The bottom strip can carry the
    // inhibit banner, so lift clear of it.
    const int margin = std::max(6, static_cast<int>(std::lround(frame.rows * 0.02)));
    const int lift = static_cast<int>(std::lround(frame.rows * 0.07));
    int x = margin;
    int y = frame.rows - lift - panel_h;
    if (y < margin) y = margin;      // tiny frame: sit at the top rather than off it
    if (x + panel_w > frame.cols) x = std::max(0, frame.cols - panel_w);

    cv::Rect panel(x, y, panel_w, panel_h);
    panel &= cv::Rect(0, 0, frame.cols, frame.rows);   // never leave the frame
    if (panel.width <= 0 || panel.height <= 0) {
        return;
    }

    // Translucent backing. Blended over the panel ROI only — cloning the whole
    // frame per frame would be pure waste at 15 fps across four cameras, and the
    // result is identical.
    cv::Mat roi = frame(panel);
    cv::Mat shade(roi.size(), roi.type(), kPanel);
    cv::addWeighted(shade, kPanelOpacity, roi, 1.0 - kPanelOpacity, 0.0, roi);

    int baseline_y = panel.y + pad + line_h - std::max(2, pad / 2);
    for (const ZoneRuntimeEntry& z : zones) {
        if (baseline_y > panel.y + panel.height) {
            break;   // clipped by a frame too short for every row
        }
        cv::putText(frame, zone_row_text(z), cv::Point(panel.x + pad, baseline_y),
                    kFont, scale, state_colour(z.state), thickness, cv::LINE_AA);
        baseline_y += step;
    }
}

} // namespace denso::ui
