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
#include "brazing/zone_runtime.h"

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <vector>

namespace denso::ui {

class ZoneReporter : public ZoneSink {
public:
    /// on_snapshot receives the full payload plus a monotonic sequence number.
    /// The sequence lets the GUI side drop a snapshot that overtook a newer one:
    /// callbacks fire OUTSIDE the mutex and marshal from several threads, so an
    /// eviction and a recovery can otherwise arrive reversed (spec §3.3d).
    /// `clock` returns a monotonic time in ms. Left empty in production, where
    /// steady_clock is read directly; injected by tests so the 30 s hold timeout
    /// is driven deterministically instead of slept through. Same seam idiom as
    /// ZoneAggregator::observe(zones, now_ms) and LogEpisode.
    ///
    /// `stable_frames` and `hold_timeout_ms` are forwarded verbatim to the ONE
    /// aggregator this reporter owns. They are the two knobs the Ball Leveler
    /// turns (1 and 0 respectively — amendment §10.4) so that a continuous,
    /// quantized measurement publishes at all and never keeps an old percentage
    /// alive. Everything else about aggregation, delivery, retry and payload is
    /// identical between the modes, which is why there is one reporter and not
    /// two.
    explicit ZoneReporter(
        std::function<void(const std::map<int, ZoneValue>&, uint64_t)> on_snapshot,
        int stable_frames = kStableFrames,
        std::function<int64_t()> clock = {},
        int64_t hold_timeout_ms = kHoldTimeoutMs);

    void on_zones(int64_t camera_id, const std::vector<ZoneReading>& zones) override;

    /// Assert or clear a CAMERA-level inhibit. Asserting evicts the camera's
    /// recorded zones atomically with marking it inhibited. Call on the GUI thread.
    void set_camera_inhibited(int64_t camera_id, bool on);

    /// Install a camera's CONFIGURED zone numbers, for DISPLAY ROUTING only.
    /// Separate from camera_zones_ on purpose: that map is recorded from
    /// accepted observations so an eviction targets what was actually published
    /// (spec §3.3b), and on_zones() returns on the inhibit check BEFORE
    /// recording — so a camera inhibited at boot owns nothing there and could
    /// never render Paused. This map holds no policy and never feeds eviction.
    void set_configured_zones(int64_t camera_id, std::set<int> zone_nos);

    /// Forget all display routing (grid teardown / reload).
    void clear_configured_zones();

    /// Open a new delivery epoch for a backend sender that was just created or
    /// replaced. Called on the GUI thread from CameraGrid::apply_brazing_config.
    ///
    /// Two effects, taken together under this reporter's mutex so they cannot be
    /// split by a concurrent observation:
    ///   1. the aggregator's last-sent baseline is dropped, so the next snapshot
    ///      that meets the existing stability requirement is published even at an
    ///      unchanged value (see ZoneAggregator::reset_sent_baseline);
    ///   2. the RETURNED value is the sequence number of the last snapshot
    ///      published before this call — the reload BARRIER.
    ///
    /// The barrier is what makes the swap clean. Snapshots travel to the GUI
    /// thread as queued calls, so one published just before a Settings Save can
    /// still be sitting in the event queue when the replacement sender is built;
    /// delivering it would hand the new backend a pre-barrier payload the
    /// retired sender owned. A caller drops everything with `seq <= barrier`.
    ///
    /// Publishes nothing itself and relaxes no debounce.
    uint64_t reset_delivery_baseline();

    /// Camera-keyed projection for the grid overlay, built under the same mutex
    /// as observe(). Returns COPIES, so the caller may use it after the lock is
    /// released. Every entry carries BOTH camera_id and zone_no: joining by zone
    /// number alone would let one camera display a value the other produced.
    std::vector<ZoneRuntimeEntry> runtime_view() const;

    /// Drain the zone escalations the aggregator has raised since the last call,
    /// joined to the camera that owns each zone. DESTRUCTIVE — each onset is
    /// returned exactly once, which is what lets a caller log an alarm without
    /// re-announcing it on every poll.
    ///
    /// The drain is LAZY on purpose. ZoneAggregator owns the event set AND its
    /// cancellation rules: a zone that recovers before anyone drains has its
    /// escalation withdrawn (zone_aggregator.cpp:68), as does one evicted by a
    /// camera-level inhibit (:167), while an expired zone keeps its owed alarm
    /// (:104-111). Harvesting events early into a queue here would place them
    /// beyond both withdrawals and make this class a second policy authority.
    ///
    /// Attribution comes from `camera_zones_`, and is total by construction:
    /// on_zones() records ownership BEFORE calling observe(), so a zone can only
    /// enter the aggregator through a call that already named its camera, and
    /// nothing ever erases the map. `camera_id` is therefore never 0.
    /// Because the map is monotonic and the aggregator holds ONE debounce per
    /// zone number, a zone number claimed by more than one camera FANS OUT to one
    /// onset per claimant rather than picking an arbitrary owner — the same
    /// refusal to guess that renders those tiles as Conflict. "Exactly once"
    /// means once per emitted (camera_id, zone_no) record.
    std::vector<ZoneInhibitOnset> take_newly_inhibited();

private:
    /// Monotonic now in ms: the injected clock when present, else steady_clock.
    int64_t now_ms() const;

    std::function<void(const std::map<int, ZoneValue>&, uint64_t)> on_snapshot_;
    std::function<int64_t()> clock_;
    mutable std::mutex mutex_;   // mutable: runtime_view() is a const read
    ZoneAggregator  aggregator_;
    std::set<int64_t>                inhibited_cameras_;
    std::map<int64_t, std::set<int>> camera_zones_;      // accepted obs -> eviction
    std::map<int64_t, std::set<int>> configured_zones_;  // config -> display only
    uint64_t                         seq_ = 0;
};

} // namespace denso::ui
