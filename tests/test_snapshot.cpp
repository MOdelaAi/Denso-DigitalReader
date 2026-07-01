#include <catch2/catch_test_macros.hpp>

#include "ui/camera/shared/frame_convert.h"
#include "ui/camera/shared/snapshot.h"

#include <QImage>
#include <opencv2/core.hpp>

using denso::ui::apply_orientation;
using denso::ui::apply_rotation;
using denso::ui::mat_to_qimage;

TEST_CASE("mat_to_qimage swaps BGR to RGB and keeps dimensions") {
    cv::Mat bgr(2, 3, CV_8UC3, cv::Scalar(255, 0, 0));  // pure blue in BGR
    const QImage img = mat_to_qimage(bgr);
    REQUIRE(img.width() == 3);
    REQUIRE(img.height() == 2);
    const QRgb px = img.pixel(0, 0);
    REQUIRE(qBlue(px) == 255);
    REQUIRE(qRed(px) == 0);
    REQUIRE(qGreen(px) == 0);
}

TEST_CASE("mat_to_qimage returns a null image for an empty mat") {
    REQUIRE(mat_to_qimage(cv::Mat()).isNull());
}

TEST_CASE("apply_rotation by 90 swaps width and height") {
    QImage src(4, 2, QImage::Format_RGB888);
    src.fill(Qt::black);
    const QImage out = apply_rotation(src, 90);
    REQUIRE(out.width() == 2);
    REQUIRE(out.height() == 4);
}

TEST_CASE("apply_rotation by 0 is identity in size") {
    QImage src(4, 2, QImage::Format_RGB888);
    src.fill(Qt::black);
    const QImage out = apply_rotation(src, 0);
    REQUIRE(out.width() == 4);
    REQUIRE(out.height() == 2);
}

TEST_CASE("apply_rotation by 180 preserves dimensions") {
    QImage src(4, 2, QImage::Format_RGB888);
    src.fill(Qt::black);
    const QImage out = apply_rotation(src, 180);
    REQUIRE(out.width() == 4);
    REQUIRE(out.height() == 2);
}

TEST_CASE("apply_rotation by 270 swaps width and height") {
    QImage src(4, 2, QImage::Format_RGB888);
    src.fill(Qt::black);
    const QImage out = apply_rotation(src, 270);
    REQUIRE(out.width() == 2);
    REQUIRE(out.height() == 4);
}

TEST_CASE("apply_orientation with no rotation/pitch/roll is identity in size") {
    QImage src(4, 2, QImage::Format_RGB888);
    src.fill(Qt::black);
    const QImage out = apply_orientation(src, 0, 0.0, 0.0);
    REQUIRE(out.width() == 4);
    REQUIRE(out.height() == 2);
}

TEST_CASE("apply_orientation honours the preset rotation when pitch/roll are zero") {
    QImage src(4, 2, QImage::Format_RGB888);
    src.fill(Qt::black);
    const QImage out = apply_orientation(src, 90, 0.0, 0.0);
    REQUIRE(out.width() == 2);
    REQUIRE(out.height() == 4);
}

TEST_CASE("apply_orientation applies roll as an in-plane rotation") {
    QImage src(4, 2, QImage::Format_RGB888);
    src.fill(Qt::black);
    // A 90° roll is an in-plane quarter turn ⇒ swaps width and height.
    const QImage out = apply_orientation(src, 0, 0.0, 90.0);
    REQUIRE(out.width() == 2);
    REQUIRE(out.height() == 4);
}

TEST_CASE("apply_orientation applies pitch as a perspective warp") {
    QImage src(40, 20, QImage::Format_RGB888);
    src.fill(Qt::black);
    const QImage out = apply_orientation(src, 0, 30.0, 0.0);
    // A non-zero pitch is a projective transform: the result is a valid,
    // non-empty image whose bounding box differs from the untilted frame.
    REQUIRE_FALSE(out.isNull());
    REQUIRE(out.height() != src.height());
}
