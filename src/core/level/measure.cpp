#include "level/measure.h"

#include <cmath>

namespace denso::level {

std::optional<double> level_percent(const LevelCalibration& c, double centre_y) {
    // The calibration is re-validated on every call rather than trusted. A row can
    // be hand-edited or restored from a backup, and this is the last point before
    // a number reaches an operator's eyes; validating here means no caller can
    // forget to. `validate_calibration` already guarantees the span is finite,
    // correctly ordered and at least kMinSpanNorm, so the division below is safe.
    if (!validate_calibration(c).ok) return std::nullopt;
    if (!std::isfinite(centre_y)) return std::nullopt;

    const double span = c.y_0 - c.y_100;   // > 0, checked above
    const double pct = (c.y_0 - centre_y) / span * 100.0;

    // Clamp. Written as explicit comparisons rather than std::clamp so the two
    // saturating cases are individually visible and individually tested.
    if (pct < 0.0) return 0.0;
    if (pct > 100.0) return 100.0;
    return pct;
}

std::optional<BallChoice> select_ball(const std::vector<BallBox>& candidates,
                                      const LevelCalibration& c) {
    if (!validate_calibration(c).ok) return std::nullopt;

    const double left = c.rect_x;
    const double right = c.rect_x + c.rect_w;
    const double top = c.rect_y;
    const double bottom = c.rect_y + c.rect_h;

    std::optional<BallChoice> best;
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        const BallBox& b = candidates[i];

        // A non-finite box is discarded, not clamped: it means the detector or the
        // caller's normalisation is broken, and inventing a position from it would
        // manufacture a plausible-looking reading out of a fault.
        if (!std::isfinite(b.x1) || !std::isfinite(b.y1) || !std::isfinite(b.x2) ||
            !std::isfinite(b.y2) || !std::isfinite(b.conf)) {
            continue;
        }
        if (b.conf < c.conf) continue;   // below the operator's threshold

        const double cx = b.centre_x();
        const double cy = b.centre_y();
        if (cx < left || cx > right || cy < top || cy > bottom) continue;

        // Strictly greater keeps the EARLIEST of equally-confident candidates, so
        // the choice is deterministic for a given input order.
        if (!best || b.conf > best->box.conf) {
            best = BallChoice{b, i};
        }
    }
    return best;
}

}  // namespace denso::level
