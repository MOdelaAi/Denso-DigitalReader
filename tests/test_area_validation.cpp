#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "camera/area_validation.h"
#include "camera/camera.h"

using denso::camera::areas_equal;
using denso::camera::CameraArea;
using denso::camera::find_zone_conflict;
using denso::camera::Point;
using denso::camera::polygon_area;
using denso::camera::polygon_is_degenerate;
using denso::camera::ZoneConflict;

namespace {

CameraArea named_area(std::string name, std::optional<int> zone) {
    CameraArea a;
    a.name = std::move(name);
    a.zone = zone;
    a.points = {{0.2f, 0.2f}, {0.8f, 0.2f}, {0.8f, 0.8f}, {0.2f, 0.8f}};
    return a;
}

}  // namespace

// ─── polygon_area ────────────────────────────────────────────────────────────

TEST_CASE("polygon_area: a unit square covers the whole frame") {
    const std::vector<Point> square = {
        {0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 1.0f}};
    REQUIRE(polygon_area(square) == Catch::Approx(1.0).epsilon(1e-6));
}

TEST_CASE("polygon_area: winding direction does not change the magnitude") {
    const std::vector<Point> ccw = {
        {0.2f, 0.2f}, {0.8f, 0.2f}, {0.8f, 0.8f}, {0.2f, 0.8f}};
    const std::vector<Point> cw = {
        {0.2f, 0.8f}, {0.8f, 0.8f}, {0.8f, 0.2f}, {0.2f, 0.2f}};
    REQUIRE(polygon_area(ccw) == Catch::Approx(polygon_area(cw)));
    REQUIRE(polygon_area(ccw) == Catch::Approx(0.36).epsilon(1e-5));
}

TEST_CASE("polygon_area: fewer than 3 vertices enclose nothing") {
    REQUIRE(polygon_area({}) == Catch::Approx(0.0));
    REQUIRE(polygon_area({{0.5f, 0.5f}}) == Catch::Approx(0.0));
    REQUIRE(polygon_area({{0.2f, 0.2f}, {0.8f, 0.8f}}) == Catch::Approx(0.0));
}

// ─── polygon_is_degenerate ───────────────────────────────────────────────────

TEST_CASE("polygon_is_degenerate: a normal square is fine") {
    REQUIRE_FALSE(polygon_is_degenerate(
        {{0.2f, 0.2f}, {0.8f, 0.2f}, {0.8f, 0.8f}, {0.2f, 0.8f}}));
}

TEST_CASE("polygon_is_degenerate: fewer than 3 vertices is degenerate") {
    REQUIRE(polygon_is_degenerate({}));
    REQUIRE(polygon_is_degenerate({{0.5f, 0.5f}}));
    REQUIRE(polygon_is_degenerate({{0.2f, 0.2f}, {0.8f, 0.8f}}));
}

TEST_CASE("polygon_is_degenerate: three collinear points enclose no area") {
    REQUIRE(polygon_is_degenerate({{0.1f, 0.5f}, {0.5f, 0.5f}, {0.9f, 0.5f}}));
}

TEST_CASE("polygon_is_degenerate: identical points collapse to nothing") {
    REQUIRE(polygon_is_degenerate({{0.5f, 0.5f}, {0.5f, 0.5f}, {0.5f, 0.5f}}));
}

TEST_CASE("polygon_is_degenerate: a sliver below the minimum area is rejected") {
    // ~0.4 wide but 1e-5 tall → area 4e-6, far under the 1e-4 floor. This is
    // what repeated clamped clicks in the letterbox bars used to produce.
    REQUIRE(polygon_is_degenerate(
        {{0.3f, 0.5f}, {0.7f, 0.5f}, {0.7f, 0.50001f}, {0.3f, 0.50001f}}));
}

TEST_CASE("polygon_is_degenerate: a small but usable area is accepted") {
    // 3% x 3% of frame = 9e-4, comfortably over the floor — a plausible ROI
    // around a small 7-segment display on a wide shot.
    REQUIRE_FALSE(polygon_is_degenerate(
        {{0.50f, 0.50f}, {0.53f, 0.50f}, {0.53f, 0.53f}, {0.50f, 0.53f}}));
}

// ─── find_zone_conflict ──────────────────────────────────────────────────────

TEST_CASE("find_zone_conflict: a clean set has no conflict") {
    const std::vector<CameraArea> areas = {named_area("Left", 1),
                                           named_area("Right", 2)};
    REQUIRE_FALSE(find_zone_conflict(areas, {}).has_value());
}

