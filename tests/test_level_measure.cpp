// Phase A — the pure Ball Leveler measurement core: percentage mapping and
// detection selection.
//
// Pure `denso_core`: no widgets, no engine, no camera, no database, no OpenCV.
// Everything here is normalized oriented-frame geometry with Y increasing
// DOWNWARD, so the 100% line has the SMALLER y.
//
// Model/mode authorization is NOT retested here — it belongs to the central
// policy and is covered against the real chokepoint in test_level_persistence.cpp
// ("save_level_configuration rejects digitv3 in ball_leveler"). This file asserts
// only what the pure core owns.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "level/calibration.h"
#include "level/measure.h"

#include <cmath>
#include <limits>
#include <vector>

using denso::level::BallBox;
using denso::level::LevelCalibration;
using denso::level::level_percent;
using denso::level::select_ball;

namespace {

/// A calibration whose arithmetic is easy to check by eye: the rectangle spans
/// y 0.1..0.9, the 0% line sits at 0.8 and the 100% line at 0.2, so the span is
/// 0.6 and the midpoint 0.5 must read exactly 50%.
LevelCalibration good() {
    LevelCalibration c;
    c.rect_x = 0.10;
    c.rect_y = 0.10;
    c.rect_w = 0.80;
    c.rect_h = 0.80;
    c.y_100 = 0.20;
    c.y_0 = 0.80;
    c.conf = 0.50;
    c.hold_ms = 2000;
    return c;
}

/// A box centred at (cx, cy) with the given confidence.
BallBox at(double cx, double cy, double conf, double half = 0.02) {
    BallBox b;
    b.x1 = cx - half;
    b.x2 = cx + half;
    b.y1 = cy - half;
    b.y2 = cy + half;
    b.conf = conf;
    return b;
}

}  // namespace

// ─── percentage mapping ──────────────────────────────────────────────────────

TEST_CASE("the 0% line reads exactly 0%", "[level][measure]") {
    const auto p = level_percent(good(), 0.80);
    REQUIRE(p.has_value());
    CHECK_THAT(*p, Catch::Matchers::WithinAbs(0.0, 1e-9));
}

TEST_CASE("the midpoint reads exactly 50%", "[level][measure]") {
    const auto p = level_percent(good(), 0.50);
    REQUIRE(p.has_value());
    CHECK_THAT(*p, Catch::Matchers::WithinAbs(50.0, 1e-9));
}

TEST_CASE("the 100% line reads exactly 100%", "[level][measure]") {
    const auto p = level_percent(good(), 0.20);
    REQUIRE(p.has_value());
    CHECK_THAT(*p, Catch::Matchers::WithinAbs(100.0, 1e-9));
}

TEST_CASE("a quarter of the way up reads 25%", "[level][measure]") {
    // 0.65 is 0.15 above the 0% line, over a 0.6 span -> 25%.
    const auto p = level_percent(good(), 0.65);
    REQUIRE(p.has_value());
    CHECK_THAT(*p, Catch::Matchers::WithinAbs(25.0, 1e-9));
}

TEST_CASE("a ball below the 0% line clamps to 0%, never negative",
          "[level][measure][clamp]") {
    // Physically reachable: the ball can sit below the calibrated 0% line. The
    // raw value here would be -16.67%, which is meaningless to report.
    const auto p = level_percent(good(), 0.90);
    REQUIRE(p.has_value());
    CHECK(*p == 0.0);
}

TEST_CASE("a ball above the 100% line clamps to 100%, never over",
          "[level][measure][clamp]") {
    const auto p = level_percent(good(), 0.10);   // raw would be 116.67%
    REQUIRE(p.has_value());
    CHECK(*p == 100.0);
}

// ─── mapping refuses an unusable calibration ─────────────────────────────────

TEST_CASE("reversed reference lines are refused, not silently inverted",
          "[level][measure][invalid]") {
    LevelCalibration c = good();
    c.y_100 = 0.80;   // 100% below 0% — the operator dragged them the wrong way
    c.y_0 = 0.20;
    CHECK(denso::level::validate_calibration(c).reason_code == "calib_lines_reversed");
    // MUTATION GUARD: dividing by a negative span would produce a confident,
    // plausible, exactly-backwards reading — the worst possible failure here.
    CHECK_FALSE(level_percent(c, 0.50).has_value());
}

TEST_CASE("a near-zero span is refused rather than divided by",
          "[level][measure][invalid]") {
    LevelCalibration c = good();
    c.y_100 = 0.500;
    c.y_0 = 0.505;    // span 0.005, below kMinSpanNorm (0.02)
    CHECK(denso::level::validate_calibration(c).reason_code == "calib_span_too_small");
    CHECK_FALSE(level_percent(c, 0.50).has_value());
}

