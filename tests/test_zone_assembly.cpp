#include <catch2/catch_test_macros.hpp>

#include "ui/camera/grid/zone_assembly.h"

#include <opencv2/core.hpp>

using denso::ui::assemble_zone_value;
using denso::ui::group_into_zones;
using denso::ui::NamedDetection;
using denso::camera::CameraArea;
using denso::camera::Point;

static NamedDetection digit(int x, const char* name) {
    return NamedDetection{cv::Rect(x, 0, 10, 20), 0.9f, name};
}

TEST_CASE("assemble_zone_value reads digits left-to-right", "[zone_assembly]") {
    // Supplied out of order; x-center ordering must produce 500.
    std::vector<NamedDetection> d = {digit(40, "0"), digit(10, "5"), digit(25, "0")};
    auto v = assemble_zone_value(d);
    REQUIRE(v.has_value());
    CHECK(*v == 500);
}

TEST_CASE("assemble_zone_value collapses leading zeros", "[zone_assembly]") {
    std::vector<NamedDetection> d = {digit(10, "0"), digit(25, "5"), digit(40, "0")};
    CHECK(assemble_zone_value(d) == 50);
}

TEST_CASE("assemble_zone_value on empty or non-digit is nullopt", "[zone_assembly]") {
    CHECK_FALSE(assemble_zone_value({}).has_value());
    CHECK_FALSE(assemble_zone_value({digit(10, "x")}).has_value());
}

TEST_CASE("assemble_zone_value rejects four digits", "[zone_assembly]") {
    std::vector<NamedDetection> d = {
        digit(10, "5"),
        digit(25, "0"),
        digit(40, "0"),
        digit(55, "0"),
    };

    CHECK_FALSE(assemble_zone_value(d).has_value());
}

TEST_CASE("assemble_zone_value accepts the maximum supported value", "[zone_assembly]") {
    std::vector<NamedDetection> d = {
        digit(10, "9"),
        digit(25, "9"),
        digit(40, "9"),
    };

    CHECK(assemble_zone_value(d) == 999);
}

TEST_CASE("assemble_zone_value rejects many digits without throwing", "[zone_assembly]") {
    std::vector<NamedDetection> d;
    for (int i = 0; i < 10; ++i) {
        d.push_back(digit(10 + i * 15, "9"));
    }

    CHECK_NOTHROW(assemble_zone_value(d));
    CHECK_FALSE(assemble_zone_value(d).has_value());
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
    CHECK(zones[0].value == 12);
    CHECK(zones[1].zone_no == 2);
    CHECK(zones[1].value == 3);
}
