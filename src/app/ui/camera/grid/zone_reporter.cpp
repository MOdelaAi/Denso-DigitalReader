#include "ui/camera/grid/zone_reporter.h"

#include <utility>

namespace denso::ui {

ZoneReporter::ZoneReporter(std::function<void(const std::map<int, int>&)> on_snapshot,
                           int stable_frames)
    : on_snapshot_(std::move(on_snapshot)), aggregator_(stable_frames) {}

void ZoneReporter::on_zones(int64_t /*camera_id*/,
                            const std::vector<ZoneReading>& zones) {
    std::optional<std::map<int, int>> snapshot;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot = aggregator_.observe(zones);
    }
    if (snapshot && on_snapshot_) {
        on_snapshot_(*snapshot);  // marshals to the GUI thread (set at wiring)
    }
}

} // namespace denso::ui
