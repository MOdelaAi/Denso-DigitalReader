#include <catch2/catch_test_macros.hpp>
#include "brazing/zone_reporter.h"
#include <map>
#include <vector>
#include "zone_value_compat.h"
using namespace denso::ui;

namespace {
struct Captured { std::map<int, denso::ui::ZoneValue> snap; uint64_t seq; };

static ZoneReading rd(int zone, int value, ReadingKind k = ReadingKind::Complete) {
    ZoneReading r; r.zone_no = zone; r.value = denso::ui::ZoneValue{value}; r.conf = 0.9f; r.kind = k;
    return r;
}
} // namespace

TEST_CASE("reporter: an inhibited camera's observations are DROPPED", "[zone_reporter]") {
    std::vector<Captured> got;
    ZoneReporter r([&](const std::map<int, denso::ui::ZoneValue>& s, uint64_t q){ got.push_back({s,q}); }, 5);
    for (int i = 0; i < 5; ++i) r.on_zones(1, {rd(10, 42)});
    REQUIRE(got.size() == 1);
    got.clear();
    r.set_camera_inhibited(1, true);
    // Simulates the resurrection race: an observation that was already in flight
    // when the inhibit landed must not re-create the zone.
    for (int i = 0; i < 20; ++i) r.on_zones(1, {rd(10, 99)});
    for (const auto& c : got) REQUIRE(c.snap.count(10) == 0);
}

TEST_CASE("reporter: inhibiting a camera evicts its zones and emits", "[zone_reporter]") {
    std::vector<Captured> got;
    ZoneReporter r([&](const std::map<int, denso::ui::ZoneValue>& s, uint64_t q){ got.push_back({s,q}); }, 5);
    for (int i = 0; i < 5; ++i) r.on_zones(1, {rd(10, 42)});
    for (int i = 0; i < 5; ++i) r.on_zones(2, {rd(20, 7)});
    got.clear();
    r.set_camera_inhibited(1, true);
    REQUIRE(got.size() == 1);
    REQUIRE(got[0].snap.count(10) == 0);
    REQUIRE(got[0].snap.at(20) == 7);
}

TEST_CASE("reporter: release lets the camera report again with a forced value",
          "[zone_reporter]") {
    std::vector<Captured> got;
    ZoneReporter r([&](const std::map<int, denso::ui::ZoneValue>& s, uint64_t q){ got.push_back({s,q}); }, 5);
    for (int i = 0; i < 5; ++i) r.on_zones(1, {rd(10, 42)});
    for (int i = 0; i < 5; ++i) r.on_zones(2, {rd(20, 7)});
    r.set_camera_inhibited(1, true);
    got.clear();
    r.set_camera_inhibited(1, false);
    for (int i = 0; i < 5; ++i) r.on_zones(1, {rd(10, 42)});   // unchanged value
    REQUIRE_FALSE(got.empty());
    REQUIRE(got.back().snap.at(10) == 42);
}

TEST_CASE("reporter: ownership is recorded, so a renumbered ROI evicts the OLD zone",
          "[zone_reporter]") {
    std::vector<Captured> got;
    ZoneReporter r([&](const std::map<int, denso::ui::ZoneValue>& s, uint64_t q){ got.push_back({s,q}); }, 5);
    for (int i = 0; i < 5; ++i) r.on_zones(1, {rd(7, 42)});    // camera 1 owns zone 7
    for (int i = 0; i < 5; ++i) r.on_zones(2, {rd(20, 1)});
    got.clear();
    r.set_camera_inhibited(1, true);
    REQUIRE_FALSE(got.empty());
    REQUIRE(got.back().snap.count(7) == 0);   // the OLD zone is gone
}

TEST_CASE("reporter: sequence numbers increase and skip suppressed snapshots",
          "[zone_reporter]") {
    std::vector<Captured> got;
    ZoneReporter r([&](const std::map<int, denso::ui::ZoneValue>& s, uint64_t q){ got.push_back({s,q}); }, 5);
    for (int i = 0; i < 5; ++i) r.on_zones(1, {rd(10, 1)});
    for (int i = 0; i < 5; ++i) r.on_zones(1, {rd(10, 2)});
    REQUIRE(got.size() == 2);
    REQUIRE(got[1].seq > got[0].seq);
    const uint64_t last = got.back().seq;
    r.set_camera_inhibited(1, true);   // evicting the only zone -> empty -> suppressed
    for (const auto& c : got) REQUIRE(c.seq <= last + 1);
}

TEST_CASE("reporter: resetting the delivery baseline republishes the current value once",
          "[zone_reporter]") {
    // The first-reading barrier as the CameraGrid uses it: a sender that comes
    // into existence after a zone is already stable must still be told the value,
    // exactly once, at the next observation that meets the stability bar.
    std::vector<Captured> got;
    ZoneReporter r([&](const std::map<int, denso::ui::ZoneValue>& s, uint64_t q) {
        got.push_back({s, q});
    }, 1);

    r.on_zones(1, {rd(10, 42)});
    REQUIRE(got.size() == 1);
    r.on_zones(1, {rd(10, 42)});
    REQUIRE(got.size() == 1);        // unchanged value: normally suppressed

    const uint64_t barrier = r.reset_delivery_baseline();  // a new sender was built

    // THE BARRIER: every snapshot published before the new sender existed is at
    // or below it, and every one after is above. That is exactly the test the
    // grid applies to drop a queued pre-swap payload.
    CHECK(barrier == got.front().seq);

    r.on_zones(1, {rd(10, 42)});
    REQUIRE(got.size() == 2);        // …published again, same value
    CHECK(got.back().snap.at(10) == 42);
    // The sequence number still advances monotonically, so the grid's drop-stale
    // guard cannot mistake this for an overtaken snapshot.
    CHECK(got.back().seq > got.front().seq);
    CHECK(got.back().seq > barrier);   // the post-barrier snapshot is deliverable

    // Suppression resumes.
    r.on_zones(1, {rd(10, 42)});
    r.on_zones(1, {rd(10, 42)});
    CHECK(got.size() == 2);
}

TEST_CASE("reporter: resetting the delivery baseline publishes nothing on its own",
          "[zone_reporter]") {
    // Saving Settings must not, by itself, put a payload on the wire.
    std::vector<Captured> got;
    ZoneReporter r([&](const std::map<int, denso::ui::ZoneValue>& s, uint64_t q) {
        got.push_back({s, q});
    }, 1);
    r.on_zones(1, {rd(10, 42)});
    REQUIRE(got.size() == 1);

    const uint64_t barrier = r.reset_delivery_baseline();
    CHECK(got.size() == 1);          // no callback fired
    CHECK(barrier == got.front().seq);
}

TEST_CASE("reporter: the barrier of a reporter that never published is zero",
          "[zone_reporter]") {
    // The boot case: the sender is built alongside a brand-new reporter, so the
    // barrier must not swallow the appliance's very first snapshot.
    std::vector<Captured> got;
    ZoneReporter r([&](const std::map<int, denso::ui::ZoneValue>& s, uint64_t q) {
        got.push_back({s, q});
    }, 1);
    CHECK(r.reset_delivery_baseline() == 0);

    r.on_zones(1, {rd(10, 42)});
    REQUIRE(got.size() == 1);
    CHECK(got.front().seq > 0);      // strictly above the barrier: deliverable
}
