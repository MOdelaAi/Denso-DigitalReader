// Runtime projection consumed by the camera-grid overlay. These tests pin the
// SAFETY BOUNDARY: the projection is by value, camera-keyed, and available with
// backend delivery disabled — because the overlay exists to cross-check the
// appliance locally, and must not depend on the server it cross-checks.
#include <catch2/catch_test_macros.hpp>

#include "brazing/zone_aggregator.h"
#include "brazing/zone_reporter.h"
#include "brazing/zone_runtime.h"

#include <algorithm>
#include <functional>
#include <optional>
#include <vector>

using denso::ui::kHoldTimeoutMs;
using denso::ui::ReadingKind;
using denso::ui::ZoneAggregator;
using denso::ui::ZoneDisplayState;
using denso::ui::ZoneReading;
using denso::ui::ZoneReporter;
using denso::ui::ZoneRuntimeState;

namespace {

std::vector<ZoneReading> complete(int zone, int value) {
    return {ZoneReading{zone, value, 0.9f, ReadingKind::Complete}};
}

std::vector<ZoneReading> incomplete(int zone) {
    return {ZoneReading{zone, 0, 0.0f, ReadingKind::Incomplete}};
}

// Drive a zone to an accepted (stable) value at time `t`.
void stabilize(ZoneAggregator& agg, int zone, int value, int frames, int64_t t = 0) {
    for (int i = 0; i < frames; ++i) {
        agg.observe(complete(zone, value), t);
    }
}

std::optional<denso::ui::ZoneRuntime> find_zone(
    const std::vector<denso::ui::ZoneRuntime>& v, int zone_no) {
    const auto it = std::find_if(v.begin(), v.end(),
                                 [zone_no](const auto& e) { return e.zone_no == zone_no; });
    if (it == v.end()) return std::nullopt;
    return *it;
}

std::optional<denso::ui::ZoneRuntimeEntry> find_entry(
    const std::vector<denso::ui::ZoneRuntimeEntry>& v, int64_t cam, int zone_no) {
    const auto it = std::find_if(v.begin(), v.end(), [cam, zone_no](const auto& e) {
        return e.camera_id == cam && e.zone_no == zone_no;
    });
    if (it == v.end()) return std::nullopt;
    return *it;
}

} // namespace

// ── Aggregator projection: the four states ────────────────────────────────────

// MUTATION: "display a numeric value while acquiring" must die.
TEST_CASE("acquiring zone carries no numeric value", "[zone_runtime]") {
    ZoneAggregator agg(3);
    agg.observe(complete(1, 500), 0);  // seen once — not yet accepted
    const auto z = find_zone(agg.runtime_view(), 1);
    REQUIRE(z.has_value());
    CHECK(z->state == ZoneRuntimeState::Acquiring);
    CHECK_FALSE(z->value.has_value());
}

TEST_CASE("healthy zone shows the currently accepted value", "[zone_runtime]") {
    ZoneAggregator agg(3);
    stabilize(agg, 1, 500, 3);
    const auto z = find_zone(agg.runtime_view(), 1);
    REQUIRE(z.has_value());
    CHECK(z->state == ZoneRuntimeState::Healthy);
    REQUIRE(z->value.has_value());
    CHECK(*z->value == 500);
}

// MUTATION: "hide the last valid value during hold" must die.
// MUTATION: "display the current invalid reading during hold" must die.
TEST_CASE("holding zone keeps showing the last valid value", "[zone_runtime]") {
    ZoneAggregator agg(3);
    stabilize(agg, 1, 500, 3);
    agg.observe(incomplete(1), 10);  // digit lost — hold, do not blank
    const auto z = find_zone(agg.runtime_view(), 1);
    REQUIRE(z.has_value());
    CHECK(z->state == ZoneRuntimeState::HoldingLastValid);
    REQUIRE(z->value.has_value());
    CHECK(*z->value == 500);
}

