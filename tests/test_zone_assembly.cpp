#include <catch2/catch_test_macros.hpp>

#include "camera/zone_assembly.h"

#include <opencv2/core.hpp>

using denso::ui::assemble_zone_value;
using denso::ui::group_into_zones;
using denso::ui::NamedDetection;
using denso::ui::ReadingKind;
using denso::camera::CameraArea;
using denso::camera::Point;

static NamedDetection digit(int x, const char* name) {
    return NamedDetection{cv::Rect(x, 0, 10, 20), 0.9f, name};
}

TEST_CASE("assemble_zone_value reads digits left-to-right", "[zone_assembly]") {
    // Supplied out of order; x-center ordering must produce 500.
    std::vector<NamedDetection> d = {digit(40, "0"), digit(10, "5"), digit(25, "0")};
    auto r = assemble_zone_value(d);
    REQUIRE(r.kind == ReadingKind::Complete);
    CHECK(r.value == 500);
}

TEST_CASE("assemble_zone_value collapses leading zeros", "[zone_assembly]") {
    std::vector<NamedDetection> d = {digit(10, "0"), digit(25, "5"), digit(40, "0")};
    auto r = assemble_zone_value(d);
    REQUIRE(r.kind == ReadingKind::Complete);
    CHECK(r.value == 50);
}

TEST_CASE("assemble_zone_value on empty or non-digit is not Complete", "[zone_assembly]") {
    CHECK(assemble_zone_value({}).kind == ReadingKind::NoDigits);
    const auto r = assemble_zone_value({digit(10, "x")});
    CHECK(r.kind == ReadingKind::Incomplete);
    CHECK(r.value == 0);  // an unparseable label must never carry a usable value
}

TEST_CASE("assemble_zone_value rejects four digits", "[zone_assembly]") {
    std::vector<NamedDetection> d = {
        digit(10, "5"),
        digit(25, "0"),
        digit(40, "0"),
        digit(55, "0"),
    };

    const auto r = assemble_zone_value(d);
    CHECK(r.kind == ReadingKind::Incomplete);
    CHECK(r.value == 0);  // never a truncated/bogus integer
}

TEST_CASE("assemble_zone_value accepts the maximum supported value", "[zone_assembly]") {
    std::vector<NamedDetection> d = {
        digit(10, "9"),
        digit(25, "9"),
        digit(40, "9"),
    };

    auto r = assemble_zone_value(d);
    REQUIRE(r.kind == ReadingKind::Complete);
    CHECK(r.value == 999);
}

TEST_CASE("assemble_zone_value rejects many digits without throwing", "[zone_assembly]") {
    std::vector<NamedDetection> d;
    for (int i = 0; i < 10; ++i) {
        d.push_back(digit(10 + i * 15, "9"));
    }

    CHECK_NOTHROW(assemble_zone_value(d));
    CHECK(assemble_zone_value(d).kind == ReadingKind::Incomplete);
}

TEST_CASE("group_into_zones assigns digits to their area and skips zoneless", "[zone_assembly]") {
    // Two rectangular zones side by side in a 100x100 frame (normalized).
    CameraArea left;
    left.zone = 1;
    left.points = {{0.0f, 0.0f}, {0.5f, 0.0f}, {0.5f, 1.0f}, {0.0f, 1.0f}};
    CameraArea right;
    right.zone = 2;
    right.points = {{0.5f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}, {0.5f, 1.0f}};
    CameraArea roi_only;  // zone == nullopt → excluded
    roi_only.points = left.points;

    // digits at x≈15,30 land in the left zone → "12"; x≈70 lands in right → "3".
    std::vector<NamedDetection> kept = {digit(10, "1"), digit(25, "2"), digit(65, "3")};
    auto zones = group_into_zones(kept, {left, right, roi_only}, 100.0f, 100.0f);

    REQUIRE(zones.size() == 2);
    CHECK(zones[0].zone_no == 1);
    CHECK(zones[0].kind == ReadingKind::Complete);
    CHECK(zones[0].value == 12);
    CHECK(zones[1].zone_no == 2);
    CHECK(zones[1].kind == ReadingKind::Complete);
    CHECK(zones[1].value == 3);
}

