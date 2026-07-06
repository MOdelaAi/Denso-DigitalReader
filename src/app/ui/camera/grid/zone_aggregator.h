// Machine-wide zone state with per-zone debounce. Each camera feeds its zones in
// via observe(); a value must repeat kStableFrames times before it's "stable".
// When any zone's stable value differs from what was last sent, observe() returns
// the full latest-value snapshot ({zone_no -> value}) to POST. Pure (std only) —
// the ZoneReporter wraps it with a mutex and the network marshal.
#pragma once

#include "ui/camera/grid/zone_reading.h"

#include <map>
#include <optional>
#include <vector>

namespace denso::ui {

constexpr int kStableFrames = 5;  // identical observations before a value is sent

class ZoneAggregator {
public:
    explicit ZoneAggregator(int stable_frames = kStableFrames);

    /// Feed one camera's assembled zones. Returns the full snapshot to send when
    /// any zone's stable value changed vs the last sent snapshot, else nullopt.
    /// Zones absent from `zones` are left untouched (occlusion tolerance).
    std::optional<std::map<int, int>> observe(const std::vector<ZoneReading>& zones);

private:
    struct Debounce {
        int candidate = 0;   // value currently accumulating
        int count = 0;       // consecutive observations of `candidate`
        bool has_stable = false;
        int stable = 0;      // last value that reached stability
    };

    int stable_frames_;
    std::map<int, Debounce> zones_;   // zone_no -> debounce state
    std::map<int, int> last_sent_;    // zone_no -> value in the last snapshot
};

} // namespace denso::ui