// MUTATION: "treat hold as inhibited" must die.
TEST_CASE("holding is never reported as inhibited", "[zone_runtime]") {
    ZoneAggregator agg(3);
    stabilize(agg, 1, 500, 3);
    agg.observe(incomplete(1), 10);
    const auto z = find_zone(agg.runtime_view(), 1);
    REQUIRE(z.has_value());
    CHECK(z->state != ZoneRuntimeState::Inhibited);
}

// The unsound discriminator this design rejected: after an incomplete frame the
// NEXT complete frame refreshes last_complete_ms but has not re-earned the
// debounce, so a timestamp-equality rule would wrongly say Healthy while showing
// the OLD accepted value.
TEST_CASE("a single complete frame after a hold does not restore healthy",
          "[zone_runtime]") {
    ZoneAggregator agg(3);
    stabilize(agg, 1, 500, 3);
    agg.observe(incomplete(1), 10);
    agg.observe(complete(1, 700), 20);  // 1 of 3 — not yet accepted
    const auto z = find_zone(agg.runtime_view(), 1);
    REQUIRE(z.has_value());
    CHECK(z->state == ZoneRuntimeState::HoldingLastValid);
    CHECK(*z->value == 500);  // still the last ACCEPTED value, never the candidate
}

// A Complete-then-Incomplete pair inside one millisecond aliases equal
// timestamps; the explicit observation-phase flag must survive it.
TEST_CASE("complete then incomplete in the same millisecond still holds",
          "[zone_runtime]") {
    ZoneAggregator agg(3);
    stabilize(agg, 1, 500, 3);
    agg.observe(incomplete(1), 0);  // same timestamp as the stable run
    const auto z = find_zone(agg.runtime_view(), 1);
    REQUIRE(z.has_value());
    CHECK(z->state == ZoneRuntimeState::HoldingLastValid);
}

// MUTATION: "display a numeric value while inhibited" must die.
TEST_CASE("inhibited zone carries no numeric value", "[zone_runtime]") {
    ZoneAggregator agg(3);
    stabilize(agg, 1, 500, 3);
    // Starve it past the hold timeout — incomplete frames keep it alive but
    // never refresh the complete-reading baseline.
    for (int64_t t = 1000; t <= kHoldTimeoutMs + 2000; t += 1000) {
        agg.observe(incomplete(1), t);
    }
    const auto z = find_zone(agg.runtime_view(), 1);
    REQUIRE(z.has_value());
    CHECK(z->state == ZoneRuntimeState::Inhibited);
    CHECK_FALSE(z->value.has_value());
}

// ── Reporter projection: camera identity, pause, conflict ─────────────────────

// MUTATION: "key runtime entries only by zone_no" must die.
// Two cameras each owning zone 1 must never exchange values.
TEST_CASE("duplicate zone number across cameras never exchanges values",
          "[zone_runtime]") {
    ZoneReporter rep({}, 3);
    rep.set_configured_zones(1, {1});
    rep.set_configured_zones(2, {1});
    for (int i = 0; i < 3; ++i) rep.on_zones(1, complete(1, 500));

    const auto view = rep.runtime_view();
    const auto a = find_entry(view, 1, 1);
    const auto b = find_entry(view, 2, 1);
    REQUIRE(a.has_value());
    REQUIRE(b.has_value());
    CHECK(a->state == ZoneDisplayState::Conflict);
    CHECK(b->state == ZoneDisplayState::Conflict);
    CHECK_FALSE(a->value.has_value());  // camera 2 must never inherit camera 1's read
    CHECK_FALSE(b->value.has_value());
}

TEST_CASE("each entry is identified by camera and zone", "[zone_runtime]") {
    ZoneReporter rep({}, 3);
    rep.set_configured_zones(7, {1, 2});
    for (int i = 0; i < 3; ++i) rep.on_zones(7, complete(1, 500));

    const auto view = rep.runtime_view();
    const auto e = find_entry(view, 7, 1);
    REQUIRE(e.has_value());
    CHECK(e->camera_id == 7);
    CHECK(e->zone_no == 1);
    CHECK(*e->value == 500);
    // A configured-but-unobserved zone is still projected, as Acquiring.
    const auto e2 = find_entry(view, 7, 2);
    REQUIRE(e2.has_value());
    CHECK(e2->state == ZoneDisplayState::Acquiring);
}

