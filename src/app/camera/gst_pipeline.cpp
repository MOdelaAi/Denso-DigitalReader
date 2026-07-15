#include "camera/gst_pipeline.h"

#include <algorithm>
#include <string>

namespace denso::ui {
namespace {

// Shared tail for the hardware-decoded paths (RTSP + USB-MJPEG): the decoder
// emits NVMM surfaces, so a leaky queue drops whole DECODED frames (never
// compressed access units — that would corrupt the stream), nvvidconv downloads
// to system-memory BGRx, and videoconvert drops the alpha to the BGR cv::Mat
// OpenCV's appsink expects. Newest-frame-only throughout.
std::string nvmm_to_bgr_appsink() {
    return " ! queue max-size-buffers=1 max-size-bytes=0 max-size-time=0"
           " leaky=downstream"
           " ! nvvidconv ! video/x-raw,format=BGRx"
           " ! videoconvert ! video/x-raw,format=BGR"
           " ! appsink drop=true sync=false max-buffers=1";
}

// Optional ",width=..,height=..,framerate=../1" caps for a v4l2 source.
std::string wh_fps_caps(int width, int height, int fps) {
    std::string c;
    if (width > 0 && height > 0) {
        c += ",width=" + std::to_string(width) + ",height=" + std::to_string(height);
    }
    if (fps > 0) {
        c += ",framerate=" + std::to_string(fps) + "/1";
    }
    return c;
}

} // namespace

std::string rtsp_gst_pipeline(const std::string& rtsp_url, Codec codec,
                              int latency_ms, int tcp_timeout_us) {
    const int latency = std::max(0, latency_ms);
    const int timeout = std::max(0, tcp_timeout_us);
    const char* depay = codec == Codec::H265 ? "rtph265depay" : "rtph264depay";
    const char* parse = codec == Codec::H265 ? "h265parse" : "h264parse";

    // rtspsrc (TCP, drop-on-latency) -> depay -> parse -> hardware NVDEC decode
    // -> shared NVMM->BGR tail. TCP avoids UDP loss/NAT; drop-on-latency keeps
    // the jitter buffer from growing under load.
    return "rtspsrc location=\"" + rtsp_url +
           "\" protocols=tcp latency=" + std::to_string(latency) +
           " drop-on-latency=true tcp-timeout=" + std::to_string(timeout) +
           " ! " + depay + " ! " + parse +
           " ! nvv4l2decoder enable-max-performance=1" +
           nvmm_to_bgr_appsink();
}

std::string usb_mjpeg_gst_pipeline(const std::string& device, int width,
                                   int height, int fps) {
    // MJPEG USB cam: hardware JPEG decode on NVDEC, then the shared NVMM->BGR
    // tail. jpegparse frames the JPEG stream for nvv4l2decoder mjpeg=1.
    return "v4l2src device=" + device + " ! image/jpeg" +
           wh_fps_caps(width, height, fps) +
           " ! jpegparse ! nvv4l2decoder mjpeg=1" + nvmm_to_bgr_appsink();
}

std::string usb_yuyv_gst_pipeline(const std::string& device, int width,
                                  int height, int fps) {
    // Raw-YUYV USB cam: no decoder needed, just a CPU colour convert to BGR.
    return "v4l2src device=" + device + " ! video/x-raw,format=YUY2" +
           wh_fps_caps(width, height, fps) +
           " ! queue max-size-buffers=1 leaky=downstream"
           " ! videoconvert ! video/x-raw,format=BGR"
           " ! appsink drop=true sync=false max-buffers=1";
}

} // namespace denso::ui
