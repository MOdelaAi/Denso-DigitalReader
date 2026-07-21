#include <catch2/catch_test_macros.hpp>
#include "health/zone_health.h"
#include <vector>
using namespace denso::health;

TEST_CASE("zone_health: causes compose and release only when ALL clear",
          "[zone_health]") {
    std::vector<std::pair<int64_t,bool>> calls;
    ZoneHealth h([&](int64_t id, bool on){ calls.push_back({id, on}); });

    h.set_cause(1, ZoneCause::CaptureOffline, true);
    h.set_cause(1, ZoneCause::InferenceWorkerFailed, true);
    REQUIRE(h.is_inhibited(1));
    REQUIRE(calls.size() == 1);            // one transition into inhibited
    REQUIRE(calls[0] == std::make_pair<int64_t,bool>(1, true));

    h.set_cause(1, ZoneCause::CaptureOffline, false);
    REQUIRE(h.is_inhibited(1));            // still held by the other cause
    REQUIRE(calls.size() == 1);            // no spurious release

    h.set_cause(1, ZoneCause::InferenceWorkerFailed, false);
    REQUIRE_FALSE(h.is_inhibited(1));
    REQUIRE(calls.size() == 2);
    REQUIRE(calls[1] == std::make_pair<int64_t,bool>(1, false));
}

TEST_CASE("zone_health: setting the same cause twice does not re-notify",
          "[zone_health]") {
    std::vector<std::pair<int64_t,bool>> calls;
    ZoneHealth h([&](int64_t id, bool on){ calls.push_back({id, on}); });
    h.set_cause(1, ZoneCause::CaptureOffline, true);
    h.set_cause(1, ZoneCause::CaptureOffline, true);
    REQUIRE(calls.size() == 1);
}

TEST_CASE("zone_health: cameras are independent", "[zone_health]") {
    std::vector<std::pair<int64_t,bool>> calls;
    ZoneHealth h([&](int64_t id, bool on){ calls.push_back({id, on}); });
    h.set_cause(1, ZoneCause::CaptureOffline, true);
    REQUIRE(h.is_inhibited(1));
    REQUIRE_FALSE(h.is_inhibited(2));
}

TEST_CASE("zone_health: clearing a cause for an unseen camera creates no entry",
          "[zone_health]") {
    std::vector<std::pair<int64_t,bool>> calls;
    ZoneHealth h([&](int64_t id, bool on){ calls.push_back({id, on}); });
    h.set_cause(42, ZoneCause::CaptureOffline, false);   // never seen before
    REQUIRE(calls.empty());
    REQUIRE(h.all().empty());              // no phantom mask-0 entry inserted
    REQUIRE_FALSE(h.is_inhibited(42));
}

TEST_CASE("zone_health: a fully-recovered camera is dropped from all()",
          "[zone_health]") {
    std::vector<std::pair<int64_t,bool>> calls;
    ZoneHealth h([&](int64_t id, bool on){ calls.push_back({id, on}); });
    h.set_cause(1, ZoneCause::CaptureOffline, true);
    h.set_cause(1, ZoneCause::InferenceWorkerFailed, true);
    REQUIRE(h.all().count(1) == 1);
    h.set_cause(1, ZoneCause::CaptureOffline, false);
    REQUIRE(h.all().count(1) == 1);        // still held by the other cause
    h.set_cause(1, ZoneCause::InferenceWorkerFailed, false);
    REQUIRE(h.all().empty());              // last cause cleared -> entry retired
    REQUIRE_FALSE(h.is_inhibited(1));
}