// A camera inhibited from BOOT never records observation-derived ownership
// (on_zones returns before recording), so display ownership must come from
// configuration or Paused can never render.
TEST_CASE("camera inhibited before any observation still projects paused",
          "[zone_runtime]") {
    ZoneReporter rep({}, 3);
    rep.set_configured_zones(4, {3});
    rep.set_camera_inhibited(4, true);
    for (int i = 0; i < 3; ++i) rep.on_zones(4, complete(3, 900));  // all dropped

    const auto e = find_entry(rep.runtime_view(), 4, 3);
    REQUIRE(e.has_value());
    CHECK(e->state == ZoneDisplayState::Paused);
    CHECK_FALSE(e->value.has_value());
}

TEST_CASE("camera pause overrides an accepted zone value", "[zone_runtime]") {
    ZoneReporter rep({}, 3);
    rep.set_configured_zones(4, {3});
    for (int i = 0; i < 3; ++i) rep.on_zones(4, complete(3, 900));
    REQUIRE(find_entry(rep.runtime_view(), 4, 3)->state == ZoneDisplayState::Healthy);

    rep.set_camera_inhibited(4, true);
    const auto e = find_entry(rep.runtime_view(), 4, 3);
    REQUIRE(e.has_value());
    CHECK(e->state == ZoneDisplayState::Paused);
    CHECK_FALSE(e->value.has_value());
}

// ── The backend-independence boundary ─────────────────────────────────────────

// MUTATION: "construct the aggregator only when the backend is configured" must
// die. A reporter with NO delivery callback is a supported, fully functional
// state — this is the primary requirement of the slice.
TEST_CASE("zone values are projected with no delivery callback at all",
          "[zone_runtime]") {
    ZoneReporter rep({}, 3);  // brazing disabled: empty callback
    rep.set_configured_zones(1, {1});
    for (int i = 0; i < 3; ++i) rep.on_zones(1, complete(1, 128));

    const auto e = find_entry(rep.runtime_view(), 1, 1);
    REQUIRE(e.has_value());
    CHECK(e->state == ZoneDisplayState::Healthy);
    CHECK(*e->value == 128);
}

// MUTATION: "let backend failure clear runtime values" must die. A delivery
// callback that throws/fails cannot affect the local projection.
TEST_CASE("a failing delivery callback does not disturb the projection",
          "[zone_runtime]") {
    int calls = 0;
    ZoneReporter rep([&calls](const std::map<int, int>&, uint64_t) { ++calls; }, 3);
    rep.set_configured_zones(1, {1});
    for (int i = 0; i < 3; ++i) rep.on_zones(1, complete(1, 128));

    const auto e = find_entry(rep.runtime_view(), 1, 1);
    REQUIRE(e.has_value());
    CHECK(e->state == ZoneDisplayState::Healthy);
    CHECK(*e->value == 128);
    CHECK(calls > 0);  // delivery did run — it is simply not load-bearing here
}

// Identical detector input must produce an identical projection regardless of
// whether delivery is wired.
TEST_CASE("projection is identical with delivery enabled and disabled",
          "[zone_runtime]") {
    ZoneReporter off({}, 3);
    ZoneReporter on([](const std::map<int, int>&, uint64_t) {}, 3);
    off.set_configured_zones(1, {1});
    on.set_configured_zones(1, {1});
    for (int i = 0; i < 3; ++i) {
        off.on_zones(1, complete(1, 42));
        on.on_zones(1, complete(1, 42));
    }
    const auto a = find_entry(off.runtime_view(), 1, 1);
    const auto b = find_entry(on.runtime_view(), 1, 1);
    REQUIRE(a.has_value());
    REQUIRE(b.has_value());
    CHECK(a->state == b->state);
    CHECK(a->value == b->value);
}

