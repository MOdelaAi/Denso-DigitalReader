#include "ui/camera/grid/zone_aggregator.h"

#include <algorithm>

namespace denso::ui {

ZoneAggregator::ZoneAggregator(int stable_frames, int64_t expiry_ms)
    : stable_frames_(std::max(1, stable_frames)), expiry_ms_(expiry_ms) {}

std::optional<std::map<int, int>> ZoneAggregator::observe(
    const std::vector<ZoneReading>& zones, int64_t now_ms) {
    bool changed = false;

    for (const ZoneReading& z : zones) {
        Debounce& d = zones_[z.zone_no];
        d.last_seen_ms = now_ms;
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

    // Expire zones unseen past the window: drop them from the payload and evict
    // their state so a one-time stale reading can't be re-sent indefinitely and
    // the maps stay bounded. An expiry that removes a previously sent zone is a
    // change, so the shrunk snapshot is emitted promptly.
    for (auto it = zones_.begin(); it != zones_.end();) {
        const Debounce& d = it->second;
        if (d.has_stable && now_ms - d.last_seen_ms > expiry_ms_) {
            if (last_sent_.erase(it->first) > 0) {
                changed = true;
            }
            it = zones_.erase(it);
        } else {
            ++it;
        }
    }

    if (!changed) {
        return std::nullopt;
    }

    // Build the full snapshot of every zone that currently holds a stable value.
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
