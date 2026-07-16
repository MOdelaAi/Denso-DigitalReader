#include <catch2/catch_test_macros.hpp>

#include "ui/camera/shared/roi_geometry.h"

#include <QPointF>
#include <QRectF>
#include <QSizeF>

using denso::ui::fitted_image_rect;
using denso::ui::hit_test_vertex;
using denso::ui::image_contains;
using denso::ui::nearest_edge_insert_index;
using denso::ui::to_normalized;
using denso::ui::to_widget;

namespace {
bool close(double a, double b) { return std::abs(a - b) < 1e-4; }
}

TEST_CASE("fitted_image_rect letterboxes a wide image into a square widget") {
    // 200x100 image into a 100x100 widget: scale 0.5 ⇒ 100x50, centered vertically.
    const QRectF r = fitted_image_rect(QSizeF(200, 100), QSizeF(100, 100));
    REQUIRE(close(r.width(), 100.0));
    REQUIRE(close(r.height(), 50.0));
    REQUIRE(close(r.left(), 0.0));
    REQUIRE(close(r.top(), 25.0));
}

TEST_CASE("fitted_image_rect pillarboxes a tall image into a square widget") {
    const QRectF r = fitted_image_rect(QSizeF(100, 200), QSizeF(100, 100));
    REQUIRE(close(r.width(), 50.0));
    REQUIRE(close(r.height(), 100.0));
    REQUIRE(close(r.left(), 25.0));
    REQUIRE(close(r.top(), 0.0));
}

TEST_CASE("to_normalized maps the image rect corners to (0,0) and (1,1)") {
    const QRectF img(10, 20, 100, 50);
    const QPointF tl = to_normalized(QPointF(10, 20), img);
    const QPointF br = to_normalized(QPointF(110, 70), img);
    REQUIRE(close(tl.x(), 0.0));
    REQUIRE(close(tl.y(), 0.0));
    REQUIRE(close(br.x(), 1.0));
    REQUIRE(close(br.y(), 1.0));
}

TEST_CASE("to_normalized clamps points outside the image rect to [0,1]") {
    const QRectF img(0, 0, 100, 100);
    const QPointF p = to_normalized(QPointF(-30, 250), img);
    REQUIRE(close(p.x(), 0.0));
    REQUIRE(close(p.y(), 1.0));
}

TEST_CASE("to_widget is the inverse of to_normalized within the image rect") {
    const QRectF img(10, 20, 100, 50);
    const QPointF src(55, 40);
    const QPointF round = to_widget(to_normalized(src, img), img);
    REQUIRE(close(round.x(), 55.0));
    REQUIRE(close(round.y(), 40.0));
}

// ─── image_contains ──────────────────────────────────────────────────────────
// The clamp in to_normalized is a safety net, not a gate: without an explicit
// contains-check the canvas silently turned a click in the letterbox bars into
// a vertex pinned to the image edge.

TEST_CASE("image_contains: a point inside the image rect is inside") {
    REQUIRE(image_contains(QRectF(10, 20, 100, 50), QPointF(55, 40), 0.0));
}

TEST_CASE("image_contains: a point in the letterbox bar is outside") {
    // 200x100 image in a 100x100 widget → bars above and below the fitted rect.
    const QRectF img = fitted_image_rect(QSizeF(200, 100), QSizeF(100, 100));
    REQUIRE_FALSE(image_contains(img, QPointF(50, 5), 0.0));   // top bar
    REQUIRE_FALSE(image_contains(img, QPointF(50, 95), 0.0));  // bottom bar
    REQUIRE(image_contains(img, QPointF(50, 50), 0.0));        // on the image
}

TEST_CASE("image_contains: the tolerance admits a near-miss at the edge") {
    const QRectF img(10, 20, 100, 50);
    REQUIRE_FALSE(image_contains(img, QPointF(6, 40), 0.0));
    REQUIRE(image_contains(img, QPointF(6, 40), 5.0));  // within 5px slop
}

TEST_CASE("image_contains: an empty image rect contains nothing") {
    REQUIRE_FALSE(image_contains(QRectF(), QPointF(0, 0), 4.0));
}

// ─── hit_test_vertex ─────────────────────────────────────────────────────────

namespace {
// A square's four corners in widget space.
const QList<QPointF> kCorners = {QPointF(20, 20), QPointF(80, 20),
                                 QPointF(80, 80), QPointF(20, 80)};
}  // namespace

TEST_CASE("hit_test_vertex: a click on a vertex returns its index") {
    REQUIRE(hit_test_vertex(kCorners, QPointF(80, 20), 10.0) == 1);
    REQUIRE(hit_test_vertex(kCorners, QPointF(20, 80), 10.0) == 3);
}

TEST_CASE("hit_test_vertex: a click within the radius still grabs the vertex") {
    REQUIRE(hit_test_vertex(kCorners, QPointF(84, 23), 10.0) == 1);
}

TEST_CASE("hit_test_vertex: a click beyond the radius grabs nothing") {
    REQUIRE(hit_test_vertex(kCorners, QPointF(50, 50), 10.0) == -1);
}

TEST_CASE("hit_test_vertex: the nearest vertex wins when two are in range") {
    // Midway between corners 0 and 1, nudged toward 0, with a radius that
    // reaches both. Grabbing "whichever came first" would be a drag surprise.
    REQUIRE(hit_test_vertex(kCorners, QPointF(45, 20), 40.0) == 0);
    REQUIRE(hit_test_vertex(kCorners, QPointF(55, 20), 40.0) == 1);
}

TEST_CASE("hit_test_vertex: an empty polygon has nothing to hit") {
    REQUIRE(hit_test_vertex({}, QPointF(20, 20), 10.0) == -1);
}

// ─── nearest_edge_insert_index ───────────────────────────────────────────────
// Returns the index the new vertex should be INSERTED AT, i.e. one past the
// edge's start vertex, so the polygon keeps its winding.

TEST_CASE("nearest_edge_insert_index: a click on the top edge inserts at 1") {
    REQUIRE(nearest_edge_insert_index(kCorners, QPointF(50, 20), 8.0) == 1);
}

TEST_CASE("nearest_edge_insert_index: a click on the right edge inserts at 2") {
    REQUIRE(nearest_edge_insert_index(kCorners, QPointF(80, 50), 8.0) == 2);
}

TEST_CASE("nearest_edge_insert_index: the implicit closing edge is insertable") {
    // The last→first edge is never stored but is drawn, so it must accept a
    // vertex too — appending at the end keeps the winding.
    REQUIRE(nearest_edge_insert_index(kCorners, QPointF(20, 50), 8.0) == 4);
}

TEST_CASE("nearest_edge_insert_index: a click far from every edge inserts nowhere") {
    REQUIRE(nearest_edge_insert_index(kCorners, QPointF(50, 50), 8.0) == -1);
}

TEST_CASE("nearest_edge_insert_index: a click past an edge's end does not match it") {
    // Beyond the top edge's right end: the perpendicular distance to the
    // infinite line is 0, but the segment is what counts.
    REQUIRE(nearest_edge_insert_index(kCorners, QPointF(200, 20), 8.0) == -1);
}

TEST_CASE("nearest_edge_insert_index: fewer than 3 vertices has no edge to split") {
    REQUIRE(nearest_edge_insert_index({}, QPointF(0, 0), 8.0) == -1);
    REQUIRE(nearest_edge_insert_index({QPointF(20, 20)}, QPointF(20, 20), 8.0) ==
            -1);
}
