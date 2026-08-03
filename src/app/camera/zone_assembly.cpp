#include "camera/zone_assembly.h"

#include "camera/area_geometry.h"  // point_in_polygon

#include <algorithm>
#include <limits>
#include <string>

namespace denso::ui {

ZoneAssembly assemble_zone_value(const std::vector<NamedDetection>& digits_in_zone) {
    if (digits_in_zone.empty()) {
        return {ReadingKind::NoValue, 0};
    }
    std::vector<const NamedDetection*> ordered;
    ordered.reserve(digits_in_zone.size());
    for (const NamedDetection& d : digits_in_zone) {
        ordered.push_back(&d);
    }
    std::sort(ordered.begin(), ordered.end(),
              [](const NamedDetection* a, const NamedDetection* b) {
                  return (a->box.x + a->box.width * 0.5) < (b->box.x + b->box.width * 0.5);
              });

    std::string digits;
    for (const NamedDetection* d : ordered) {
        digits += d->name;
    }
    // Anything unparseable is Incomplete, never a value. A group wider than the
    // four-position face means a spurious extra detection; a non-digit label
    // means the model emitted a class we cannot place. Both must hold the
    // previous value rather than POST.
    if (digits.size() > static_cast<std::size_t>(kDigitPositions) ||
        !std::all_of(digits.begin(), digits.end(),
                     [](unsigned char c) { return c >= '0' && c <= '9'; })) {
        return {ReadingKind::Incomplete, 0};
    }

    // Gap guard: estimate pitch from median box height, then look for an adjacent
    // pair sitting materially more than one pitch apart. One missing interior
    // digit yields ~2.0 pitch, comfortably past the 1.60 threshold; the margin is
    // deliberate, because a FALSE gap freezes a healthy zone, which is worse than
    // the missing-digit bug this mitigates.
    if (ordered.size() >= 2) {
        std::vector<float> heights;
        heights.reserve(ordered.size());
        for (const NamedDetection* d : ordered) {
            heights.push_back(static_cast<float>(d->box.height));
        }
        std::sort(heights.begin(), heights.end());
        const float median_h = heights[heights.size() / 2];
        // A non-positive median height (degenerate zero/negative-height boxes from
        // the detector) would collapse max_gap to <= 0, flagging virtually ANY
        // positive centre separation as a gap. Since Incomplete zones eventually
        // escalate to a permanently inhibited zone, a false gap here would freeze
        // an otherwise-healthy zone forever. The project's stated bias is to
        // UNDER-detect gaps (a missed gap merely holds the previous behaviour,
        // which is far cheaper than a false one), so skip the gap check entirely
        // rather than let it reject on bad geometry.
        if (median_h <= 0.0f) {
            return {ReadingKind::Complete, std::stoi(digits)};
        }
        const float max_gap = kGapFactor * kPitchPerHeight * median_h;
        for (std::size_t i = 1; i < ordered.size(); ++i) {
            const float prev_c = ordered[i - 1]->box.x + ordered[i - 1]->box.width * 0.5f;
            const float cur_c  = ordered[i]->box.x + ordered[i]->box.width * 0.5f;
            if (cur_c - prev_c > max_gap) {
                return {ReadingKind::Incomplete, 0};
            }
        }
    }
    return {ReadingKind::Complete, std::stoi(digits)};
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
        const ZoneAssembly a = assemble_zone_value(in_zone);
        // The zone's decimal format is applied HERE, after detection, ordering,
        // validation and grouping have already accepted (or rejected) the
        // reading. It moves the point; it can never turn a rejected reading
        // into a value, because `a.kind` is carried through untouched.
        // Emit for EVERY zoned area, including NoValue: the aggregator needs the
        // liveness signal, or the 10s expiry erases a held zone (spec §5.3).
        ZoneReading r;
        r.zone_no = *area.zone;
        r.kind    = a.kind;
        r.value   = ZoneValue{a.value, area.decimal_places, kDigitPositions};
        r.conf    = in_zone.empty() ? 0.0f : min_conf;
        out.push_back(r);
    }
    return out;
}

} // namespace denso::ui
