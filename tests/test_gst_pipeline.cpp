#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "ui/camera/shared/gst_pipeline.h"

using Catch::Matchers::ContainsSubstring;
using denso::ui::Codec;
using denso::ui::rtsp_gst_pipeline;

TEST_CASE("rtsp_gst_pipeline embeds the URL and forces low-latency TCP") {
    const std::string p =
        rtsp_gst_pipeline("rtsp://user:pass@10.0.0.5:554/stream1");
    REQUIRE_THAT(p, ContainsSubstring(
                        "location=\"rtsp://user:pass@10.0.0.5:554/stream1\""));
    REQUIRE_THAT(p, ContainsSubstring("protocols=tcp"));
    REQUIRE_THAT(p, ContainsSubstring("drop-on-latency=true"));
}

TEST_CASE("rtsp_gst_pipeline drops stale frames at the queue and appsink") {
    const std::string p = rtsp_gst_pipeline("rtsp://cam/live");
    REQUIRE_THAT(p, ContainsSubstring("leaky=downstream"));
    REQUIRE_THAT(p, ContainsSubstring("appsink"));
    REQUIRE_THAT(p, ContainsSubstring("drop=true"));
    REQUIRE_THAT(p, ContainsSubstring("max-buffers=1"));
    REQUIRE_THAT(p, ContainsSubstring("sync=false"));
}

TEST_CASE("rtsp_gst_pipeline uses explicit software decode, not decodebin") {
    // decodebin's autoplugging can intermittently fail to link and crash the
    // capture; the explicit avdec chain links deterministically.
    const std::string p = rtsp_gst_pipeline("rtsp://cam/live");
    REQUIRE_THAT(p, !ContainsSubstring("decodebin"));
    REQUIRE_THAT(p, ContainsSubstring("rtph264depay"));
    REQUIRE_THAT(p, ContainsSubstring("h264parse"));
    REQUIRE_THAT(p, ContainsSubstring("avdec_h264"));
}

TEST_CASE("rtsp_gst_pipeline selects H.265 elements when asked") {
    const std::string p = rtsp_gst_pipeline("rtsp://cam/live", Codec::H265);
    REQUIRE_THAT(p, ContainsSubstring("rtph265depay"));
    REQUIRE_THAT(p, ContainsSubstring("h265parse"));
    REQUIRE_THAT(p, ContainsSubstring("avdec_h265"));
}

TEST_CASE("rtsp_gst_pipeline uses the given latency and clamps negatives") {
    REQUIRE_THAT(rtsp_gst_pipeline("rtsp://cam/live", Codec::H264, 200),
                 ContainsSubstring("latency=200"));
    REQUIRE_THAT(rtsp_gst_pipeline("rtsp://cam/live", Codec::H264, -5),
                 ContainsSubstring("latency=0"));
}

TEST_CASE("rtsp_gst_pipeline carries the tcp timeout that bounds a dead camera") {
    REQUIRE_THAT(rtsp_gst_pipeline("rtsp://cam/live", Codec::H264, 50, 3000000),
                 ContainsSubstring("tcp-timeout=3000000"));
}
