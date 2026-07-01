#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "ui/camera/grid/fps_meter.h"

#include <chrono>

using Catch::Approx;
using denso::ui::FpsMeter;

namespace {
// A steady_clock time point at `ms` milliseconds from the epoch, so tests feed
// deterministic timestamps instead of real time.
FpsMeter::clock::time_point at(int ms) {
    return FpsMeter::clock::time_point(std::chrono::milliseconds(ms));
}
}  // namespace

TEST_CASE("FpsMeter reads 0 before it has two ticks") {
    FpsMeter m;
    REQUIRE(m.fps() == 0.0);
    m.tick(at(0));  // baseline only
    REQUIRE(m.fps() == 0.0);
}

TEST_CASE("FpsMeter seeds on the first interval") {
    FpsMeter m;
    m.tick(at(0));
    m.tick(at(100));  // 0.1 s → 10 fps
    REQUIRE(m.fps() == Approx(10.0));
}

TEST_CASE("FpsMeter converges to a steady rate") {
    FpsMeter m;
    for (int i = 0; i <= 30; ++i) {
        m.tick(at(i * 100));  // steady 10 fps
    }
    REQUIRE(m.fps() == Approx(10.0).epsilon(0.001));
}

TEST_CASE("FpsMeter smooths toward a changed rate (EMA, not instantaneous)") {
    FpsMeter m;
    m.tick(at(0));
    m.tick(at(100));  // seed at 10 fps
    m.tick(at(150));  // instantaneous 20 fps, but EMA stays between 10 and 20
    REQUIRE(m.fps() > 10.0);
    REQUIRE(m.fps() < 20.0);
}

TEST_CASE("FpsMeter ignores a zero or backward interval") {
    FpsMeter m;
    m.tick(at(0));
    m.tick(at(100));  // 10 fps
    const double before = m.fps();
    m.tick(at(100));  // same instant → dt = 0, ignored
    REQUIRE(m.fps() == before);
    m.tick(at(50));   // time went backward → ignored
    REQUIRE(m.fps() == before);
}

TEST_CASE("FpsMeter reset clears the rate and re-seeds cleanly") {
    FpsMeter m;
    m.tick(at(0));
    m.tick(at(100));
    REQUIRE(m.fps() > 0.0);
    m.reset();
    REQUIRE(m.fps() == 0.0);
    m.tick(at(1000));      // baseline again after reset
    REQUIRE(m.fps() == 0.0);
    m.tick(at(1200));      // 0.2 s → 5 fps, freshly seeded (no blend across gap)
    REQUIRE(m.fps() == Approx(5.0));
}