TEST_CASE("find_zone_conflict: unreported areas never conflict with each other") {
    // nullopt = ROI-only. Any number of them may coexist.
    const std::vector<CameraArea> areas = {named_area("A", std::nullopt),
                                           named_area("B", std::nullopt),
                                           named_area("C", std::nullopt)};
    REQUIRE_FALSE(find_zone_conflict(areas, {}).has_value());
}

TEST_CASE("find_zone_conflict: the same zone twice in one camera conflicts") {
    const std::vector<CameraArea> areas = {named_area("Left", 3),
                                           named_area("Right", 3)};
    const auto c = find_zone_conflict(areas, {});
    REQUIRE(c.has_value());
    REQUIRE(c->zone == 3);
    REQUIRE(c->area_name == "Right");  // the second one is the offender
    REQUIRE(c->owner.empty());         // empty owner = duplicated within this camera
}

TEST_CASE("find_zone_conflict: a zone owned by another camera conflicts") {
    const std::vector<CameraArea> areas = {named_area("Left", 4)};
    const auto c = find_zone_conflict(areas, {{4, "Line 2 Cam"}});
    REQUIRE(c.has_value());
    REQUIRE(c->zone == 4);
    REQUIRE(c->area_name == "Left");
    REQUIRE(c->owner == "Line 2 Cam");
}

TEST_CASE("find_zone_conflict: an unrelated zone elsewhere is not a conflict") {
    const std::vector<CameraArea> areas = {named_area("Left", 4)};
    REQUIRE_FALSE(find_zone_conflict(areas, {{7, "Line 2 Cam"}}).has_value());
}

TEST_CASE("find_zone_conflict: reports the first conflict in area order") {
    const std::vector<CameraArea> areas = {
        named_area("A", 1), named_area("B", 5), named_area("C", 5)};
    const auto c = find_zone_conflict(areas, {{1, "Other"}});
    REQUIRE(c.has_value());
    REQUIRE(c->zone == 1);  // A's cross-camera clash is hit before B/C's duplicate
}

// ─── areas_equal (dirty-state detection) ─────────────────────────────────────

TEST_CASE("areas_equal: an unchanged set is equal to itself") {
    const std::vector<CameraArea> a = {named_area("Left", 1),
                                       named_area("Right", 2)};
    REQUIRE(areas_equal(a, a));
}

TEST_CASE("areas_equal: an empty set equals an empty set") {
    REQUIRE(areas_equal({}, {}));
}

TEST_CASE("areas_equal: a renamed area is a change") {
    const std::vector<CameraArea> a = {named_area("Left", 1)};
    const std::vector<CameraArea> b = {named_area("Left side", 1)};
    REQUIRE_FALSE(areas_equal(a, b));
}

TEST_CASE("areas_equal: a re-zoned area is a change") {
    const std::vector<CameraArea> a = {named_area("Left", 1)};
    const std::vector<CameraArea> b = {named_area("Left", 2)};
    REQUIRE_FALSE(areas_equal(a, b));
}

TEST_CASE("areas_equal: clearing a zone is a change") {
    const std::vector<CameraArea> a = {named_area("Left", 1)};
    const std::vector<CameraArea> b = {named_area("Left", std::nullopt)};
    REQUIRE_FALSE(areas_equal(a, b));
}

TEST_CASE("areas_equal: an added or removed area is a change") {
    const std::vector<CameraArea> one = {named_area("Left", 1)};
    const std::vector<CameraArea> two = {named_area("Left", 1),
                                         named_area("Right", 2)};
    REQUIRE_FALSE(areas_equal(one, two));
    REQUIRE_FALSE(areas_equal(two, one));
}

TEST_CASE("areas_equal: a moved vertex is a change") {
    std::vector<CameraArea> a = {named_area("Left", 1)};
    std::vector<CameraArea> b = a;
    b[0].points[2].x = 0.75f;  // dragged one corner
    REQUIRE_FALSE(areas_equal(a, b));
}

TEST_CASE("areas_equal: an added vertex is a change") {
    std::vector<CameraArea> a = {named_area("Left", 1)};
    std::vector<CameraArea> b = a;
    b[0].points.push_back({0.5f, 0.9f});
    REQUIRE_FALSE(areas_equal(a, b));
}

TEST_CASE("areas_equal: the db-assigned id is not part of the comparison") {
    // load() hands back rows carrying ids; the working copy of a redrawn area
    // may not. Ids must not make an otherwise-identical set look dirty.
    std::vector<CameraArea> a = {named_area("Left", 1)};
    std::vector<CameraArea> b = a;
    a[0].id = 17;
    a[0].camera_id = 3;
    REQUIRE(areas_equal(a, b));
}
