#include "ui/camera/shared/detection/merge_detections.h"

#include <catch2/catch_test_macros.hpp>

using denso::ui::merge_detections;
using denso::ui::NamedDetection;

TEST_CASE("overlapping same-name boxes collapse to the highest confidence") {
    std::vector<NamedDetection> in{
        {cv::Rect(10, 10, 100, 100), 0.6f, "person"},
        {cv::Rect(14, 12, 100, 100), 0.9f, "person"},  // ~same box, higher conf
    };
    const auto out = merge_detections(in, 0.5f);
    REQUIRE(out.size() == 1);
    CHECK(out[0].conf == 0.9f);
}

TEST_CASE("non-overlapping same-name boxes both survive") {
    std::vector<NamedDetection> in{
        {cv::Rect(0, 0, 50, 50), 0.8f, "person"},
        {cv::Rect(500, 500, 50, 50), 0.7f, "person"},
    };
    const auto out = merge_detections(in, 0.5f);
    REQUIRE(out.size() == 2);
}

TEST_CASE("overlapping boxes of different names are both kept") {
    std::vector<NamedDetection> in{
        {cv::Rect(10, 10, 100, 100), 0.8f, "person"},
        {cv::Rect(12, 12, 100, 100), 0.7f, "car"},
    };
    const auto out = merge_detections(in, 0.5f);
    REQUIRE(out.size() == 2);
}

TEST_CASE("output is sorted by confidence descending") {
    std::vector<NamedDetection> in{
        {cv::Rect(0, 0, 10, 10), 0.3f, "a"},
        {cv::Rect(100, 0, 10, 10), 0.9f, "b"},
        {cv::Rect(200, 0, 10, 10), 0.6f, "c"},
    };
    const auto out = merge_detections(in, 0.5f);
    REQUIRE(out.size() == 3);
    CHECK(out[0].conf == 0.9f);
    CHECK(out[1].conf == 0.6f);
    CHECK(out[2].conf == 0.3f);
}
