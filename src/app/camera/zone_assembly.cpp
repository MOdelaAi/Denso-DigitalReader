#include "camera/zone_assembly.h"

#include "camera/area_geometry.h"  // point_in_polygon

#include <algorithm>
#include <limits>
#include <string>

namespace denso::ui {

std::optional<int> assemble_zone_value(const std::vector<NamedDetection>& digits_in_zone) {
    if (digits_in_zone.empty()) {
        return std::nullopt;
    }
    std::vector<const NamedDetection*> ordered;
    ordered.reserve(digits_in_zone.size());
    for (const NamedDetection& d : digits_in_zone) {
        ordered.push_back(&d);
    }
    // Left-to-right by box x-center.
    std::sort(ordered.begin(), ordered.end(),
              [](const NamedDetection* a, const NamedDetection* b) {
                  return (a->box.x + a->box.width * 0.5) < (b->box.x + b->box.width * 0.5);
              });

    std::string digits;
    for (const NamedDetection* d : ordered) {
        digits += d->name;  // class name is the digit label ("0".."9")
    }
    // Every char must be a decimal digit, else it's not a number we can send.
    if (!std::all_of(digits.begin(), digits.end(),
                     [](unsigned char c) { return c >= '0' && c <= '9'; })) {
        return std::nullopt;
    }
    // Zone values are restricted to 0..999. A spurious extra detection (>3
    // digits) is rejected rather than POSTed as a bogus 4-digit reading; ≤3
    // digits can never overflow int (max 999), so no try/catch is needed.
    if (digits.empty() || digits.size() > 3) {
        return std::nullopt;
    }
    return std::stoi(digits);  // leading zeros collapse; "050" -> 50
}

std::vector<ZoneReading> group_into_zones(const std::vector<NamedDetection>& kept,
                                          const std::vector<camera::CameraArea>& areas,
                                          float frame_w, float frame_h) {
    std::vector<ZoneReading> out;
    if (frame_w <= 0.0f || frame_h <= 0.0f) {
        return out;
    }
    for (const camera::CameraArea& area : areas) {
        if (!area.zone) {
            continue;  // ROI-only area — not reported
        }
        std::vector<NamedDetection> in_zone;
        float min_conf = std::numeric_limits<float>::max();
        for (const NamedDetection& d : kept) {
            const camera::Point c{
                (d.box.x + d.box.width * 0.5f) / frame_w,
                (d.box.y + d.box.height * 0.5f) / frame_h};
            if (camera::point_in_polygon(area.points, c)) {
                in_zone.push_back(d);
                min_conf = std::min(min_conf, d.conf);
            }
        }
        if (const auto v = assemble_zone_value(in_zone)) {
            out.push_back(ZoneReading{*area.zone, *v, min_conf});
        }
    }
    return out;
}

} // namespace denso::ui
