#include <catch2/catch_test_macros.hpp>

#include "brazing/zone_aggregator.h"

using denso::ui::ZoneAggregator;
using denso::ui::ZoneReading;
using denso::ui::ReadingKind;

static std::vector<ZoneReading> obs(int zone, int value) {
    return {ZoneReading{zone, value, 0.9f}};
}

TEST_CASE("value sends only after it is stable for kStableFrames", "[zone_aggregator]") {
    ZoneAggregator agg(3);  // stable after 3 identical observations
    CHECK_FALSE(agg.observe(obs(1, 500)).has_value());  // 1
    CHECK_FALSE(agg.observe(obs(1, 500)).has_value());  // 2
    auto snap = agg.observe(obs(1, 500));               // 3 -> stable + new
    REQUIRE(snap.has_value());
    CHECK((*snap).at(1) == 500);
}

TEST_CASE("an unchanged stable value does not re-send", "[zone_aggregator]") {
    ZoneAggregator agg(1);  // stable immediately
    REQUIRE(agg.observe(obs(1, 500)).has_value());       // first send
    CHECK_FALSE(agg.observe(obs(1, 500)).has_value());   // same -> no send
}

TEST_CASE("a change in one zone sends the full snapshot of all zones", "[zone_aggregator]") {
    ZoneAggregator agg(1);
    REQUIRE(agg.observe(obs(1, 500)).has_value());
    REQUIRE(agg.observe(obs(2, 200)).has_value());
    auto snap = agg.observe(obs(1, 510));  // zone1 changed
    REQUIRE(snap.has_value());
    CHECK((*snap).at(1) == 510);
    CHECK((*snap).at(2) == 200);  // full snapshot carries zone2 too
}

TEST_CASE("a frame missing a zone keeps its last stable value", "[zone_aggregator]") {
    ZoneAggregator agg(1);
    REQUIRE(agg.observe(obs(1, 500)).has_value());     // zone1 stable at 500
    CHECK(agg.observe(obs(2, 200)).has_value());       // zone2 new -> sends
    CHECK_FALSE(agg.observe(obs(2, 200)).has_value()); // zone2 unchanged -> no send
    // Observations that never mention zone 1 must not have reset or cleared it:
    // a later change still carries zone1's retained 500 base alongside the change.
    auto s2 = agg.observe(obs(1, 505));                // zone1 changes 500 -> 505
    REQUIRE(s2.has_value());
    CHECK((*s2).at(1) == 505);
    CHECK((*s2).at(2) == 200);  // zone2 retained
}

TEST_CASE("an expired zone is dropped from the emitted snapshot", "[zone_aggregator]") {
    ZoneAggregator agg(1, 100);  // stable immediately, expire after 100 ms

    auto first = agg.observe(obs(1, 500), 0);
    REQUIRE(first.has_value());
    CHECK(first->at(1) == 500);

    // A later observation of a different zone, past zone1's expiry window, must
    // evict zone1 from the payload rather than keep re-sending its stale value.
    auto after_expiry = agg.observe(obs(2, 200), 101);
    REQUIRE(after_expiry.has_value());
    CHECK_FALSE(after_expiry->contains(1));
    REQUIRE(after_expiry->contains(2));
    CHECK(after_expiry->at(2) == 200);
}

TEST_CASE("a zone refreshed within the expiry window is retained", "[zone_aggregator]") {
    ZoneAggregator agg(1, 100);

    REQUIRE(agg.observe(obs(1, 500), 0).has_value());
    CHECK_FALSE(agg.observe(obs(1, 500), 100).has_value());  // refreshed at t=100

    auto snapshot = agg.observe(obs(2, 200), 150);  // still within zone1's window
    REQUIRE(snapshot.has_value());
    REQUIRE(snapshot->contains(1));
    CHECK(snapshot->at(1) == 500);
    REQUIRE(snapshot->contains(2));
    CHECK(snapshot->at(2) == 200);
}

TEST_CASE("an expired zone is sent again when it reappears", "[zone_aggregator]") {
    ZoneAggregator agg(1, 100);

    REQUIRE(agg.observe(obs(1, 500), 0).has_value());

    auto expired = agg.observe({}, 101);  // no zones observed; zone1 ages out
    REQUIRE(expired.has_value());
    CHECK(expired->empty());

    auto reappeared = agg.observe(obs(1, 500), 102);
    REQUIRE(reappeared.has_value());
    REQUIRE(reappeared->contains(1));
    CHECK(reappeared->at(1) == 500);
}

