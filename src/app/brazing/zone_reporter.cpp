#include "brazing/zone_reporter.h"

#include <algorithm>
#include <chrono>
#include <utility>

namespace denso::ui {

ZoneReporter::ZoneReporter(
    std::function<void(const std::map<int, ZoneValue>&, uint64_t)> on_snapshot,
    int stable_frames, std::function<int64_t()> clock, int64_t hold_timeout_ms)
    : on_snapshot_(std::move(on_snapshot)), clock_(std::move(clock)),
      aggregator_(stable_frames, kZoneExpiryMs, hold_timeout_ms) {}

int64_t ZoneReporter::now_ms() const {
    if (clock_) {
        return clock_();
    }
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

void ZoneReporter::on_zones(int64_t camera_id, const std::vector<ZoneReading>& zones) {
    const int64_t now_ms = this->now_ms();
    std::optional<std::map<int, ZoneValue>> snapshot;
    uint64_t seq = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // CAMERA-level causes drop the WHOLE observation. Checking this under the
        // same mutex as observe() is what kills the resurrection race: a worker
        // already blocked here cannot re-create a zone the inhibit just evicted.
        // Contrast the zone-level hold-timeout inhibit, which suppresses only
        // PUBLICATION so the zone can still recover (spec §3.1.1).
        if (inhibited_cameras_.count(camera_id) > 0) {
            return;
        }
        // Ownership is recorded from observations we ACCEPT, never derived from
        // current config: a renumbered ROI must still evict the zone we actually
        // published under (spec §3.3b).
        auto& owned = camera_zones_[camera_id];
        for (const ZoneReading& z : zones) {
            owned.insert(z.zone_no);
        }
        snapshot = aggregator_.observe(zones, now_ms);
        if (snapshot) {
            seq = ++seq_;
        }
    }
    if (snapshot && on_snapshot_) {
        on_snapshot_(*snapshot, seq);
    }
}

void ZoneReporter::set_configured_zones(int64_t camera_id, std::set<int> zone_nos) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (zone_nos.empty()) {
        configured_zones_.erase(camera_id);
    } else {
        configured_zones_[camera_id] = std::move(zone_nos);
    }
}

void ZoneReporter::clear_configured_zones() {
    std::lock_guard<std::mutex> lock(mutex_);
    configured_zones_.clear();
}

std::vector<ZoneRuntimeEntry> ZoneReporter::runtime_view() const {
    std::lock_guard<std::mutex> lock(mutex_);

    // Zone-level state, keyed by zone number, from the ONE authority.
    const std::vector<ZoneRuntime> zone_states = aggregator_.runtime_view();

    // How many cameras claim each zone number. A number claimed twice cannot be
    // attributed: the aggregator holds a single debounce per zone, so showing it
    // on both tiles would let one camera display the other's reading. Saves
    // already reject duplicates, so this only catches a legacy/corrupt DB.
    std::map<int, int> claims;
    for (const auto& [camera_id, zones] : configured_zones_) {
        (void)camera_id;
        for (const int zone_no : zones) {
            ++claims[zone_no];
        }
    }

    std::vector<ZoneRuntimeEntry> out;
    // Iteration is over CONFIGURED zones, not observed ones: a zone that has
    // never been observed must still render (as Acquiring), and an inhibited
    // camera's zones are never observed at all.
    for (const auto& [camera_id, zones] : configured_zones_) {
        const bool paused = inhibited_cameras_.count(camera_id) > 0;
        for (const int zone_no : zones) {
            ZoneRuntimeEntry e;
            e.camera_id = camera_id;
            e.zone_no   = zone_no;

            if (claims[zone_no] > 1) {
                // Broken configuration outranks everything: never guess an owner.
                e.state = ZoneDisplayState::Conflict;
            } else if (paused) {
                // The camera's observations are dropped wholesale, so any value
                // still held is not being maintained. Show no number.
                e.state = ZoneDisplayState::Paused;
            } else {
                const auto it = std::find_if(
                    zone_states.begin(), zone_states.end(),
                    [zone_no](const ZoneRuntime& z) { return z.zone_no == zone_no; });
                if (it == zone_states.end()) {
                    e.state = ZoneDisplayState::Acquiring;  // configured, not yet seen
                } else {
                    switch (it->state) {
                        case ZoneRuntimeState::Healthy:
                            e.state = ZoneDisplayState::Healthy;
                            e.value = it->value;
                            break;
                        case ZoneRuntimeState::HoldingLastValid:
                            e.state = ZoneDisplayState::HoldingLastValid;
                            e.value = it->value;
                            break;
                        case ZoneRuntimeState::Inhibited:
                            e.state = ZoneDisplayState::Inhibited;
                            break;
                        case ZoneRuntimeState::Acquiring:
                            e.state = ZoneDisplayState::Acquiring;
                            break;
                    }
                }
            }
            out.push_back(e);
        }
    }
    return out;
}

std::vector<ZoneInhibitOnset> ZoneReporter::take_newly_inhibited() {
    std::lock_guard<std::mutex> lock(mutex_);
    // FIRST and ONLY consumption: the aggregator hands the event set over
    // destructively, so whatever it withdrew in the meantime (a recovery, an
    // eviction) is simply not here. Nothing is cached on this side.
    const std::set<int> zone_nos = aggregator_.take_newly_inhibited();

    std::vector<ZoneInhibitOnset> out;
    for (const int zone_no : zone_nos) {
        // Observation-derived ownership, not the display map: an escalation is a
        // fact about what the appliance READ, so it is attributed to whoever
        // actually fed the zone. One record per claimant (see the header).
        for (const auto& [camera_id, owned] : camera_zones_) {
            if (owned.count(zone_no) > 0) {
                out.push_back(ZoneInhibitOnset{camera_id, zone_no});
            }
        }
    }
    // Deterministic order so a log file and a status document read the same way
    // on every run.
    std::sort(out.begin(), out.end(),
              [](const ZoneInhibitOnset& a, const ZoneInhibitOnset& b) {
                  return a.camera_id != b.camera_id ? a.camera_id < b.camera_id
                                                    : a.zone_no < b.zone_no;
              });
    return out;
}

void ZoneReporter::set_camera_inhibited(int64_t camera_id, bool on) {
    std::optional<std::map<int, ZoneValue>> snapshot;
    uint64_t seq = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (on) {
            // Mark and evict atomically — any gap between them is a window in which
            // a blocked observation could republish the zone.
            inhibited_cameras_.insert(camera_id);
            const auto it = camera_zones_.find(camera_id);
            if (it != camera_zones_.end()) {
                snapshot = aggregator_.evict_zones(it->second);
            }
        } else {
            // Release clears the flag only. The zone re-enters naturally on the next
            // observation, rebuilds a fresh Debounce (so it re-earns the debounce),
            // and reports even an unchanged value because last_sent_ no longer
            // holds it (spec §3.4).
            inhibited_cameras_.erase(camera_id);
        }
        // A suppressed (empty) snapshot must NOT consume a sequence number, or the
        // GUI's drop-stale rule would discard the next genuine snapshot (spec §3.3).
        if (snapshot) {
            seq = ++seq_;
        }
    }
    if (snapshot && on_snapshot_) {
        on_snapshot_(*snapshot, seq);
    }
}

} // namespace denso::ui