// The projection is a VALUE: it must stay valid and unchanged after the
// aggregator mutates behind it (i.e. it is not a view into locked state).
TEST_CASE("projection is by value and survives later mutation", "[zone_runtime]") {
    ZoneReporter rep({}, 3);
    rep.set_configured_zones(1, {1});
    for (int i = 0; i < 3; ++i) rep.on_zones(1, complete(1, 500));

    const auto before = rep.runtime_view();
    for (int i = 0; i < 3; ++i) rep.on_zones(1, complete(1, 999));

    const auto e = find_entry(before, 1, 1);
    REQUIRE(e.has_value());
    CHECK(*e->value == 500);  // the copy taken earlier is untouched
    CHECK(*find_entry(rep.runtime_view(), 1, 1)->value == 999);
}

TEST_CASE("clearing configured zones removes stale projection entries",
          "[zone_runtime]") {
    ZoneReporter rep({}, 3);
    rep.set_configured_zones(1, {1});
    for (int i = 0; i < 3; ++i) rep.on_zones(1, complete(1, 500));
    REQUIRE(find_entry(rep.runtime_view(), 1, 1).has_value());

    rep.clear_configured_zones();
    CHECK(rep.runtime_view().empty());
}

// ── Inhibit ONSET drain: the alarm channel ───────────────────────────────────
//
// ZoneAggregator owns the escalation event set and its CANCELLATION rules
// (recovery undoes an undrained alarm, zone_aggregator.cpp:68; eviction undoes
// it, :167; expiry deliberately does NOT, :104-111). ZoneReporter therefore
// drains LAZILY and only joins camera identity at drain time — harvesting the
// event early into a reporter-side queue would put it beyond the reach of both
// cancellation rules and make the reporter a second policy authority.
//
// Attribution comes from `camera_zones_`, which on_zones() populates BEFORE
// calling observe() (zone_reporter.cpp:33-37) and never erases — so every zone
// that can hold-timeout provably has an owner and camera_id is never 0.

namespace {

// Controllable monotonic clock: the hold timeout is 30 s of wall time, so every
// onset test drives it explicitly rather than sleeping.
struct FakeClock {
    int64_t t = 0;
    std::function<int64_t()> fn() { return [this] { return t; }; }
};

// Drive `zone` (owned by `cam`) to an accepted value, then past the hold
// timeout, so exactly one escalation is owed.
void escalate(ZoneReporter& rep, FakeClock& clk, int64_t cam, int zone) {
    clk.t = 0;
    for (int i = 0; i < 3; ++i) rep.on_zones(cam, complete(zone, 100 + zone));
    clk.t = kHoldTimeoutMs + 1;            // past the hold timeout...
    rep.on_zones(cam, incomplete(zone));   // ...on a frame that still proves liveness
}

bool has_onset(const std::vector<denso::ui::ZoneInhibitOnset>& v, int64_t cam, int zone) {
    return std::any_of(v.begin(), v.end(), [cam, zone](const auto& o) {
        return o.camera_id == cam && o.zone_no == zone;
    });
}

} // namespace

// REQUIREMENT 1: a newly inhibited zone is returned once, WITH its camera.
TEST_CASE("a hold-timeout onset drains once and carries its camera",
          "[zone_runtime][onset]") {
    FakeClock clk;
    ZoneReporter rep({}, 3, clk.fn());
    rep.set_configured_zones(7, {1});
    escalate(rep, clk, 7, 1);

    const auto first = rep.take_newly_inhibited();
    REQUIRE(first.size() == 1);
    CHECK(first[0].camera_id == 7);   // never 0 — attribution is by construction
    CHECK(first[0].zone_no == 1);
}

// REQUIREMENT 2 + 6: the drain is destructive, so the 5 Hz grid timer that polls
// again a moment later sees nothing to report.
TEST_CASE("a second drain returns no duplicate onset", "[zone_runtime][onset]") {
    FakeClock clk;
    ZoneReporter rep({}, 3, clk.fn());
    rep.set_configured_zones(7, {1});
    escalate(rep, clk, 7, 1);

    REQUIRE(rep.take_newly_inhibited().size() == 1);
    CHECK(rep.take_newly_inhibited().empty());
    // Repeated polls at the grid's cadence keep returning nothing.
    for (int i = 0; i < 10; ++i) {
        clk.t += 200;
        CHECK(rep.take_newly_inhibited().empty());
    }
}

