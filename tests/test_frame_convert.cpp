#include <catch2/catch_test_macros.hpp>

#include "ui/camera/shared/frame_convert.h"

#include <QImage>
#include <opencv2/core.hpp>

using denso::ui::qimage_to_mat;

TEST_CASE("qimage_to_mat converts RGB to BGR CV_8UC3") {
    QImage img(2, 1, QImage::Format_RGB888);
    img.setPixelColor(0, 0, QColor(255, 0, 0));   // red
    img.setPixelColor(1, 0, QColor(0, 0, 255));   // blue
    const cv::Mat m = qimage_to_mat(img);
    REQUIRE(m.cols == 2);
    REQUIRE(m.rows == 1);
    REQUIRE(m.type() == CV_8UC3);
    // OpenCV is BGR: red pixel → (0,0,255)
    REQUIRE(m.at<cv::Vec3b>(0, 0)[0] == 0);
    REQUIRE(m.at<cv::Vec3b>(0, 0)[2] == 255);
    REQUIRE(m.at<cv::Vec3b>(0, 1)[0] == 255);  // blue pixel → B=255
}
