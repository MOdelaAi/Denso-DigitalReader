// The machine's ZoneSink: every camera's DetectionProcessor feeds its assembled
// zones here (from capture threads), so it locks a mutex around a ZoneAggregator.
// When a zone's stable value changes, it invokes on_snapshot with the full
// {zone_no -> value} map plus a monotonic sequence number — the wiring passes a
// callback that marshals to the GUI thread's BrazingReporter (post_to_gui), so
// capture threads never touch the network.
//
// Pure logic (no Qt dependency): it lives in denso_brazing so denso_tests can
// link it directly. A CAMERA-level inhibit (set_camera_inhibited) drops the whole
// observation under the same mutex as observe() — that is what kills the
// resurrection race. Contrast the zone-level hold-timeout inhibit inside
// ZoneAggregator, which suppresses PUBLICATION only so a zone can still recover.
#pragma once

#include "brazing/zone_sink.h"   // ZoneSink (no dep on frame_processor)
#include "brazing/zone_aggregator.h"

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <set>

namespace denso::ui {

class ZoneReporter : public ZoneSink {
public:
    /// on_snapshot receives the full payload plus a monotonic sequence number.
    /// The sequence lets the GUI side drop a snapshot that overtook a newer one:
    /// callbacks fire OUTSIDE the mutex and marshal from several threads, so an
    /// eviction and a recovery can otherwise arrive reversed (spec §3.3d).
    explicit ZoneReporter(
        std::function<void(const std::map<int, int>&, uint64_t)> on_snapshot,
        int stable_frames = kStableFrames);

    void on_zones(int64_t camera_id, const std::vector<ZoneReading>& zones) override;

    /// Assert or clear a CAMERA-level inhibit. Asserting evicts the camera's
    /// recorded zones atomically with marking it inhibited. Call on the GUI thread.
    void set_camera_inhibited(int64_t camera_id, bool on);

private:
    std::function<void(const std::map<int, int>&, uint64_t)> on_snapshot_;
    std::mutex      mutex_;
    ZoneAggregator  aggregator_;
    std::set<int64_t>                inhibited_cameras_;
    std::map<int64_t, std::set<int>> camera_zones_;   // recorded from accepted obs
    uint64_t                         seq_ = 0;
};

} // namespace denso::ui
