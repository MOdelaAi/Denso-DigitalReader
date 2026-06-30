#include <catch2/catch_test_macros.hpp>

#include "ui/camera/frame_convert.h"
#include "ui/camera/snapshot.h"

#include <QImage>
#include <opencv2/core.hpp>

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
