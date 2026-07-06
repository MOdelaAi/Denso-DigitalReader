#include <catch2/catch_test_macros.hpp>

#include "ui/camera/grid/zone_aggregator.h"

using denso::ui::ZoneAggregator;
using denso::ui::ZoneReading;

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
