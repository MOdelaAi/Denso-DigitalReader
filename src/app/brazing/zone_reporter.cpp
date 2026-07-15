#include "brazing/zone_reporter.h"

#include <chrono>
#include <utility>

namespace denso::ui {

ZoneReporter::ZoneReporter(std::function<void(const std::map<int, int>&)> on_snapshot,
                           int stable_frames)
    : on_snapshot_(std::move(on_snapshot)), aggregator_(stable_frames) {}

void ZoneReporter::on_zones(int64_t /*camera_id*/,
                            const std::vector<ZoneReading>& zones) {
    const int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now().time_since_epoch())
                               .count();
    std::optional<std::map<int, int>> snapshot;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot = aggregator_.observe(zones, now_ms);
    }
    if (snapshot && on_snapshot_) {
        on_snapshot_(*snapshot);  // marshals to the GUI thread (set at wiring)
    }
}

} // namespace denso::ui