// Build a digit detection: name, box at (x,y) sized w*h.
static NamedDetection dg(const char* name, int x, int y, int w, int h) {
    NamedDetection d;
    d.name = name;
    d.box = cv::Rect(x, y, w, h);
    d.conf = 0.9f;
    return d;
}

TEST_CASE("assemble: three evenly spaced digits are Complete", "[zone_assembly]") {
    // height 40 -> pitch 28; centres 28 apart = exactly 1.0 pitch.
    const auto r = assemble_zone_value({dg("1", 0, 0, 20, 40),
                                        dg("2", 28, 0, 20, 40),
                                        dg("3", 56, 0, 20, 40)});
    REQUIRE(r.kind == ReadingKind::Complete);
    REQUIRE(r.value == 123);
}

TEST_CASE("assemble: an internal missing digit is Incomplete", "[zone_assembly]") {
    // "1_3": centres 56 apart = 2.0 pitch > 1.6 threshold.
    const auto r = assemble_zone_value({dg("1", 0, 0, 20, 40),
                                        dg("3", 56, 0, 20, 40)});
    REQUIRE(r.kind == ReadingKind::Incomplete);
}

TEST_CASE("assemble: a separation just under the gap threshold stays Complete",
          "[zone_assembly]") {
    // height 40 -> max_gap = kGapFactor(1.60) * kPitchPerHeight(0.70) * 40 = 44.8.
    // A centre separation of 44 is unambiguously below 44.8 in float, so the
    // strict '>' comparison must NOT flag a gap.
    //
    // NOTE: with integer box coordinates the threshold (44.8) is not itself a
    // representable separation, so these two tests (44 vs 46) cannot land exactly
    // on it and therefore cannot distinguish a strict '>' comparison from an
    // inclusive '>='. That distinction is deliberately left untested rather than
    // faked with a fragile float-equality test.
    const auto r = assemble_zone_value({dg("1", 0, 0, 20, 40),
                                        dg("2", 44, 0, 20, 40)});
    REQUIRE(r.kind == ReadingKind::Complete);
    REQUIRE(r.value == 12);
}

TEST_CASE("assemble: a separation just over the gap threshold is Incomplete",
          "[zone_assembly]") {
    // height 40 -> max_gap = kGapFactor(1.60) * kPitchPerHeight(0.70) * 40 = 44.8.
    // A centre separation of 46 is unambiguously above 44.8 in float, so the
    // strict '>' comparison must flag a gap.
    //
    // NOTE: see the sibling test above -- the strict-vs-inclusive boundary at
    // exactly 44.8 is unreachable with integer geometry and is deliberately
    // untested here for the same reason.
    const auto r = assemble_zone_value({dg("1", 0, 0, 20, 40),
                                        dg("2", 46, 0, 20, 40)});
    REQUIRE(r.kind == ReadingKind::Incomplete);
    REQUIRE(r.value == 0);
}

TEST_CASE("assemble: a non-positive median box height does not false-flag a gap",
          "[zone_assembly]") {
    // Degenerate zero-height boxes from the detector must not make max_gap collapse
    // to 0 (which would flag virtually any positive separation as a gap and, via
    // the later inhibition escalation, permanently freeze an otherwise-healthy
    // zone). The project's stated bias is to UNDER-detect gaps, so a non-positive
    // median height must skip the gap check entirely rather than reject.
    const auto r = assemble_zone_value({dg("1", 0, 0, 20, 0),
                                        dg("2", 1000, 0, 20, 0)});
    REQUIRE(r.kind == ReadingKind::Complete);
    REQUIRE(r.value == 12);
}

