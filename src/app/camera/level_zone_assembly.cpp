#include "camera/level_zone_assembly.h"

#include <algorithm>
#include <cmath>

namespace denso::ui {

std::vector<LevelZoneResult> evaluate_level_zones(
    const std::vector<denso::level::BallBox>& candidates,
    const std::vector<denso::level::LevelZone>& zones) {
    std::vector<LevelZoneResult> out;
    out.reserve(zones.size());
    for (const denso::level::LevelZone& z : zones) {
        LevelZoneResult r;
        r.zone_no = z.zone_no;
        // The SAME `candidates` vector every iteration — no per-zone inference,
        // no per-zone model, and no mutation of the shared set. select_ball is
        // const in its inputs, so one zone's choice cannot remove a detection
        // another zone would have chosen: two zones may legitimately select the
        // same ball if their rectangles overlap, and that is the operator's
        // geometry to fix, not something to silently arbitrate here.
        const auto choice = denso::level::select_ball(candidates, z.calibration);
        if (!choice) {
            out.push_back(std::move(r));   // present, but nothing selected
            continue;
        }
        const auto pct =
            denso::level::level_percent(z.calibration, choice->box.centre_y());
        if (!pct) {
            // select_ball already validated the calibration, so this is
            // unreachable for finite geometry — but level_percent is fail-closed
            // and there is no safe percentage to invent, so the zone reports
            // nothing rather than a fabricated number.
            out.push_back(std::move(r));
            continue;
        }
        // Assigned together, so the both-or-neither invariant holds by
        // construction rather than by every consumer re-checking it.
        r.percent = *pct;
        r.ball = choice->box;
        out.push_back(std::move(r));
    }
    return out;
}

int quantize_level_percent(double percent) {
    if (!std::isfinite(percent)) return 0;
    const double clamped = std::clamp(percent, 0.0, 100.0);
    // std::lround, not a truncating cast: 24.6 must report 25, not 24. Halfway
    // cases round away from zero, which on a [0,100] range means upward — a
    // single documented rule rather than the platform's rounding mode.
    return static_cast<int>(std::lround(clamped));
}

std::vector<ZoneReading> level_zone_readings(
    const std::vector<LevelZoneResult>& results) {
    std::vector<ZoneReading> out;
    out.reserve(results.size());
    for (const LevelZoneResult& r : results) {
        ZoneReading zr;
        zr.zone_no = r.zone_no;
        if (r.percent) {
            zr.kind = ReadingKind::Complete;
            zr.value = quantize_level_percent(*r.percent);
            // The selected ball's own confidence. The both-or-neither invariant
            // means `ball` is engaged wherever `percent` is, so this is not a
            // conditional read dressed up as an unconditional one.
            zr.conf = r.ball ? static_cast<float>(r.ball->conf) : 0.0f;
        } else {
            // NOT Incomplete: a percentage is whole or absent. Ball has no
            // partial reading, and claiming one would misreport the diagnostic.
            zr.kind = ReadingKind::NoValue;
            zr.value = 0;
            zr.conf = 0.0f;
        }
        out.push_back(zr);
    }
    return out;
}

} // namespace denso::ui
