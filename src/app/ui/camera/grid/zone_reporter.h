// The machine's ZoneSink: every camera's DetectionProcessor feeds its assembled
// zones here (from capture threads), so it locks a mutex around a ZoneAggregator.
// When a zone's stable value changes, it invokes on_snapshot with the full
// {zone_no -> value} map — the wiring passes a callback that marshals to the GUI
// thread's BrazingReporter (post_to_gui), so capture threads never touch the
// network.
#pragma once

#include "ui/camera/grid/zone_sink.h"   // ZoneSink (no dep on frame_processor)
#include "ui/camera/grid/zone_aggregator.h"

#include <functional>
#include <map>
#include <mutex>

namespace denso::ui {

class ZoneReporter : public ZoneSink {
public:
    explicit ZoneReporter(std::function<void(const std::map<int, int>&)> on_snapshot,
                          int stable_frames = kStableFrames);

    void on_zones(int64_t camera_id, const std::vector<ZoneReading>& zones) override;

private:
    std::function<void(const std::map<int, int>&)> on_snapshot_;
    std::mutex mutex_;
    ZoneAggregator aggregator_;
};

} // namespace denso::ui
