#include <catch2/catch_test_macros.hpp>

#include "ui/camera/shared/detection/letterbox.h"

#include <opencv2/core.hpp>

using denso::ui::letterbox;
using denso::ui::LetterboxInfo;
using denso::ui::undo_letterbox;

TEST_CASE("letterbox scales the long side to 640 and pads the short side") {
    cv::Mat src(480, 640, CV_8UC3, cv::Scalar(0, 0, 0));  // 640x480 landscape
    cv::Mat dst;
    const LetterboxInfo lb = letterbox(src, dst, 640);
    REQUIRE(dst.cols == 640);
    REQUIRE(dst.rows == 640);
    REQUIRE(lb.scale == 1.0f);          // 640 long side already fits
    REQUIRE(lb.pad_x == 0);
    REQUIRE(lb.pad_y == 80);            // (640-480)/2
}

TEST_CASE("undo_letterbox inverts the mapping to original pixels") {
    cv::Mat src(480, 640, CV_8UC3, cv::Scalar(0, 0, 0));
    cv::Mat dst;
    const LetterboxInfo lb = letterbox(src, dst, 640);
    // A box centered in the padded image at (320, 320) size 64x64 maps back to
    // original center (320, 240).
    const cv::Rect r = undo_letterbox(320.f, 320.f, 64.f, 64.f, lb, 640, 480);
    REQUIRE(r.x == 288);   // 320 - 32
    REQUIRE(r.y == 208);   // (320-80) - 32 = 208
    REQUIRE(r.width == 64);
    REQUIRE(r.height == 64);
}
