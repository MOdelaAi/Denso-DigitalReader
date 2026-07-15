// Builds low-latency GStreamer pipeline strings for cv::VideoCapture(...,
// cv::CAP_GSTREAMER). The default cv::CAP_FFMPEG path buffers frames and, when
// the reader consumes slower than the camera pushes, glass-to-glass lag grows
// without bound. A leaky queue + drop-on-latency + shallow appsink always hands
// back the freshest frame instead, keeping latency near constant.
//
// Decode runs on the Jetson's hardware NVDEC (nvv4l2decoder) so that up to four
// concurrent streams don't starve TensorRT + Qt on the CPU. The decoder emits
// NVMM surfaces; nvvidconv downloads them to system-memory BGRx and videoconvert
// drops the alpha to the BGR cv::Mat OpenCV expects. On non-Jetson hosts the
// nvv4l2decoder element is absent, so the pipeline fails to build and the caller
// falls back to FFMPEG — these strings are Jetson-oriented by design.
//
// Explicit depay/parse/decoder elements (never decodebin): decodebin's dynamic
// autoplugging intermittently fails to link under load, which crashes capture.
//
// Pure string builders (no OpenCV/Qt), so they're unit-tested off-device.
#pragma once

#include <string>

namespace denso::ui {

/// Video codec carried by the RTSP stream — picks the depay/parse set. NVDEC
/// (nvv4l2decoder) decodes both.
enum class Codec { H264, H265 };

/// RTSP pipeline for `rtsp_url` (credentials, if any, already embedded), using
/// hardware NVDEC decode → system-memory BGR. `codec` selects H.264 vs H.265
/// elements. `latency_ms` is the rtspsrc jitter-buffer target (floored at 0;
/// ~150 tolerates normal jitter — 0 makes jitter look like camera failure).
/// `tcp_timeout_us` bounds a dead-camera connect/read so the capture thread
/// stays responsive to stop().
std::string rtsp_gst_pipeline(const std::string& rtsp_url,
                              Codec codec = Codec::H264,
                              int latency_ms = 150,
                              int tcp_timeout_us = 5000000);

/// USB MJPEG pipeline with hardware JPEG decode (nvv4l2decoder mjpeg=1) → BGR.
/// width/height/fps caps are added only when > 0.
std::string usb_mjpeg_gst_pipeline(const std::string& device, int width = 0,
                                   int height = 0, int fps = 0);

/// USB raw-YUYV pipeline → BGR (no decoder; software videoconvert only).
/// width/height/fps caps are added only when > 0.
std::string usb_yuyv_gst_pipeline(const std::string& device, int width = 0,
                                  int height = 0, int fps = 0);

} // namespace denso::ui