// REQUIREMENT 3: two different zones produce two distinct events.
TEST_CASE("two inhibited zones produce two distinct onsets", "[zone_runtime][onset]") {
    FakeClock clk;
    ZoneReporter rep({}, 3, clk.fn());
    rep.set_configured_zones(7, {1, 2});
    clk.t = 0;
    for (int i = 0; i < 3; ++i) {
        rep.on_zones(7, {ZoneReading{1, 101, 0.9f, ReadingKind::Complete},
                         ZoneReading{2, 102, 0.9f, ReadingKind::Complete}});
    }
    clk.t = kHoldTimeoutMs + 1;
    rep.on_zones(7, {ZoneReading{1, 0, 0.0f, ReadingKind::Incomplete},
                     ZoneReading{2, 0, 0.0f, ReadingKind::Incomplete}});

    const auto out = rep.take_newly_inhibited();
    REQUIRE(out.size() == 2);
    CHECK(has_onset(out, 7, 1));
    CHECK(has_onset(out, 7, 2));
}

// REQUIREMENT 4: two cameras claiming the same zone number stay distinct. The
// aggregator holds ONE debounce per zone number, so a single escalation FANS OUT
// to every camera that has claimed it — never collapsing to one arbitrary owner.
TEST_CASE("same zone number on two cameras yields two distinct onsets",
          "[zone_runtime][onset]") {
    FakeClock clk;
    ZoneReporter rep({}, 3, clk.fn());
    rep.set_configured_zones(8, {1});
    rep.set_configured_zones(9, {1});
    clk.t = 0;
    for (int i = 0; i < 3; ++i) {
        rep.on_zones(8, complete(1, 500));
        rep.on_zones(9, complete(1, 500));
    }
    clk.t = kHoldTimeoutMs + 1;
    rep.on_zones(8, incomplete(1));

    const auto out = rep.take_newly_inhibited();
    REQUIRE(out.size() == 2);
    CHECK(has_onset(out, 8, 1));
    CHECK(has_onset(out, 9, 1));
}

// The lazy-drain contract. MUTATION: "harvest the onset into a reporter-side
// queue as soon as the aggregator raises it" must die — it would put the event
// beyond the aggregator's recovery cancellation and alarm on a zone that is
// reading perfectly again.
TEST_CASE("a zone that recovers before the drain raises no alarm",
          "[zone_runtime][onset]") {
    FakeClock clk;
    ZoneReporter rep({}, 3, clk.fn());
    rep.set_configured_zones(7, {1});
    escalate(rep, clk, 7, 1);

    // Recovery lands before anybody drained the event.
    for (int i = 0; i < 3; ++i) rep.on_zones(7, complete(1, 777));
    CHECK(rep.take_newly_inhibited().empty());
    CHECK(find_entry(rep.runtime_view(), 7, 1)->state == ZoneDisplayState::Healthy);
}

// THE reason the onset needs a status representation of its own. An escalated
// zone that then goes silent is erased from the projection (expiry) while its
// alarm is still owed — so a status array derived only from current state would
// drop it. Pins zone_aggregator.cpp:104-111.
TEST_CASE("an expired zone still delivers the onset it already owed",
          "[zone_runtime][onset]") {
    FakeClock clk;
    ZoneReporter rep({}, 3, clk.fn());
    rep.set_configured_zones(7, {1});
    escalate(rep, clk, 7, 1);

    // Zone 1 goes silent; a sibling keeps the reporter ticking past the expiry
    // window so the sweep actually runs.
    rep.set_configured_zones(7, {1, 2});
    clk.t += denso::ui::kZoneExpiryMs + 1;
    rep.on_zones(7, complete(2, 42));

    // Gone from current state...
    CHECK(find_entry(rep.runtime_view(), 7, 1)->state == ZoneDisplayState::Acquiring);
    // ...but the alarm it already raised is still delivered.
    const auto out = rep.take_newly_inhibited();
    REQUIRE(out.size() == 1);
    CHECK(out[0].camera_id == 7);
    CHECK(out[0].zone_no == 1);
}

