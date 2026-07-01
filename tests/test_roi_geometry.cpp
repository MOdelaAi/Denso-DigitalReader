#include <catch2/catch_test_macros.hpp>

#include "ui/camera/shared/roi_geometry.h"

#include <QPointF>
#include <QRectF>
#include <QSizeF>

using denso::ui::fitted_image_rect;
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
