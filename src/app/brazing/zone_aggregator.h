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
#include <set>
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

    /// Drop these zones from both the debounce state and the last-sent payload,
    /// as an inhibit does. Returns the shrunk snapshot when the payload actually
    /// changed, else nullopt. NEVER returns an empty snapshot (spec §3.3).
    std::optional<std::map<int, int>> evict_zones(const std::set<int>& zone_nos);

private:
    struct Debounce {
        int     candidate = 0;
        int     count = 0;
        bool    has_stable = false;
        int     stable = 0;
        int64_t last_seen_ms = 0;      // ANY frame, incl. incomplete — liveness
        // ── Hold state (spec §5.3) ──
        bool    has_last_valid = false;
        int     last_valid = 0;
        int64_t last_complete_ms = 0;  // ONLY complete readings — hold timeout base
        int64_t first_seen_ms = 0;     // cold-start timeout base (spec §5.3.1)
        bool    needs_reannounce = false;
    };

    // Build the full snapshot of every zone holding a stable value and commit it.
    // Returns nullopt when the result would be EMPTY: build_brazing_payload({})
    // renders literal "{}" and, under an unverified backend, could clear every zone.
    std::optional<std::map<int, int>> build_snapshot();

    int stable_frames_;
    int64_t expiry_ms_;
    std::map<int, Debounce> zones_;   // zone_no -> debounce state
    std::map<int, int> last_sent_;    // zone_no -> value in the last snapshot
};

} // namespace denso::ui
