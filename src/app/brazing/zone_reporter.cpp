#include "brazing/zone_reporter.h"

#include <chrono>
#include <utility>

namespace denso::ui {

ZoneReporter::ZoneReporter(
    std::function<void(const std::map<int, int>&, uint64_t)> on_snapshot,
    int stable_frames)
    : on_snapshot_(std::move(on_snapshot)), aggregator_(stable_frames) {}

void ZoneReporter::on_zones(int64_t camera_id, const std::vector<ZoneReading>& zones) {
    const int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now().time_since_epoch())
                               .count();
    std::optional<std::map<int, int>> snapshot;
    uint64_t seq = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // CAMERA-level causes drop the WHOLE observation. Checking this under the
        // same mutex as observe() is what kills the resurrection race: a worker
        // already blocked here cannot re-create a zone the inhibit just evicted.
        // Contrast the zone-level hold-timeout inhibit, which suppresses only
        // PUBLICATION so the zone can still recover (spec §3.1.1).
        if (inhibited_cameras_.count(camera_id) > 0) {
            return;
        }
        // Ownership is recorded from observations we ACCEPT, never derived from
        // current config: a renumbered ROI must still evict the zone we actually
        // published under (spec §3.3b).
        auto& owned = camera_zones_[camera_id];
        for (const ZoneReading& z : zones) {
            owned.insert(z.zone_no);
        }
        snapshot = aggregator_.observe(zones, now_ms);
        if (snapshot) {
            seq = ++seq_;
        }
    }
    if (snapshot && on_snapshot_) {
        on_snapshot_(*snapshot, seq);
    }
}

void ZoneReporter::set_camera_inhibited(int64_t camera_id, bool on) {
    std::optional<std::map<int, int>> snapshot;
    uint64_t seq = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (on) {
            // Mark and evict atomically — any gap between them is a window in which
            // a blocked observation could republish the zone.
            inhibited_cameras_.insert(camera_id);
            const auto it = camera_zones_.find(camera_id);
            if (it != camera_zones_.end()) {
                snapshot = aggregator_.evict_zones(it->second);
            }
        } else {
            // Release clears the flag only. The zone re-enters naturally on the next
            // observation, rebuilds a fresh Debounce (so it re-earns the debounce),
            // and reports even an unchanged value because last_sent_ no longer
            // holds it (spec §3.4).
            inhibited_cameras_.erase(camera_id);
        }
        // A suppressed (empty) snapshot must NOT consume a sequence number, or the
        // GUI's drop-stale rule would discard the next genuine snapshot (spec §3.3).
        if (snapshot) {
            seq = ++seq_;
        }
    }
    if (snapshot && on_snapshot_) {
        on_snapshot_(*snapshot, seq);
    }
}

} // namespace denso::ui