static ZoneReading rd(int zone, int value, ReadingKind k = ReadingKind::Complete) {
    ZoneReading r; r.zone_no = zone; r.value = value; r.conf = 0.9f; r.kind = k;
    return r;
}
// Drive `n` identical complete frames; returns the last snapshot emitted.
static std::optional<std::map<int,int>> feed(ZoneAggregator& a, int zone, int value,
                                             int n, int64_t& t) {
    std::optional<std::map<int,int>> last;
    for (int i = 0; i < n; ++i) { t += 100; if (auto s = a.observe({rd(zone, value)}, t)) last = s; }
    return last;
}

TEST_CASE("hold: an incomplete reading does not change the reported value",
          "[zone_aggregator]") {
    ZoneAggregator a(5, 10000);
    int64_t t = 0;
    REQUIRE(feed(a, 1, 42, 5, t).has_value());          // 42 becomes stable and is sent
    t += 100;
    REQUIRE_FALSE(a.observe({rd(1, 0, ReadingKind::Incomplete)}, t).has_value());
}

TEST_CASE("hold: incomplete readings keep the zone alive past the expiry window",
          "[zone_aggregator]") {
    ZoneAggregator a(5, 10000);
    int64_t t = 0;
    feed(a, 1, 42, 5, t);
    // 15s of incomplete frames — longer than the 10s expiry. The zone must NOT be
    // evicted, so no shrunk snapshot is emitted.
    for (int i = 0; i < 150; ++i) {
        t += 100;
        REQUIRE_FALSE(a.observe({rd(1, 0, ReadingKind::Incomplete)}, t).has_value());
    }
}

TEST_CASE("hold: NoDigits also keeps the zone alive", "[zone_aggregator]") {
    ZoneAggregator a(5, 10000);
    int64_t t = 0;
    feed(a, 1, 42, 5, t);
    for (int i = 0; i < 150; ++i) {
        t += 100;
        REQUIRE_FALSE(a.observe({rd(1, 0, ReadingKind::NoDigits)}, t).has_value());
    }
}

TEST_CASE("hold: recovery re-announces even when the value is UNCHANGED",
          "[zone_aggregator]") {
    ZoneAggregator a(5, 10000);
    int64_t t = 0;
    feed(a, 1, 42, 5, t);
    for (int i = 0; i < 10; ++i) { t += 100; a.observe({rd(1, 0, ReadingKind::Incomplete)}, t); }
    const auto snap = feed(a, 1, 42, 5, t);   // same value as before the hold
    REQUIRE(snap.has_value());
    REQUIRE(snap->at(1) == 42);
}

TEST_CASE("hold: frames either side of a gap do not combine into one stable run",
          "[zone_aggregator]") {
    ZoneAggregator a(5, 10000);
    int64_t t = 0;
    feed(a, 1, 42, 3, t);                                  // 3 of the 5 needed
    t += 100; a.observe({rd(1, 0, ReadingKind::Incomplete)}, t);
    REQUIRE_FALSE(feed(a, 1, 42, 3, t).has_value());        // 3 more must NOT reach 5
    REQUIRE(feed(a, 1, 42, 2, t).has_value());              // 5 consecutive completes
}

TEST_CASE("hold: a sibling zone on the same camera keeps reporting",
          "[zone_aggregator]") {
    ZoneAggregator a(5, 10000);
    int64_t t = 0;
    for (int i = 0; i < 5; ++i) { t += 100; a.observe({rd(1, 11), rd(2, 22)}, t); }
    // Zone 1 goes incomplete; zone 2 changes and must still be reported.
    std::optional<std::map<int,int>> snap;
    for (int i = 0; i < 5; ++i) {
        t += 100;
        if (auto s = a.observe({rd(1, 0, ReadingKind::Incomplete), rd(2, 33)}, t)) snap = s;
    }
    REQUIRE(snap.has_value());
    REQUIRE((*snap)[2] == 33);
    REQUIRE((*snap)[1] == 11);   // zone 1 still carries its held value
}
