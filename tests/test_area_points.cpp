#include <catch2/catch_test_macros.hpp>

#include "camera/area_points.h"
#include "camera/model.h"

using denso::camera::parse_points;
using denso::camera::Point;
using denso::camera::serialize_points;

TEST_CASE("serialize_points joins normalized vertices as 'x,y;x,y;...'") {
    const std::vector<Point> pts = {{0.25f, 0.5f}, {0.75f, 0.5f}, {0.5f, 0.9f}};
    REQUIRE(serialize_points(pts) == "0.25,0.5;0.75,0.5;0.5,0.9");
}

TEST_CASE("serialize_points of an empty polygon is the empty string") {
    REQUIRE(serialize_points({}).empty());
}

TEST_CASE("parse_points round-trips a triangle") {
    const std::vector<Point> pts = {{0.2f, 0.3f}, {0.6f, 0.3f}, {0.4f, 0.7f}};
    const auto out = parse_points(serialize_points(pts));
    REQUIRE(out.size() == 3);
    REQUIRE(out[0].x == 0.2f);
    REQUIRE(out[0].y == 0.3f);
    REQUIRE(out[2].x == 0.4f);
    REQUIRE(out[2].y == 0.7f);
}

TEST_CASE("parse_points round-trips a rectangle (4 vertices)") {
    const std::vector<Point> pts = {
        {0.2f, 0.2f}, {0.8f, 0.2f}, {0.8f, 0.6f}, {0.2f, 0.6f}};
    const auto out = parse_points(serialize_points(pts));
    REQUIRE(out.size() == 4);
    REQUIRE(out[1].x == 0.8f);
    REQUIRE(out[3].y == 0.6f);
}

TEST_CASE("parse_points of an empty or blank string yields no vertices") {
    REQUIRE(parse_points("").empty());
    REQUIRE(parse_points("   ").empty());
}

TEST_CASE("parse_points skips malformed pairs instead of throwing") {
    // A stray token with no comma, and a pair with a non-numeric coord, are
    // dropped; the well-formed vertices survive.
    const auto out = parse_points("0.1,0.2;garbage;0.3,abc;0.4,0.5");
    REQUIRE(out.size() == 2);
    REQUIRE(out[0].x == 0.1f);
    REQUIRE(out[1].x == 0.4f);
    REQUIRE(out[1].y == 0.5f);
}

TEST_CASE("parse_points tolerates a trailing separator") {
    const auto out = parse_points("0.1,0.2;0.3,0.4;");
    REQUIRE(out.size() == 2);
}
