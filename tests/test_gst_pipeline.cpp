#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "ui/camera/shared/gst_pipeline.h"

using Catch::Matchers::ContainsSubstring;
using denso::ui::Codec;
using denso::ui::rtsp_gst_pipeline;
using denso::ui::usb_mjpeg_gst_pipeline;
using denso::ui::usb_yuyv_gst_pipeline;

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

TEST_CASE("rtsp_gst_pipeline uses hardware NVDEC decode, not decodebin/software") {
    // Orin Nano: offload decode to NVDEC (nvv4l2decoder) so 4 concurrent streams
    // don't starve TensorRT + Qt on the CPU. Explicit depay/parse (not decodebin,
    // whose autoplug intermittently fails to link under load).
    const std::string p = rtsp_gst_pipeline("rtsp://cam/live");
    REQUIRE_THAT(p, !ContainsSubstring("decodebin"));
    REQUIRE_THAT(p, !ContainsSubstring("avdec"));
    REQUIRE_THAT(p, ContainsSubstring("rtph264depay"));
    REQUIRE_THAT(p, ContainsSubstring("h264parse"));
    REQUIRE_THAT(p, ContainsSubstring("nvv4l2decoder"));
}

TEST_CASE("rtsp_gst_pipeline selects H.265 elements when asked") {
    const std::string p = rtsp_gst_pipeline("rtsp://cam/live", Codec::H265);
    REQUIRE_THAT(p, ContainsSubstring("rtph265depay"));
    REQUIRE_THAT(p, ContainsSubstring("h265parse"));
    REQUIRE_THAT(p, ContainsSubstring("nvv4l2decoder"));  // NVDEC handles both codecs
}

TEST_CASE("rtsp_gst_pipeline converts NVMM to system-memory BGR for OpenCV") {
    const std::string p = rtsp_gst_pipeline("rtsp://cam/live");
    REQUIRE_THAT(p, ContainsSubstring("nvvidconv"));
    REQUIRE_THAT(p, ContainsSubstring("video/x-raw,format=BGR"));
}

TEST_CASE("rtsp_gst_pipeline drops only whole decoded frames (queue after decoder)") {
    // The leaky queue must sit AFTER nvv4l2decoder — dropping compressed access
    // units corrupts the stream until the next keyframe.
    const std::string p = rtsp_gst_pipeline("rtsp://cam/live");
    const auto dec = p.find("nvv4l2decoder");
    const auto leaky = p.find("leaky=downstream");
    REQUIRE(dec != std::string::npos);
    REQUIRE(leaky != std::string::npos);
    REQUIRE(dec < leaky);
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

TEST_CASE("usb_mjpeg_gst_pipeline hardware-decodes MJPEG to BGR") {
    const std::string p = usb_mjpeg_gst_pipeline("/dev/video0");
    REQUIRE_THAT(p, ContainsSubstring("v4l2src device=/dev/video0"));
    REQUIRE_THAT(p, ContainsSubstring("image/jpeg"));
    REQUIRE_THAT(p, ContainsSubstring("nvv4l2decoder mjpeg=1"));
    REQUIRE_THAT(p, ContainsSubstring("video/x-raw,format=BGR"));
    REQUIRE_THAT(p, ContainsSubstring("appsink"));
}

TEST_CASE("usb_mjpeg_gst_pipeline adds width/height/fps caps when given") {
    const std::string p = usb_mjpeg_gst_pipeline("/dev/video2", 1280, 720, 30);
    REQUIRE_THAT(p, ContainsSubstring("width=1280,height=720"));
    REQUIRE_THAT(p, ContainsSubstring("framerate=30/1"));
}

TEST_CASE("usb_yuyv_gst_pipeline converts YUY2 to BGR without a decoder") {
    const std::string p = usb_yuyv_gst_pipeline("/dev/video0");
    REQUIRE_THAT(p, ContainsSubstring("format=YUY2"));
    REQUIRE_THAT(p, ContainsSubstring("videoconvert"));
    REQUIRE_THAT(p, ContainsSubstring("video/x-raw,format=BGR"));
    REQUIRE_THAT(p, !ContainsSubstring("nvv4l2decoder"));  // raw frames, no decode
}
