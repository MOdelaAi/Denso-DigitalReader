// What has actually reached status.json, and what is still owed.
//
// This exists because two things in the overlay slice are LOSSY if a write
// failure is treated as a success:
//   • the 5 Hz change throttle — marking an unpublished projection as published
//     suppresses every retry until the zone picture happens to move again;
//   • the onset buffer — the aggregator's drain is DESTRUCTIVE, so once an onset
//     has been drained this buffer is the only thing holding it. Dropping it on a
//     failed write loses the alarm from status output permanently.
//
// write_status_file() returns bool and CAN fail (unopenable path, full disk); its
// own tests already cover that. These pin what the grid does with that answer.
#include <catch2/catch_test_macros.hpp>

#include "ui/camera/grid/zone_status_publication.h"

#include <vector>

using denso::ui::ZoneStatusPublication;
using Projection = ZoneStatusPublication::Projection;

namespace {

denso::health::ZoneInhibitRecord onset(int64_t cam, int zone) {
    return {cam, zone, QStringLiteral("hold_timeout")};
}

Projection proj(std::set<int> held, std::set<int> inhibited) {
    return {std::move(held), std::move(inhibited)};
}

} // namespace

TEST_CASE("nothing is owed when the picture has not moved", "[zone_status]") {
    ZoneStatusPublication p;
    p.on_write(true, proj({}, {}));
    CHECK_FALSE(p.needs_write(proj({}, {})));
}

TEST_CASE("a moved zone picture owes a write", "[zone_status]") {
    ZoneStatusPublication p;
    p.on_write(true, proj({}, {}));
    CHECK(p.needs_write(proj({}, {3})));
}

TEST_CASE("a drained onset owes a write even when the picture is unchanged",
          "[zone_status]") {
    ZoneStatusPublication p;
    p.on_write(true, proj({}, {3}));
    REQUIRE_FALSE(p.needs_write(proj({}, {3})));

    // An escalation can land on a tick where the standing picture is identical —
    // an expired zone leaves the projection entirely while still owing its alarm.
    p.enqueue({onset(7, 3)});
    CHECK(p.needs_write(proj({}, {3})));
}

// THE finding this class was extracted for.
TEST_CASE("a FAILED write keeps the onset owed", "[zone_status]") {
    ZoneStatusPublication p;
    p.enqueue({onset(7, 3)});
    REQUIRE(p.pending().size() == 1);

    p.on_write(false, proj({}, {3}));
    CHECK(p.pending().size() == 1);          // not dropped — nothing was published
    CHECK(p.needs_write(proj({}, {3})));     // and the retry is still due

    p.on_write(true, proj({}, {3}));
    CHECK(p.pending().empty());              // published at last
    CHECK_FALSE(p.needs_write(proj({}, {3})));
}

// The throttle must not be poisoned by a failure: the projection the grid tried
// to publish is NOT the projection that reached the file.
TEST_CASE("a FAILED write does not advance the published picture", "[zone_status]") {
    ZoneStatusPublication p;
    p.on_write(true, proj({}, {}));

    p.on_write(false, proj({}, {3}));
    CHECK(p.needs_write(proj({}, {3})));   // still owed, so the next tick retries
    // ...and keeps being owed for as long as the write keeps failing.
    for (int i = 0; i < 5; ++i) {
        p.on_write(false, proj({}, {3}));
        CHECK(p.needs_write(proj({}, {3})));
    }
    p.on_write(true, proj({}, {3}));
    CHECK_FALSE(p.needs_write(proj({}, {3})));
}

TEST_CASE("onsets accumulate across several failed writes", "[zone_status]") {
    ZoneStatusPublication p;
    p.enqueue({onset(7, 1)});
    p.on_write(false, proj({}, {1}));
    p.enqueue({onset(7, 2)});
    p.on_write(false, proj({}, {1, 2}));

    REQUIRE(p.pending().size() == 2);
    CHECK(p.pending()[0].zone_no == 1);
    CHECK(p.pending()[1].zone_no == 2);
}

// Bounded, because this runs for months. The rotating log is the durable record
// of every alarm, so the STATUS buffer may shed the oldest rather than grow.
TEST_CASE("the pending buffer is bounded and keeps the newest", "[zone_status]") {
    ZoneStatusPublication p;
    const size_t over = ZoneStatusPublication::kMaxPending + 10;
    for (size_t i = 0; i < over; ++i) {
        p.enqueue({onset(7, static_cast<int>(i))});
        p.on_write(false, proj({}, {}));   // status.json unwritable throughout
    }
    REQUIRE(p.pending().size() == ZoneStatusPublication::kMaxPending);
    // The most recent alarm survives; the oldest were shed.
    CHECK(p.pending().back().zone_no == static_cast<int>(over - 1));
    CHECK(p.pending().front().zone_no ==
          static_cast<int>(over - ZoneStatusPublication::kMaxPending));
}

// A grid rebuild must republish the zone picture from scratch, but must NOT throw
// away an alarm that has been logged and not yet published.
TEST_CASE("resetting the published picture keeps owed alarms", "[zone_status]") {
    ZoneStatusPublication p;
    p.enqueue({onset(7, 3)});
    p.on_write(false, proj({}, {3}));

    p.reset_published();
    CHECK(p.pending().size() == 1);
    CHECK(p.needs_write(proj({}, {})));
}

// A fresh tracker has published NOTHING, which is not the same as having published
// an empty picture. Until the first successful write, every state is unpublished.
TEST_CASE("nothing has been published before the first successful write",
          "[zone_status]") {
    ZoneStatusPublication p;
    CHECK(p.needs_write(proj({}, {})));   // no alarms, empty picture — still owed
    p.on_write(false, proj({}, {}));
    CHECK(p.needs_write(proj({}, {})));   // a failure publishes nothing
    p.on_write(true, proj({}, {}));
    CHECK_FALSE(p.needs_write(proj({}, {})));
}

// MUTATION: "use an empty projection as the unpublished sentinel" must die.
// After a rebuild the file may still describe the OLD grid's inhibited zones. If
// the new grid's picture is ALSO empty, an empty-as-sentinel tracker would call
// that already-published and never correct the stale file.
TEST_CASE("a rebuild owes a write even when both pictures are empty",
          "[zone_status]") {
    ZoneStatusPublication p;
    p.on_write(true, proj({}, {3}));      // file now says zone 3 inhibited
    REQUIRE_FALSE(p.needs_write(proj({}, {3})));

    p.reset_published();                  // grid rebuilt; new grid has no zones
    CHECK(p.needs_write(proj({}, {})));   // the stale file MUST be corrected
}
