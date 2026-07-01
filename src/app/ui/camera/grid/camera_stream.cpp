#include "ui/camera/grid/camera_stream.h"

#include "ui/camera/shared/frame_convert.h"   // mat_to_qimage
#include "ui/camera/grid/frame_processor.h"
#include "ui/camera/shared/gst_pipeline.h"    // rtsp_gst_pipeline
#include "ui/camera/shared/rtsp_templates.h"  // with_credentials

#include <QDebug>
#include <QString>

#include <opencv2/videoio.hpp>

#include <algorithm>
#include <chrono>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX  // keep std::min/max usable
#endif
#include <windows.h>
#endif

namespace denso::ui {

namespace {
constexpr int kDisplayIntervalMs = 66;  // ~15 fps display cap, per camera
constexpr int kStopPollMs = 20;         // re-check stop() this often while pacing

// Sleep for `d` with sub-millisecond accuracy. std::this_thread::sleep_for on
// this MinGW runtime is pinned to the ~15.6 ms OS tick (and ignores
// timeBeginPeriod), which wrecks frame pacing: a 66 ms target overshoots to
// ~100 ms, so a "15 fps" cap actually delivers ~9 fps. A high-resolution
// waitable timer honours the requested duration. Falls back to sleep_for on
// non-Windows, or if the timer can't be created (pre-1803 Windows).
void precise_sleep(std::chrono::steady_clock::duration d) {
    if (d <= std::chrono::steady_clock::duration::zero()) {
        return;
    }
#ifdef _WIN32
    static thread_local HANDLE timer = CreateWaitableTimerExW(
        nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (timer) {
        const auto ns =
            std::chrono::duration_cast<std::chrono::nanoseconds>(d).count();
        LARGE_INTEGER due;
        due.QuadPart = -(ns / 100);  // negative = relative, in 100 ns units
        if (due.QuadPart < 0 &&
            SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
            WaitForSingleObject(timer, INFINITE);
            return;
        }
    }
#endif
    std::this_thread::sleep_for(d);
}
}

CameraStream::CameraStream(camera::Camera cam,
                           std::unique_ptr<FrameProcessor> processor,
                           QObject* parent)
    : QObject(parent), cam_(std::move(cam)), processor_(std::move(processor)) {}

CameraStream::~CameraStream() { stop(); }

void CameraStream::start() {
    if (thread_.joinable()) {
        return;  // already running
    }
    stop_.store(false);
    thread_ = std::thread([this] { run(); });
}

void CameraStream::stop() {
    stop_.store(true);
    if (thread_.joinable()) {
        thread_.join();
    }
}

void CameraStream::run() {
    emit status_changed(static_cast<int>(Status::Connecting));

    cv::VideoCapture cap;
    // Fail fast instead of hanging on an unreachable camera (mirrors snapshot).
    const std::vector<int> params = {
        cv::CAP_PROP_OPEN_TIMEOUT_MSEC, 5000,
        cv::CAP_PROP_READ_TIMEOUT_MSEC, 5000,
    };
    if (cam_.camera_type == "usb") {
        const int index = cam_.index ? static_cast<int>(*cam_.index) : 0;
        cap.open(index, cv::CAP_ANY, params);
    } else {
        const QString rtsp = cam_.rtsp ? QString::fromStdString(*cam_.rtsp) : QString();
        const QString user = cam_.username ? QString::fromStdString(*cam_.username) : QString();
        const QString pass = cam_.password ? QString::fromStdString(*cam_.password) : QString();
        const std::string url = with_credentials(rtsp, user, pass).toStdString();
        // Prefer GStreamer: it drops stale frames so glass-to-glass latency
        // stays low, unlike the FFMPEG backend which buffers unboundedly when
        // the reader lags. Fall back to FFMPEG if GStreamer can't open (e.g. no
        // gst plugins on the host) so IP cameras still work everywhere.
        cap.open(rtsp_gst_pipeline(url), cv::CAP_GSTREAMER);
        if (!cap.isOpened()) {
            qWarning().noquote() << "[stream]" << QString::fromStdString(cam_.name)
                                 << "GStreamer open failed — falling back to FFMPEG";
            cap.open(url, cv::CAP_FFMPEG, params);
        }
    }
    if (!cap.isOpened()) {
        qWarning().noquote() << "[stream]" << QString::fromStdString(cam_.name)
                             << "failed to open";
        emit status_changed(static_cast<int>(Status::Offline));
        return;
    }
    // Request a capture resolution only for USB devices. On the GStreamer
    // backend, set(FRAME_WIDTH/HEIGHT) reconfigures the live pipeline's caps and
    // segfaults inside gst_caps_new_simple; and for an RTSP stream the camera
    // dictates the resolution anyway, so the call is useless there. IP framing,
    // if ever needed, belongs in the pipeline (videoscale), not here.
    if (cam_.camera_type == "usb" && cam_.width > 0 && cam_.height > 0) {
        cap.set(cv::CAP_PROP_FRAME_WIDTH, cam_.width);
        cap.set(cv::CAP_PROP_FRAME_HEIGHT, cam_.height);
    }
    emit status_changed(static_cast<int>(Status::Live));

    using namespace std::chrono;
    const auto interval = milliseconds(kDisplayIntervalMs);
    const auto poll = duration_cast<steady_clock::duration>(milliseconds(kStopPollMs));
    cv::Mat frame;
    while (!stop_.load()) {
        const auto t0 = steady_clock::now();
        if (!cap.read(frame) || frame.empty()) {
            emit status_changed(static_cast<int>(Status::Offline));
            break;
        }
        const QImage img = mat_to_qimage(frame);
        emit frame_ready(processor_ ? processor_->process(img) : img);

        // Cap the display rate. Chunked sleep stays responsive to stop(), but
        // chunk/remaining MUST be in the clock's own duration — casting the
        // chunk to whole milliseconds truncates a sub-millisecond remainder to
        // 0, so `remaining -= 0` spins forever and wedges the feed.
        auto remaining = interval - (steady_clock::now() - t0);
        while (remaining > steady_clock::duration::zero() && !stop_.load()) {
            const auto chunk = std::min(remaining, poll);
            precise_sleep(chunk);
            remaining -= chunk;
        }
    }
    cap.release();
}

} // namespace denso::ui
