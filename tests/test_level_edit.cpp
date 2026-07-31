// Phase A — the calibration EDITING state machine behind the wizard page.
// Pure denso_core: no widgets, no database. Y increases DOWNWARD, so the 100%
// line has the SMALLER y.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "level/calibration.h"
#include "level/edit.h"

#include <cmath>
#include <limits>

using denso::level::CalibrationDraft;
using denso::level::LevelCalibration;
using denso::level::kDefaultConf;
using denso::level::kDefaultHoldMs;
using denso::level::kMinSpanNorm;

TEST_CASE("a fresh draft is seeded with the shared defaults and no rectangle",
          "[level][edit]") {
    CalibrationDraft d;
    CHECK_FALSE(d.has_rect());
    CHECK_FALSE(d.ready());
    // MUTATION GUARD: seeding from local literals instead of the shared constants
    // would let the page and the persistence layer drift apart.
    CHECK(d.draft().conf == kDefaultConf);
    CHECK(d.draft().hold_ms == kDefaultHoldMs);
}

TEST_CASE("drawing the first rectangle seeds a legal, ready calibration",
          "[level][edit]") {
    CalibrationDraft d;
    d.set_rect(0.1, 0.1, 0.8, 0.8);
    REQUIRE(d.has_rect());
    CHECK(d.ready());
    CHECK(d.check().ok);
    // Lines land at thirds, ordered, inside the band.
    CHECK(d.draft().y_100 < d.draft().y_0);
    CHECK(d.draft().y_100 >= d.draft().rect_y);
    CHECK(d.draft().y_0 <= d.draft().rect_y + d.draft().rect_h);
}

TEST_CASE("both reference lines are constrained inside the rectangle",
          "[level][edit][constrain]") {
    CalibrationDraft d;
    d.set_rect(0.20, 0.30, 0.60, 0.40);   // band y 0.30 .. 0.70
    const double top = 0.30, bottom = 0.70;

    SECTION("dragging 100% above the top edge stops at the edge") {
        d.set_y_100(-5.0);
        CHECK_THAT(d.draft().y_100, Catch::Matchers::WithinAbs(top, 1e-9));
        CHECK(d.check().ok);
    }
    SECTION("dragging 0% below the bottom edge stops at the edge") {
        d.set_y_0(5.0);
        CHECK_THAT(d.draft().y_0, Catch::Matchers::WithinAbs(bottom, 1e-9));
        CHECK(d.check().ok);
    }
    SECTION("both stay inside after any drag") {
        for (double y : {-1.0, 0.0, 0.31, 0.5, 0.69, 1.0, 4.0}) {
            d.set_y_100(y);
            d.set_y_0(y);
            CHECK(d.draft().y_100 >= top - 1e-9);
            CHECK(d.draft().y_0 <= bottom + 1e-9);
        }
    }
}

TEST_CASE("100% is always kept above 0%, whichever line is dragged past the other",
          "[level][edit][constrain]") {
    CalibrationDraft d;
    d.set_rect(0.1, 0.1, 0.8, 0.8);

    SECTION("dragging 100% down past 0% pushes 0% ahead of it") {
        d.set_y_100(0.85);
        // MUTATION GUARD: allowing y_100 >= y_0 would store an inverted mapping
        // that reads confidently backwards.
        CHECK(d.draft().y_100 < d.draft().y_0);
        CHECK(d.draft().y_0 - d.draft().y_100 >= kMinSpanNorm - 1e-9);
        CHECK(d.check().ok);
    }
    SECTION("dragging 0% up past 100% pushes 100% ahead of it") {
        d.set_y_0(0.12);
        CHECK(d.draft().y_100 < d.draft().y_0);
        CHECK(d.draft().y_0 - d.draft().y_100 >= kMinSpanNorm - 1e-9);
        CHECK(d.check().ok);
    }
    SECTION("the minimum span is never violated by nudging them together") {
        d.set_y_100(0.50);
        d.set_y_0(0.50 + kMinSpanNorm / 4.0);
        CHECK(d.draft().y_0 - d.draft().y_100 >= kMinSpanNorm - 1e-9);
        CHECK(d.check().ok);
    }
}

TEST_CASE("resizing the rectangle carries the lines with it instead of breaking them",
          "[level][edit]") {
    CalibrationDraft d;
    d.set_rect(0.0, 0.0, 1.0, 0.9);
    d.set_y_100(0.30);
    d.set_y_0(0.60);

    d.set_rect(0.1, 0.4, 0.5, 0.5);   // move + shrink the band to 0.40 .. 0.90
    CHECK(d.check().ok);
    CHECK(d.draft().y_100 >= 0.40 - 1e-9);
    CHECK(d.draft().y_0 <= 0.90 + 1e-9);
    CHECK(d.draft().y_100 < d.draft().y_0);
}

TEST_CASE("a non-finite or degenerate edit is ignored, never applied",
          "[level][edit][invalid]") {
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    CalibrationDraft d;
    d.set_rect(0.1, 0.1, 0.8, 0.8);
    const LevelCalibration before = d.draft();

    d.set_rect(nan, 0.1, 0.8, 0.8);
    d.set_rect(0.1, 0.1, inf, 0.8);
    d.set_rect(0.1, 0.1, 0.0, 0.8);    // degenerate width
    d.set_rect(0.1, 0.1, 0.8, -0.5);   // negative height
    d.set_y_100(nan);
    d.set_y_0(inf);
    d.set_conf(nan);

    CHECK(d.draft().rect_x == before.rect_x);
    CHECK(d.draft().rect_w == before.rect_w);
    CHECK(d.draft().y_100 == before.y_100);
    CHECK(d.draft().y_0 == before.y_0);
    CHECK(d.draft().conf == before.conf);
    CHECK(d.check().ok);
}

