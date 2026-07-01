#include <catch2/catch_test_macros.hpp>

#include "camera/area_geometry.h"
#include "camera/camera.h"

using denso::camera::CameraArea;
using denso::camera::inside_any_area;
using denso::camera::Point;
using denso::camera::point_in_polygon;

namespace {
// Axis-aligned square (0.2,0.2)-(0.8,0.8), CCW.
const std::vector<Point> kSquare = {
    {0.2f, 0.2f}, {0.8f, 0.2f}, {0.8f, 0.8f}, {0.2f, 0.8f}};
}  // namespace

TEST_CASE("point_in_polygon: a point inside a square is inside") {
    REQUIRE(point_in_polygon(kSquare, {0.5f, 0.5f}));
}

TEST_CASE("point_in_polygon: points outside a square are outside") {
    REQUIRE_FALSE(point_in_polygon(kSquare, {0.1f, 0.5f}));  // left
    REQUIRE_FALSE(point_in_polygon(kSquare, {0.9f, 0.5f}));  // right
    REQUIRE_FALSE(point_in_polygon(kSquare, {0.5f, 0.1f}));  // above
    REQUIRE_FALSE(point_in_polygon(kSquare, {0.5f, 0.9f}));  // below
}

TEST_CASE("point_in_polygon: a triangle contains and excludes correctly") {
    const std::vector<Point> tri = {{0.2f, 0.8f}, {0.8f, 0.8f}, {0.5f, 0.2f}};
    REQUIRE(point_in_polygon(tri, {0.5f, 0.6f}));        // interior
    REQUIRE_FALSE(point_in_polygon(tri, {0.25f, 0.3f}));  // above a slanted edge
    REQUIRE_FALSE(point_in_polygon(tri, {0.5f, 0.9f}));   // below the base
}

TEST_CASE("point_in_polygon: a concave (arrow) polygon respects the notch") {
    // A downward chevron: the notch at the top-center is OUTSIDE the polygon.
    const std::vector<Point> chevron = {
        {0.1f, 0.2f}, {0.5f, 0.5f}, {0.9f, 0.2f}, {0.9f, 0.8f}, {0.1f, 0.8f}};
    REQUIRE(point_in_polygon(chevron, {0.5f, 0.7f}));        // solid lower body
    REQUIRE_FALSE(point_in_polygon(chevron, {0.5f, 0.25f}));  // inside the notch
}

TEST_CASE("point_in_polygon: fewer than 3 vertices contains nothing") {
    REQUIRE_FALSE(point_in_polygon({}, {0.5f, 0.5f}));
    REQUIRE_FALSE(point_in_polygon({{0.5f, 0.5f}}, {0.5f, 0.5f}));
    REQUIRE_FALSE(
        point_in_polygon({{0.2f, 0.2f}, {0.8f, 0.8f}}, {0.5f, 0.5f}));
}

TEST_CASE("inside_any_area: false when there are no areas") {
    REQUIRE_FALSE(inside_any_area({}, {0.5f, 0.5f}));
}

TEST_CASE("inside_any_area: true when the point is in any one area") {
    CameraArea left;
    left.points = {{0.0f, 0.0f}, {0.3f, 0.0f}, {0.3f, 1.0f}, {0.0f, 1.0f}};
    CameraArea right;
    right.points = {{0.7f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}, {0.7f, 1.0f}};
    const std::vector<CameraArea> areas = {left, right};

    REQUIRE(inside_any_area(areas, {0.15f, 0.5f}));   // in left
    REQUIRE(inside_any_area(areas, {0.85f, 0.5f}));   // in right
    REQUIRE_FALSE(inside_any_area(areas, {0.5f, 0.5f}));  // in the gap
}
