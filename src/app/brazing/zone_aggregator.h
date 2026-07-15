// Machine-wide zone state with per-zone debounce. Each camera feeds its zones in
// via observe(); a value must repeat kStableFrames times before it's "stable".
// When any zone's stable value differs from what was last sent, observe() returns
// the full latest-value snapshot ({zone_no -> value}) to POST. A zone that stops
// being observed for longer than the expiry window is dropped from the snapshot
// (and evicted) so a one-time stale reading can't be re-sent forever. Pure (std
// only) — the ZoneReporter wraps it with a mutex and the network marshal.
#pragma once

#include "brazing/zone_reading.h"

#include <cstdint>
#include <map>
#include <optional>
#include <vector>

namespace denso::ui {

constexpr int kStableFrames = 5;          // identical observations before a value is sent
constexpr int64_t kZoneExpiryMs = 10000;  // drop a zone unseen for longer than this

class ZoneAggregator {
public:
    explicit ZoneAggregator(int stable_frames = kStableFrames,
                            int64_t expiry_ms = kZoneExpiryMs);

    /// Feed one camera's assembled zones, stamped with a monotonic time in ms.
    /// Returns the full snapshot to send when any zone's stable value changed vs
    /// the last sent snapshot, OR when a previously sent zone expired (so the
    /// backend stops seeing a dead zone), else nullopt. Zones absent from a call
    /// are retained until their expiry window elapses (occlusion tolerance).
    std::optional<std::map<int, int>> observe(const std::vector<ZoneReading>& zones,
                                              int64_t now_ms = 0);

private:
    struct Debounce {
        int candidate = 0;         // value currently accumulating
        int count = 0;             // consecutive observations of `candidate`
        bool has_stable = false;
        int stable = 0;            // last value that reached stability
        int64_t last_seen_ms = 0;  // time this zone was last observed
    };

    int stable_frames_;
    int64_t expiry_ms_;
    std::map<int, Debounce> zones_;   // zone_no -> debounce state
    std::map<int, int> last_sent_;    // zone_no -> value in the last snapshot
};

} // namespace denso::ui
