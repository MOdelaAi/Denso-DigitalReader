#include <catch2/catch_test_macros.hpp>

#include "ui/camera/shared/detection/letterbox.h"
#include "ui/camera/shared/detection/yolo_decode.h"

#include <vector>

using denso::ui::decode_yolo;
using denso::ui::decode_yolo_end2end;
using denso::ui::Detection;
using denso::ui::LetterboxInfo;

TEST_CASE("decode_yolo returns the argmax class above the conf floor") {
    // nc=2 classes, na=2 anchors. Layout is [4+nc, na] row-major: row r, col a
    // at index r*na + a. Anchor 0: box (320,320,64,64), class1=0.9. Anchor 1:
    // all-low scores (filtered out).
    const int nc = 2, na = 2;
    std::vector<float> out((4 + nc) * na, 0.0f);
    auto at = [&](int row, int a) -> float& { return out[row * na + a]; };
    at(0, 0) = 320.f; at(1, 0) = 320.f; at(2, 0) = 64.f; at(3, 0) = 64.f;
    at(4, 0) = 0.10f;  // class 0 score
    at(5, 0) = 0.90f;  // class 1 score
    at(4, 1) = 0.05f; at(5, 1) = 0.02f;  // anchor 1 below floor

    LetterboxInfo lb;  // identity: scale 1, no pad, size 640
    lb.scale = 1.0f; lb.pad_x = 0; lb.pad_y = 0; lb.size = 640;

    const auto dets = decode_yolo(out.data(), nc, na, lb, 640, 640, 0.25f, 0.45f);
    REQUIRE(dets.size() == 1);
    REQUIRE(dets[0].class_id == 1);
    REQUIRE(dets[0].conf == 0.90f);
    REQUIRE(dets[0].box.x == 288);   // 320 - 32
    REQUIRE(dets[0].box.width == 64);
}

TEST_CASE("decode_yolo_end2end filters by conf and maps xyxy boxes back") {
    LetterboxInfo lb; lb.scale = 1.0f; lb.pad_x = 0; lb.pad_y = 0; lb.size = 640;
    // [num_dets, 6] row-major: x1,y1,x2,y2,score,class_id
    std::vector<float> out = {
        100.f, 100.f, 200.f, 200.f, 0.90f, 3.f,   // keep
         10.f,  10.f,  50.f,  50.f, 0.10f, 1.f,    // below floor -> drop
    };
    const auto dets = decode_yolo_end2end(out.data(), 2, lb, 640, 640, 0.25f);
    REQUIRE(dets.size() == 1);
    REQUIRE(dets[0].class_id == 3);
    REQUIRE(dets[0].conf == 0.90f);
    REQUIRE(dets[0].box.x == 100);
    REQUIRE(dets[0].box.y == 100);
    REQUIRE(dets[0].box.width == 100);
    REQUIRE(dets[0].box.height == 100);
}
