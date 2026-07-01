// Builds a low-latency GStreamer pipeline string for an RTSP IP camera, for
// cv::VideoCapture(pipeline, cv::CAP_GSTREAMER). The default cv::CAP_FFMPEG path
// buffers frames and, when the reader consumes slower than the camera pushes,
// glass-to-glass lag grows without bound. GStreamer with a leaky queue + a
// drop-on-latency rtspsrc + a shallow appsink always hands back the freshest
// frame instead, keeping latency near constant.
//
// Pure string builder (no OpenCV/Qt), so it's unit-tested off-device. Uses
// EXPLICIT depay/parse/decoder elements (not decodebin): decodebin's dynamic
// autoplugging intermittently fails to link ("not-linked") under load, which
// crashes the capture. The explicit chain links deterministically and forces
// the robust software avdec decoder. Mirrors the Aicam-detection-Pi5 pipeline.
#pragma once

#include <string>

namespace denso::ui {

/// Video codec carried by the RTSP stream — picks the depay/parse/decoder set.
enum class Codec { H264, H265 };

/// GStreamer pipeline for `rtsp_url` (credentials, if any, already embedded).
/// `codec` selects H.264 vs H.265 elements. `latency_ms` is the rtspsrc
/// jitter-buffer target (floored at 0); smaller is lower-latency but less
/// tolerant of network jitter. `tcp_timeout_us` bounds a dead-camera
/// connect/read so the capture thread stays responsive to stop().
std::string rtsp_gst_pipeline(const std::string& rtsp_url,
                              Codec codec = Codec::H264,
                              int latency_ms = 50,
                              int tcp_timeout_us = 5000000);

} // namespace denso::ui
