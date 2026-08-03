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
