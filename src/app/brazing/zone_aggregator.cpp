#include "brazing/zone_aggregator.h"

#include <algorithm>

namespace denso::ui {

ZoneAggregator::ZoneAggregator(int stable_frames, int64_t expiry_ms)
    : stable_frames_(std::max(1, stable_frames)), expiry_ms_(expiry_ms) {}

std::optional<std::map<int, int>> ZoneAggregator::observe(
    const std::vector<ZoneReading>& zones, int64_t now_ms) {
    bool changed = false;
    // Zones that complete THEIR OWN recovery (reach count >= stable_frames_) in
    // this call. Only these may have their needs_reannounce consumed by whatever
    // commit results — a sibling's snapshot must not swallow a zone's own
    // still-pending re-announce just because it appears in that snapshot at its
    // held value (see build_snapshot()).
    std::set<int> recovered_this_call;

    for (const ZoneReading& z : zones) {
        Debounce& d = zones_[z.zone_no];
        if (!d.has_first_seen) {
            d.first_seen_ms = now_ms;
            d.has_first_seen = true;
        }
        // Liveness is refreshed by ANY frame. An incomplete or no-digit frame still
        // proves the camera is alive; without this the 10s expiry would erase a held
        // zone long before the 30s hold timeout could run (spec §5.3).
        d.last_seen_ms = now_ms;

        if (z.kind != ReadingKind::Complete) {
            // Soft hold: break the stable run so frames either side of the gap cannot
            // combine into five "consecutive" observations. Leave stable/has_stable
            // and last_sent_ untouched so the held value keeps being reported, and
            // owe a re-announce so recovery reports even an UNCHANGED value.
            d.count = 0;
            if (d.has_last_valid) {
                d.needs_reannounce = true;
            }
            continue;   // NOT a fresh observation: last_complete_ms is not refreshed
        }

        d.last_complete_ms = now_ms;
        if (z.value == d.candidate) {
            ++d.count;
        } else {
            d.candidate = z.value;
            d.count = 1;
        }
        if (d.count >= stable_frames_) {
            recovered_this_call.insert(z.zone_no);
            const bool newly_stable = (!d.has_stable || d.stable != d.candidate);
            if (newly_stable) {
                d.has_stable = true;
                d.stable = d.candidate;
            }
            d.has_last_valid = true;
            d.last_valid = d.stable;
            const auto it = last_sent_.find(z.zone_no);
            if (it == last_sent_.end() || it->second != d.stable || d.needs_reannounce) {
                changed = true;
            }
        }
    }

    // Expire zones unseen past the window: drop their state so a one-time stale
    // reading (or a zone that never even reached stability) can't linger forever
    // and the maps stay bounded. This applies regardless of has_stable — a
    // never-stable entry's first_seen_ms/count must not survive to be misread by
    // a later timeout sweep. Only removing a zone that was actually part of the
    // last-sent payload counts as a payload change worth emitting promptly.
    for (auto it = zones_.begin(); it != zones_.end();) {
        const Debounce& d = it->second;
        if (now_ms - d.last_seen_ms > expiry_ms_) {
            if (last_sent_.erase(it->first) > 0) {
                changed = true;
            }
            it = zones_.erase(it);
        } else {
            ++it;
        }
    }

    if (!changed) {
        return std::nullopt;
    }

    return build_snapshot(recovered_this_call);
}

std::optional<std::map<int, int>> ZoneAggregator::evict_zones(
    const std::set<int>& zone_nos) {
    bool changed = false;
    for (const int zone_no : zone_nos) {
        zones_.erase(zone_no);
        if (last_sent_.erase(zone_no) > 0) {
            changed = true;
        }
    }
    if (!changed) {
        return std::nullopt;
    }
    return build_snapshot();
}

// Build the full snapshot of every zone holding a stable value and commit it.
// Returns nullopt when the result would be EMPTY: build_brazing_payload({})
// renders literal "{}" and, under an unverified backend, could clear every zone.
std::optional<std::map<int, int>> ZoneAggregator::build_snapshot(
    const std::set<int>& recovered_zone_nos) {
    std::map<int, int> snapshot;
    for (const auto& [zone_no, d] : zones_) {
        if (d.has_stable) {
            snapshot[zone_no] = d.stable;
        }
    }
    if (snapshot.empty()) {
        return std::nullopt;
    }
    last_sent_ = snapshot;
    // Consume re-announce ONLY for a zone that itself just completed recovery in
    // THIS call (recovered_zone_nos). A zone merely carried into this snapshot at
    // its already-held value — because a SIBLING zone's change triggered the
    // commit, or because evict_zones() committed a shrink — must keep owing its
    // forced report; otherwise that report is silently swallowed (spec §5.3).
    for (const int zone_no : recovered_zone_nos) {
        const auto it = zones_.find(zone_no);
        if (it != zones_.end()) {
            it->second.needs_reannounce = false;
        }
    }
    return snapshot;
}

} // namespace denso::ui