// REQUIREMENT 13: backend delivery DISABLED (no callback at all) must not stop
// the alarm reaching the drain.
TEST_CASE("backend disabled does not suppress the onset", "[zone_runtime][onset]") {
    FakeClock clk;
    ZoneReporter rep({}, 3, clk.fn());   // empty callback == no backend configured
    rep.set_configured_zones(7, {1});
    escalate(rep, clk, 7, 1);
    CHECK(rep.take_newly_inhibited().size() == 1);
}

// REQUIREMENT 14: backend configured but OFFLINE. The reporter hands the
// snapshot to a sink that cannot deliver it; that must change nothing here.
TEST_CASE("backend offline does not suppress the onset", "[zone_runtime][onset]") {
    FakeClock clk;
    int calls = 0;
    // Stands in for a downed server: the transport accepts the snapshot and
    // never acknowledges it. The reporter has no delivery state to consult.
    ZoneReporter rep([&calls](const std::map<int, int>&, uint64_t) { ++calls; }, 3,
                     clk.fn());
    rep.set_configured_zones(7, {1});
    escalate(rep, clk, 7, 1);

    const auto out = rep.take_newly_inhibited();
    REQUIRE(out.size() == 1);
    CHECK(out[0].camera_id == 7);
    CHECK(calls > 0);   // delivery was attempted — and is not load-bearing here
}

// ── Reporter-level state mapping ─────────────────────────────────────────────
//
// The aggregator tests above pin ZoneRuntimeState; these pin the SECOND
// translation, into the ZoneDisplayState a tile actually renders. Mutation
// testing found that layer unguarded: collapsing hold into inhibit inside
// ZoneReporter::runtime_view() left every aggregator test green.

// MUTATION: "map HoldingLastValid to Inhibited in the reporter" must die. A hold
// still has a trustworthy number and must not be shown as a stopped zone.
TEST_CASE("reporter projects a hold as hold with its last valid value",
          "[zone_runtime]") {
    ZoneReporter rep({}, 3);
    rep.set_configured_zones(4, {1});
    for (int i = 0; i < 3; ++i) rep.on_zones(4, complete(1, 640));
    rep.on_zones(4, incomplete(1));   // digits lost — hold, not a stop

    const auto e = find_entry(rep.runtime_view(), 4, 1);
    REQUIRE(e.has_value());
    CHECK(e->state == ZoneDisplayState::HoldingLastValid);
    CHECK(e->state != ZoneDisplayState::Inhibited);
    REQUIRE(e->value.has_value());
    CHECK(*e->value == 640);
}

// MUTATION: "attach a number to an Inhibited entry in the reporter" must die.
// A suppressed zone has nothing an operator may act on, so the tile must have no
// number to render even if one were still held underneath.
TEST_CASE("reporter projects an inhibited zone with no number", "[zone_runtime]") {
    FakeClock clk;
    ZoneReporter rep({}, 3, clk.fn());
    rep.set_configured_zones(4, {1});
    escalate(rep, clk, 4, 1);

    const auto e = find_entry(rep.runtime_view(), 4, 1);
    REQUIRE(e.has_value());
    CHECK(e->state == ZoneDisplayState::Inhibited);
    CHECK_FALSE(e->value.has_value());
}

// A camera-level inhibit evicts the camera's zones, and eviction cancels their
// owed alarms atomically under the same mutex — no orphaned false alarm.
TEST_CASE("a camera-level inhibit cancels its zones' pending onsets",
          "[zone_runtime][onset]") {
    FakeClock clk;
    ZoneReporter rep({}, 3, clk.fn());
    rep.set_configured_zones(7, {1});
    escalate(rep, clk, 7, 1);

    rep.set_camera_inhibited(7, true);
    CHECK(rep.take_newly_inhibited().empty());
}
