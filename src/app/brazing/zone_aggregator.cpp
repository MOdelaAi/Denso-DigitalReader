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
            // Quarantined recovery: a hold-timeout inhibit suppresses PUBLICATION
            // only, so the zone kept accumulating debounce and can clear itself
            // here. Gating the observation instead would deadlock it forever.
            const bool was_inhibited = zone_inhibit_.erase(z.zone_no) > 0;
            // The escalation this zone owed, if any, has now been undone by its
            // own recovery — a caller that hasn't drained take_newly_inhibited()
            // yet must not later be told about an inhibit that no longer holds.
            newly_inhibited_.erase(z.zone_no);
            const auto it = last_sent_.find(z.zone_no);
            if (it == last_sent_.end() || it->second != d.stable ||
                d.needs_reannounce || was_inhibited) {
                changed = true;
            }
        }
    }

    // "Recovered this call" must reflect the zone's FINAL state at the end of
    // the loop above, not a transition merely observed mid-loop: a duplicate
    // reading for the same zone (e.g. Complete then Incomplete in one call) can
    // make count cross stable_frames_ and then immediately fall back below it.
    // Prune any zone whose current count no longer actually meets the bar so
    // build_snapshot() cannot wrongly consume its needs_reannounce debt.
    for (auto it = recovered_this_call.begin(); it != recovered_this_call.end();) {
        const auto zit = zones_.find(*it);
        if (zit == zones_.end() || zit->second.count < stable_frames_) {
            it = recovered_this_call.erase(it);
        } else {
            ++it;
        }
    }

    // Expiry sweep runs BEFORE the timeout sweep: a zone that has genuinely aged
    // out (unseen — by ANY frame, complete or not — past expiry_ms_) must be
    // expired outright, never escalated to inhibited first. A zone truly stuck
    // in hold keeps being observed (incomplete frames refresh last_seen_ms), so
    // it will not expire here — only a zone that stopped arriving entirely does.
    // Expiry drops the zone from EVERY container: state for a zone that no
    // longer exists must not survive anywhere (zone_inhibit_/newly_inhibited_
    // included), or a later re-observation of that zone number would inherit an
    // orphaned inhibit instead of behaving as a fresh zone.
    for (auto it = zones_.begin(); it != zones_.end();) {
        const int zone_no = it->first;
        const Debounce& d = it->second;
        if (now_ms - d.last_seen_ms > expiry_ms_) {
            if (last_sent_.erase(zone_no) > 0) {
                changed = true;
            }
            zone_inhibit_.erase(zone_no);
            newly_inhibited_.erase(zone_no);
            it = zones_.erase(it);
        } else {
            ++it;
        }
    }

    // Hold-timeout sweep. One rule covers warm and cold start: measure against the
    // last COMPLETE reading, or — for a zone that has never read successfully —
    // against its first observation of any kind (spec §5.3.1). Baseline validity
    // comes from the explicit has_last_valid/has_first_seen flags, never from a
    // magic-zero timestamp check — t == 0 is a fully usable timestamp (a cold
    // zone first seen at t == 0, or a warm zone last complete at t == 0, must
    // still be able to time out). Every entry still in zones_ at this point has
    // has_first_seen == true (set unconditionally on first observation above),
    // so `base` is always a valid baseline.
    for (auto& [zone_no, d] : zones_) {
        if (zone_inhibit_.count(zone_no) > 0) {
            continue;   // already inhibited; recovery is handled in the stable branch
        }
        const int64_t base = d.has_last_valid ? d.last_complete_ms : d.first_seen_ms;
        if (now_ms - base > hold_timeout_ms_) {
            zone_inhibit_.insert(zone_no);
            newly_inhibited_.insert(zone_no);
            if (last_sent_.erase(zone_no) > 0) {
                changed = true;
            }
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
        // Evicting a zone must drop it from every container, exactly like
        // expiry does — otherwise a leftover zone_inhibit_/newly_inhibited_
        // entry orphans state for a zone that no longer exists (a stale
        // newly_inhibited_ entry would surface as a false alarm on the next
        // drain even though the zone was evicted before ever being reported).
        zone_inhibit_.erase(zone_no);
        newly_inhibited_.erase(zone_no);
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
        if (d.has_stable && zone_inhibit_.count(zone_no) == 0) {
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

std::set<int> ZoneAggregator::take_newly_inhibited() {
    std::set<int> out;
    out.swap(newly_inhibited_);
    return out;
}

} // namespace denso::ui