TEST_CASE("reloading a saved calibration is lossless and editing is opt-in",
          "[level][edit][reload]") {
    // Requirement 10: reload and allow editing WITHOUT changing values. Merely
    // opening the page must not nudge a stored calibration.
    LevelCalibration saved;
    saved.rect_x = 0.1234;
    saved.rect_y = 0.2345;
    saved.rect_w = 0.5;
    saved.rect_h = 0.5;
    saved.y_100 = 0.30;
    saved.y_0 = 0.70;
    saved.conf = 0.66;
    saved.hold_ms = 1500;
    REQUIRE(denso::level::validate_calibration(saved).ok);

    CalibrationDraft d = CalibrationDraft::from_calibration(saved);
    REQUIRE(d.has_rect());
    CHECK(d.ready());
    // MUTATION GUARD: routing the load through the clamping mutators would nudge
    // these values, so a round-trip would rewrite the operator's calibration.
    CHECK(d.draft().rect_x == saved.rect_x);
    CHECK(d.draft().rect_y == saved.rect_y);
    CHECK(d.draft().rect_w == saved.rect_w);
    CHECK(d.draft().rect_h == saved.rect_h);
    CHECK(d.draft().y_100 == saved.y_100);
    CHECK(d.draft().y_0 == saved.y_0);
    CHECK(d.draft().conf == saved.conf);
    CHECK(d.draft().hold_ms == saved.hold_ms);

    // …and it is then editable, still under every constraint.
    d.set_y_0(0.95);
    CHECK(d.draft().y_0 <= saved.rect_y + saved.rect_h + 1e-9);
    CHECK(d.check().ok);
}

TEST_CASE("the draft's verdict is the SAME validator the write chokepoint uses",
          "[level][edit]") {
    CalibrationDraft d;
    d.set_rect(0.1, 0.1, 0.8, 0.8);
    d.set_conf(1.5);   // deliberately not clamped — the operator must see it named
    CHECK_FALSE(d.ready());
    CHECK(d.check().reason_code == "calib_conf_out_of_range");
    CHECK(d.check().reason_code ==
          denso::level::validate_calibration(d.draft()).reason_code);

    d.set_conf(0.4);
    CHECK(d.ready());
}

// ─────────────────────────────────────────────────────────────────────────────
// The pushed line must leave a span the VALIDATOR accepts, not merely a span the
// arithmetic looks like it left.
//
// `y_0 - kMinSpanNorm` does not reliably satisfy `y_0 - y_100 >= kMinSpanNorm` in
// binary floating point: 0.15 - 0.02 == 0.13, but 0.15 - 0.13 == 0.019999999…,
// which is BELOW kMinSpanNorm. check() measures the span exactly that way and the
// wizard's Save button is gated on check(), so the difference is an ordinary,
// legal drag that silently leaves Save greyed out with the reason "the lines are
// too close" — when the operator just put them exactly far enough apart.
//
// Swept rather than spot-checked: which values misbehave depends on where the
// operands fall in the binary grid, so a single example proves almost nothing.
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("a pushed 0% line leaves a span the validator accepts", "[level][edit]") {
    CalibrationDraft d;
    d.set_rect(0.2, 0.0, 0.6, 1.0);   // a full-height band, so only the push acts
    for (int i = 1; i <= 199; ++i) {
        const double y = static_cast<double>(i) / 200.0;
        d.set_y_100(0.999);           // force the push on every iteration
        d.set_y_0(y);
        INFO("y_0 = " << y << " y_100 = " << d.draft().y_100
                      << " span = " << (d.draft().y_0 - d.draft().y_100));
        CHECK(d.draft().y_0 - d.draft().y_100 >= kMinSpanNorm);
        CHECK(d.check().ok);
    }
}

TEST_CASE("a pushed 100% line leaves a span the validator accepts", "[level][edit]") {
    CalibrationDraft d;
    d.set_rect(0.2, 0.0, 0.6, 1.0);
    for (int i = 1; i <= 199; ++i) {
        const double y = static_cast<double>(i) / 200.0;
        d.set_y_0(0.001);             // force the push on every iteration
        d.set_y_100(y);
        INFO("y_100 = " << y << " y_0 = " << d.draft().y_0
                        << " span = " << (d.draft().y_0 - d.draft().y_100));
        CHECK(d.draft().y_0 - d.draft().y_100 >= kMinSpanNorm);
        CHECK(d.check().ok);
    }
}

TEST_CASE("re-seating after a rectangle move leaves an acceptable span",
          "[level][edit]") {
    // The third site that separates the lines: a rectangle whose new band is too
    // short for the lines' previous relative positions.
    for (int i = 1; i <= 90; ++i) {
        CalibrationDraft d;
        d.set_rect(0.1, 0.0, 0.8, 1.0);
        d.set_y_100(0.40);
        d.set_y_0(0.42);
        const double top = static_cast<double>(i) / 100.0;
        d.set_rect(0.1, top, 0.8, 0.05);   // a short band the lines must fit into
        INFO("top = " << top << " span = " << (d.draft().y_0 - d.draft().y_100));
        CHECK(d.draft().y_0 - d.draft().y_100 >= kMinSpanNorm);
        CHECK(d.draft().y_100 >= d.draft().rect_y);
        CHECK(d.draft().y_0 <= d.draft().rect_y + d.draft().rect_h);
        CHECK(d.check().ok);
    }
}
