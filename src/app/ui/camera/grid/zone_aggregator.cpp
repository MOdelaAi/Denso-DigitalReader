#include "ui/camera/grid/zone_aggregator.h"

#include <algorithm>

namespace denso::ui {

ZoneAggregator::ZoneAggregator(int stable_frames)
    : stable_frames_(std::max(1, stable_frames)) {}

std::optional<std::map<int, int>> ZoneAggregator::observe(
    const std::vector<ZoneReading>& zones) {
    bool changed = false;

    for (const ZoneReading& z : zones) {
        Debounce& d = zones_[z.zone_no];
        if (z.value == d.candidate) {
            ++d.count;
        } else {
            d.candidate = z.value;
            d.count = 1;
        }
        if (d.count >= stable_frames_) {
            if (!d.has_stable || d.stable != d.candidate) {
                d.has_stable = true;
                d.stable = d.candidate;
                // Did this differ from what we last sent for this zone?
                const auto it = last_sent_.find(z.zone_no);
                if (it == last_sent_.end() || it->second != d.stable) {
                    changed = true;
                }
            }
        }
    }

    if (!changed) {
        return std::nullopt;
    }

    // Build the full snapshot of every zone that has ever reached a stable value.
    std::map<int, int> snapshot;
    for (const auto& [zone_no, d] : zones_) {
        if (d.has_stable) {
            snapshot[zone_no] = d.stable;
        }
    }
    last_sent_ = snapshot;
    return snapshot;
}

} // namespace denso::ui