TEST_CASE("NaN and infinity are refused everywhere they can enter",
          "[level][measure][invalid]") {
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();

    SECTION("a non-finite calibration field") {
        LevelCalibration c = good();
        c.y_0 = nan;
        CHECK_FALSE(level_percent(c, 0.5).has_value());
        c = good();
        c.rect_h = inf;
        CHECK_FALSE(level_percent(c, 0.5).has_value());
    }
    SECTION("a non-finite measurement") {
        // A NaN centre would compare false against both clamp bounds and escape
        // as NaN, which would render as "nan%" on the operator's screen.
        CHECK_FALSE(level_percent(good(), nan).has_value());
        CHECK_FALSE(level_percent(good(), inf).has_value());
    }
    SECTION("a non-finite detection is dropped, not clamped") {
        const std::vector<BallBox> cands{at(0.5, nan, 0.9), at(0.5, 0.5, 0.6)};
        const auto s = select_ball(cands, good());
        REQUIRE(s.has_value());
        CHECK(s->index == 1);   // the healthy, LOWER-confidence one
    }
}

// ─── detection selection ─────────────────────────────────────────────────────

TEST_CASE("a detection whose centre is outside the rectangle is ignored",
          "[level][measure][select]") {
    // Rectangle spans x 0.10..0.90, y 0.10..0.90.
    SECTION("outside horizontally") {
        CHECK_FALSE(select_ball({at(0.95, 0.50, 0.99)}, good()).has_value());
    }
    SECTION("outside vertically") {
        CHECK_FALSE(select_ball({at(0.50, 0.95, 0.99)}, good()).has_value());
    }
    SECTION("a big box merely OVERLAPPING the rectangle does not count") {
        // Centre at (0.97, 0.97) but the box reaches well inside. Centre-inside is
        // what makes a stray large box unable to hijack the reading.
        BallBox b;
        b.x1 = 0.80; b.y1 = 0.80; b.x2 = 1.14; b.y2 = 1.14; b.conf = 0.99;
        CHECK_FALSE(select_ball({b}, good()).has_value());
    }
    SECTION("an inside detection IS selected") {
        const auto s = select_ball({at(0.50, 0.50, 0.99)}, good());
        REQUIRE(s.has_value());
        CHECK(s->index == 0);
    }
}

TEST_CASE("the highest-confidence valid detection is selected",
          "[level][measure][select]") {
    const std::vector<BallBox> cands{
        at(0.30, 0.70, 0.55),   // valid, low
        at(0.95, 0.50, 0.99),   // HIGHEST but outside the rectangle
        at(0.50, 0.40, 0.80),   // valid, highest of the valid ones
        at(0.60, 0.60, 0.20),   // below the confidence threshold
    };
    const auto s = select_ball(cands, good());
    REQUIRE(s.has_value());
    CHECK(s->index == 2);
    CHECK(s->box.conf == 0.80);

    // …and the reading follows from THAT box's centre: 0.40 -> 66.67%.
    const auto p = level_percent(good(), s->box.centre_y());
    REQUIRE(p.has_value());
    CHECK_THAT(*p, Catch::Matchers::WithinAbs(200.0 / 3.0, 1e-9));
}

TEST_CASE("a detection below the calibrated confidence is ignored",
          "[level][measure][select]") {
    LevelCalibration c = good();
    c.conf = 0.75;
    CHECK_FALSE(select_ball({at(0.5, 0.5, 0.74)}, c).has_value());
    // The threshold is inclusive — exactly meeting it qualifies.
    CHECK(select_ball({at(0.5, 0.5, 0.75)}, c).has_value());
}

TEST_CASE("equal confidence resolves to the earliest candidate, deterministically",
          "[level][measure][select]") {
    // Lean V1 uses a simple stable rule rather than an ordering hierarchy; what
    // matters is that the same input always yields the same output.
    const std::vector<BallBox> cands{at(0.30, 0.30, 0.90), at(0.70, 0.70, 0.90)};
    for (int i = 0; i < 3; ++i) {
        const auto s = select_ball(cands, good());
        REQUIRE(s.has_value());
        CHECK(s->index == 0);
    }
}

TEST_CASE("no candidates and no qualifying candidate are ordinary answers",
          "[level][measure][select]") {
    CHECK_FALSE(select_ball({}, good()).has_value());
    CHECK_FALSE(select_ball({at(0.5, 0.5, 0.10)}, good()).has_value());
}

TEST_CASE("selection refuses an invalid calibration outright",
          "[level][measure][select][invalid]") {
    LevelCalibration c = good();
    c.rect_w = 0.0;   // degenerate
    CHECK_FALSE(select_ball({at(0.5, 0.5, 0.99)}, c).has_value());
}