TEST_CASE("assemble: no detections is NoDigits", "[zone_assembly]") {
    REQUIRE(assemble_zone_value({}).kind == ReadingKind::NoDigits);
}

TEST_CASE("assemble: an incomplete reading carries NO usable value", "[zone_assembly]") {
    const auto r = assemble_zone_value({dg("1", 0, 0, 20, 40),
                                        dg("3", 56, 0, 20, 40)});
    REQUIRE(r.kind == ReadingKind::Incomplete);
    REQUIRE(r.value == 0);  // never 13 — spec §5.1
}

// ─────────────────────────────────────────────────────────────────────────────
// KNOWN LIMITATIONS — spec §5.2 / §10. These are NOT correctness expectations.
// They assert the guard does NOT fire, pinning residuals we have explicitly
// accepted for this slice so a future contributor cannot assume they are solved.
// Slice (b2)'s calibrated anchor/slot work is what closes them; when it lands,
// these cases flip to Incomplete and these tests SHOULD be rewritten.
// Tagged [known_limit] so they can be listed on demand.
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("KNOWN LIMITATION: a missing LEADING digit is undetectable",
          "[zone_assembly][known_limit]") {
    const auto r = assemble_zone_value({dg("2", 28, 0, 20, 40),
                                        dg("3", 56, 0, 20, 40)});
    REQUIRE(r.kind == ReadingKind::Complete);
    REQUIRE(r.value == 23);
}

TEST_CASE("KNOWN LIMITATION: a missing TRAILING digit is undetectable",
          "[zone_assembly][known_limit]") {
    const auto r = assemble_zone_value({dg("1", 0, 0, 20, 40),
                                        dg("2", 28, 0, 20, 40)});
    REQUIRE(r.kind == ReadingKind::Complete);
    REQUIRE(r.value == 12);
}

TEST_CASE("KNOWN LIMITATION: a single remaining detection is undetectable",
          "[zone_assembly][known_limit]") {
    const auto r = assemble_zone_value({dg("3", 56, 0, 20, 40)});
    REQUIRE(r.kind == ReadingKind::Complete);
    REQUIRE(r.value == 3);
}

TEST_CASE("assemble: more than three digits is Incomplete, not a bogus value",
          "[zone_assembly]") {
    const auto r = assemble_zone_value({dg("1", 0, 0, 20, 40), dg("2", 28, 0, 20, 40),
                                        dg("3", 56, 0, 20, 40), dg("4", 84, 0, 20, 40)});
    REQUIRE(r.kind == ReadingKind::Incomplete);
    REQUIRE(r.value == 0);
}

TEST_CASE("group_into_zones emits NoDigits for a zoned area with no detections",
          "[zone_assembly]") {
    CameraArea a;
    a.zone = 7;
    a.points = {{0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 1.0f}};
    const auto out = group_into_zones({}, {a}, 640.0f, 480.0f);
    REQUIRE(out.size() == 1);
    REQUIRE(out[0].zone_no == 7);
    REQUIRE(out[0].kind == ReadingKind::NoDigits);
    REQUIRE(out[0].value == 0);
    REQUIRE(out[0].conf == 0.0f);
}

TEST_CASE("group_into_zones does not reconstruct a value for a gapped zone",
          "[zone_assembly]") {
    // A zone with a gapped pair (internal missing digit) must survive grouping as
    // Incomplete + value 0 — group_into_zones must not derive any value of its own.
    CameraArea a;
    a.zone = 3;
    a.points = {{0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 1.0f}};
    std::vector<NamedDetection> kept = {dg("1", 0, 0, 20, 40), dg("3", 560, 0, 20, 40)};
    const auto out = group_into_zones(kept, {a}, 640.0f, 480.0f);
    REQUIRE(out.size() == 1);
    REQUIRE(out[0].zone_no == 3);
    REQUIRE(out[0].kind == ReadingKind::Incomplete);
    REQUIRE(out[0].value == 0);
}
