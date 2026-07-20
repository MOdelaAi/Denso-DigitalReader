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
constexpr int64_t kHoldTimeoutMs = 30000; // hold -> Inhibited escalation (spec §5.3)

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

    /// Zones that escalated from hold to inhibited since the last call. Draining
    /// is destructive so a caller raises each alarm exactly once.
    std::set<int> take_newly_inhibited();

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
        bool    has_first_seen = false; // false until first_seen_ms is set (t=0 is a
                                         // valid timestamp, so it can't be its own sentinel)
        int64_t first_seen_ms = 0;     // cold-start timeout base (spec §5.3.1)
        bool    needs_reannounce = false;
    };

    // Build the full snapshot of every zone holding a stable value and commit it.
    // Returns nullopt when the result would be EMPTY: build_brazing_payload({})
    // renders literal "{}" and, under an unverified backend, could clear every zone.
    // `recovered_zone_nos` is the set of zones that completed their OWN recovery
    // (reached count >= stable_frames_) in the calling observe() invocation — only
    // those zones' needs_reannounce may be consumed by this commit. Passing an
    // empty set (as evict_zones() does) means no zone's re-announce is consumed,
    // since merely carrying a sibling's held value in the snapshot must not count.
    std::optional<std::map<int, int>> build_snapshot(
        const std::set<int>& recovered_zone_nos = {});

    int stable_frames_;
    int64_t expiry_ms_;
    int64_t hold_timeout_ms_ = kHoldTimeoutMs;
    std::map<int, Debounce> zones_;   // zone_no -> debounce state
    std::map<int, int> last_sent_;    // zone_no -> value in the last snapshot
    std::set<int> zone_inhibit_;      // publication suppressed; debounce continues
    std::set<int> newly_inhibited_;   // drained by take_newly_inhibited()
};

} // namespace denso::ui
