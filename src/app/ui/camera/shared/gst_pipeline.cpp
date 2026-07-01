#include "ui/camera/shared/gst_pipeline.h"

#include <algorithm>

namespace denso::ui {

std::string rtsp_gst_pipeline(const std::string& rtsp_url, Codec codec,
                              int latency_ms, int tcp_timeout_us) {
    const int latency = std::max(0, latency_ms);
    const int timeout = std::max(0, tcp_timeout_us);

    // Explicit decode chain per codec — no decodebin autoplugging (see header).
    const char* depay = codec == Codec::H265 ? "rtph265depay" : "rtph264depay";
    const char* parse = codec == Codec::H265 ? "h265parse" : "h264parse";
    const char* dec = codec == Codec::H265 ? "avdec_h265" : "avdec_h264";

    // rtspsrc: force TCP (avoids UDP packet loss / NAT issues), keep the jitter
    //   buffer shallow, and drop rather than stall when it overflows.
    // depay ! parse ! avdec: software decode — robust to dropped frames, unlike
    //   the hardware d3d12 decoder which crashes under concurrent multi-camera
    //   decode. rtspsrc's dynamic src pad links to depay at runtime.
    // queue leaky=downstream: on the decoded frames, throw away the oldest when
    //   the consumer falls behind — this is what keeps lag from accumulating.
    // appsink drop=true max-buffers=1 sync=false: hand OpenCV only the newest
    //   frame, decoded as fast as possible with no clock throttling.
    return "rtspsrc location=\"" + rtsp_url +
           "\" protocols=tcp latency=" + std::to_string(latency) +
           " drop-on-latency=true tcp-timeout=" + std::to_string(timeout) +
           " ! " + depay + " ! " + parse + " ! " + dec +
           " ! queue max-size-buffers=6 max-size-bytes=0 max-size-time=0"
           " leaky=downstream"
           " ! videoconvert"
           " ! appsink drop=true sync=false max-buffers=1";
}

} // namespace denso::ui
