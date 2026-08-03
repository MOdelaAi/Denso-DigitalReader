// Machine-wide zone state with per-zone debounce. Each camera feeds its zones in
// via observe(); a value must repeat kStableFrames times before it's "stable".
// When any zone's stable value differs from what was last sent, observe() returns
// the full latest-value snapshot ({zone_no -> value}) to POST. A zone that stops
// being observed for longer than the expiry window is dropped from the snapshot
// (and evicted) so a one-time stale reading can't be re-sent forever. Pure (std
// only) — the ZoneReporter wraps it with a mutex and the network marshal.
#pragma once

#include "brazing/zone_reading.h"
#include "brazing/zone_runtime.h"

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
    /// `hold_timeout_ms` is the window a zone may keep republishing its last
    /// accepted value while its readings are non-Complete, before escalating to
    /// Inhibited. It is a PARAMETER, exactly as `stable_frames` already is,
    /// because the two modes need different values of the same policy — not two
    /// policies (amendment §10.4):
    ///
    ///   digit_reader: kHoldTimeoutMs (30 s) — an OCR gap is transient and the
    ///                 last read number is still the best available truth.
    ///   ball_leveler: 0 — a level measurement that stopped is not evidence of
    ///                 the current level, and the mode's invariant is that no old
    ///                 percentage is ever annotated or reported as live. Zero
    ///                 makes the hold window empty, so the first non-Complete
    ///                 reading escalates immediately and the value is evicted.
    ///
    /// A second aggregator for Ball would be a second debounce, expiry, hold and
    /// inhibit authority. This is one authority, configured.
    explicit ZoneAggregator(int stable_frames = kStableFrames,
                            int64_t expiry_ms = kZoneExpiryMs,
                            int64_t hold_timeout_ms = kHoldTimeoutMs);

    /// Feed one camera's assembled zones, stamped with a monotonic time in ms.
    /// Returns the full snapshot to send when any zone's stable value changed vs
    /// the last sent snapshot, OR when a previously sent zone expired (so the
    /// backend stops seeing a dead zone), else nullopt. Zones absent from a call
    /// are retained until their expiry window elapses (occlusion tolerance).
    std::optional<std::map<int, ZoneValue>> observe(const std::vector<ZoneReading>& zones,
                                              int64_t now_ms = 0);

    /// Drop these zones from both the debounce state and the last-sent payload,
    /// as an inhibit does. Returns the shrunk snapshot when the payload actually
    /// changed, else nullopt. NEVER returns an empty snapshot (spec §3.3).
    std::optional<std::map<int, ZoneValue>> evict_zones(const std::set<int>& zone_nos);

    /// Forget what the backend has already been told, and NOTHING else.
    ///
    /// `last_sent_` is the only reason an unchanged stable value is suppressed.
    /// A backend sender created (or replaced) while the appliance is already
    /// running inherits none of the delivery history the old sender had, so
    /// without this the newly configured server would never be told a zone's
    /// current value — it would wait, possibly forever, for the reading to
    /// CHANGE. Clearing the baseline makes the next observation that meets the
    /// existing stable-frame bar compare against an empty baseline and publish
    /// once; normal unchanged-value suppression resumes from that snapshot.
    ///
    /// Deliberately narrow: it touches no debounce counter, no hold/last-valid
    /// state, no inhibit set, and no runtime projection, so it can neither
    /// fabricate a value nor let one skip the stability requirement. It emits
    /// nothing — there is no snapshot to return, only a baseline to drop.
    void reset_sent_baseline();

    /// Zones that escalated from hold to inhibited since the last call. Draining
    /// is destructive so a caller raises each alarm exactly once.
    std::set<int> take_newly_inhibited();

    /// Read-only projection of every tracked zone, for the grid overlay. Returns
    /// COPIES: the caller holds no reference into aggregator state, so it stays
    /// valid once the wrapping lock is released. Exposes no debounce counters,
    /// no last-sent payload and no delivery state — a projection, not a second
    /// authority. Zone-keyed only; the camera join happens in ZoneReporter.
    std::vector<ZoneRuntime> runtime_view() const;

private:
    struct Debounce {
        ZoneValue candidate;
        int     count = 0;
        bool    has_stable = false;
        ZoneValue stable;
        int64_t last_seen_ms = 0;      // ANY frame, incl. incomplete — liveness
        // ── Hold state (spec §5.3) ──
        bool    has_last_valid = false;
        ZoneValue last_valid;
        int64_t last_complete_ms = 0;  // ONLY complete readings — hold timeout base
        bool    has_first_seen = false; // false until first_seen_ms is set (t=0 is a
                                         // valid timestamp, so it can't be its own sentinel)
        int64_t first_seen_ms = 0;     // cold-start timeout base (spec §5.3.1)
        bool    needs_reannounce = false;
        // Whether the MOST RECENT reading was Complete. Recorded explicitly
        // because it cannot be derived from the timestamps: after an incomplete
        // frame the next complete one refreshes last_complete_ms while still
        // sitting below the debounce bar, and a Complete/Incomplete pair inside
        // one millisecond leaves last_complete_ms == last_seen_ms. This is the
        // aggregator recording its own observation phase — not a second policy.
        bool    last_reading_complete = false;
    };

    // Build the full snapshot of every zone holding a stable value and commit it.
    // Returns nullopt when the result would be EMPTY: build_brazing_payload({})
    // renders literal "{}" and, under an unverified backend, could clear every zone.
    // `recovered_zone_nos` is the set of zones that completed their OWN recovery
    // (reached count >= stable_frames_) in the calling observe() invocation — only
    // those zones' needs_reannounce may be consumed by this commit. Passing an
    // empty set (as evict_zones() does) means no zone's re-announce is consumed,
    // since merely carrying a sibling's held value in the snapshot must not count.
    std::optional<std::map<int, ZoneValue>> build_snapshot(
        const std::set<int>& recovered_zone_nos = {});

    int stable_frames_;
    int64_t expiry_ms_;
    int64_t hold_timeout_ms_;
    std::map<int, Debounce> zones_;   // zone_no -> debounce state
    std::map<int, ZoneValue> last_sent_;    // zone_no -> value in the last snapshot
    std::set<int> zone_inhibit_;      // publication suppressed; debounce continues
    std::set<int> newly_inhibited_;   // drained by take_newly_inhibited()
};

} // namespace denso::ui
