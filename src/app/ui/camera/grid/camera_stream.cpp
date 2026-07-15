#include "ui/camera/grid/camera_stream.h"

#include "ui/camera/shared/frame_convert.h"   // mat_to_qimage
#include "ui/camera/grid/frame_processor.h"
#include "ui/camera/grid/stream_pacing.h"     // next_backoff_ms
#include "ui/camera/grid/safe_process.h"      // safe_process
#include "ui/camera/shared/gst_pipeline.h"    // rtsp_gst_pipeline
#include "ui/camera/shared/rtsp_templates.h"  // with_credentials

#include <QDebug>
#include <QString>

#include <opencv2/videoio.hpp>

#include <algorithm>
#include <chrono>
#include <functional>
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
constexpr int kMaxInFlight = 2;         // drop-oldest: max frames queued for the GUI

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
    struct WaitableTimer {
        HANDLE h = CreateWaitableTimerExW(nullptr, nullptr,
                       CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        ~WaitableTimer() { if (h) CloseHandle(h); }
    };
    static thread_local WaitableTimer timer_holder;
    const HANDLE timer = timer_holder.h;
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

// Sleep `ms` in stop-responsive chunks; returns early if `stop` flips. Keeps the
// reconnect wait from delaying teardown.
bool wait_or_stop(int ms, const std::atomic<bool>& stop) {
    using namespace std::chrono;
    auto remaining = duration_cast<steady_clock::duration>(milliseconds(ms));
    const auto poll = duration_cast<steady_clock::duration>(milliseconds(kStopPollMs));
    while (remaining > steady_clock::duration::zero()) {
        if (stop.load()) return false;
        const auto chunk = std::min(remaining, poll);
        precise_sleep(chunk);
        remaining -= chunk;
    }
    return !stop.load();
}

// One labelled way to open a cv::VideoCapture for this camera.
struct SourceCandidate {
    const char* label;
    std::function<void(cv::VideoCapture&)> open;
};

// Ordered capture backends to try for `cam`. RTSP: hardware NVDEC H.264 → H.265
// → software FFMPEG. USB: hardware MJPEG → raw YUYV → OpenCV's default V4L2. We
// discover the right one by trying each until one opens AND yields a frame, so
// the camera's codec/pixel-format need not be known or configured up front. On
// non-Jetson hosts the nv* GStreamer elements are absent, so those candidates
// fail to build and the FFMPEG / CAP_ANY fallbacks take over.
std::vector<SourceCandidate> source_candidates(const camera::Camera& cam) {
    const std::vector<int> params = {
        cv::CAP_PROP_OPEN_TIMEOUT_MSEC, 5000,
        cv::CAP_PROP_READ_TIMEOUT_MSEC, 5000,
    };
    if (cam.camera_type == "usb") {
        const int index = cam.index ? static_cast<int>(*cam.index) : 0;
        const std::string dev = "/dev/video" + std::to_string(index);
        const int w = static_cast<int>(cam.width);
        const int h = static_cast<int>(cam.height);
        const int fps = static_cast<int>(cam.fps);
        return {
            {"usb-mjpeg-nvdec",
             [=](cv::VideoCapture& c) {
                 c.open(usb_mjpeg_gst_pipeline(dev, w, h, fps), cv::CAP_GSTREAMER);
             }},
            {"usb-yuyv",
             [=](cv::VideoCapture& c) {
                 c.open(usb_yuyv_gst_pipeline(dev, w, h, fps), cv::CAP_GSTREAMER);
             }},
            {"usb-any",
             [=, p = params](cv::VideoCapture& c) { c.open(index, cv::CAP_ANY, p); }},
        };
    }
    const QString rtsp = cam.rtsp ? QString::fromStdString(*cam.rtsp) : QString();
    const QString user = cam.username ? QString::fromStdString(*cam.username) : QString();
    const QString pass = cam.password ? QString::fromStdString(*cam.password) : QString();
    const std::string url = with_credentials(rtsp, user, pass).toStdString();
    return {
        {"rtsp-h264-nvdec",
         [=](cv::VideoCapture& c) {
             c.open(rtsp_gst_pipeline(url, Codec::H264), cv::CAP_GSTREAMER);
         }},
        {"rtsp-h265-nvdec",
         [=](cv::VideoCapture& c) {
             c.open(rtsp_gst_pipeline(url, Codec::H265), cv::CAP_GSTREAMER);
         }},
        {"rtsp-ffmpeg",
         [=, p = params](cv::VideoCapture& c) { c.open(url, cv::CAP_FFMPEG, p); }},
    };
}

// Try candidates (last-good first) until one opens AND reads a non-empty frame —
// opening alone can succeed just before a codec/format negotiation failure, so
// a real read is the accept test. Remembers the winner in `preferred` so
// reconnects skip the dead backends. The probe frame is discarded; the read loop
// fetches fresh.
bool open_source(const camera::Camera& cam, int& preferred, cv::VideoCapture& cap,
                 const std::atomic<bool>& stop) {
    const auto cands = source_candidates(cam);
    const int n = static_cast<int>(cands.size());
    for (int i = 0; i < n && !stop.load(); ++i) {
        const int idx = (preferred + i) % n;  // last-good backend first
        cv::VideoCapture c;
        cands[idx].open(c);
        cv::Mat probe;
        if (c.isOpened() && c.read(probe) && !probe.empty()) {
            cap = std::move(c);
            preferred = idx;
            // Debug, not info: a flapping camera reopens constantly — this would
            // flood the 24/7 log at the default level. The throttled reconnect
            // episode (in run()) carries the field-visible up/down signal.
            qDebug().noquote() << "[stream]" << QString::fromStdString(cam.name)
                               << "opened via" << cands[idx].label;
            return true;
        }
    }
    return false;
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
    using namespace std::chrono;
    const auto interval = milliseconds(kDisplayIntervalMs);
    const auto poll = duration_cast<steady_clock::duration>(milliseconds(kStopPollMs));
    int backoff_ms = 0;  // 0 -> first attempt is immediate; grows on failure

    while (!stop_.load()) {
        emit status_changed(static_cast<int>(Status::Connecting));

        cv::VideoCapture cap;
        // Discover the working capture backend (NVDEC H264/H265/FFMPEG for RTSP,
        // MJPEG/YUYV/V4L2 for USB) by trying each until one opens and delivers a
        // frame; the winner is remembered for fast reconnects.
        if (!open_source(cam_, preferred_source_, cap, stop_)) {
            if (stop_.load()) break;
            // Episode-throttled: log the first failure, then at most once every
            // 5 min while still down (with a suppressed count) — a camera offline
            // for days must not flood the 24/7 log every backoff cycle.
            const int64_t now =
                duration_cast<milliseconds>(steady_clock::now().time_since_epoch())
                    .count();
            const auto d = reconnect_episode_.on_failure(now);
            if (d.log) {
                qWarning().noquote()
                    << "[stream]" << QString::fromStdString(cam_.name)
                    << "failed to open (all backends) — retrying"
                    << (d.suppressed > 0
                            ? QStringLiteral("(%1 attempts suppressed)").arg(d.suppressed)
                            : QString());
            }
            emit status_changed(static_cast<int>(Status::Offline));
            backoff_ms = next_backoff_ms(backoff_ms);
            if (!wait_or_stop(backoff_ms, stop_)) break;
            continue;  // reconnect
        }
        // Opened — if we were in a failure episode, log one recovery line.
        {
            const int64_t now =
                duration_cast<milliseconds>(steady_clock::now().time_since_epoch())
                    .count();
            const auto r = reconnect_episode_.on_success(now);
            if (r.log) {
                qInfo().noquote()
                    << "[stream]" << QString::fromStdString(cam_.name)
                    << "reconnected after" << (r.downtime_ms / 1000) << "s and"
                    << r.total_failures << "failed attempts";
            }
        }

        // USB-only capture-resolution request (see note in the header history).
        if (cam_.camera_type == "usb" && cam_.width > 0 && cam_.height > 0) {
            cap.set(cv::CAP_PROP_FRAME_WIDTH, cam_.width);
            cap.set(cv::CAP_PROP_FRAME_HEIGHT, cam_.height);
        }
        emit status_changed(static_cast<int>(Status::Live));

        cv::Mat frame;
        while (!stop_.load()) {
            const auto t0 = steady_clock::now();
            if (!cap.read(frame) || frame.empty()) {
                break;  // dropped — fall through to reconnect (was: return)
            }
            backoff_ms = 0;  // a live frame resets the reconnect schedule
            if (should_emit(queued_->load(), kMaxInFlight)) {
                const QImage img = mat_to_qimage(frame);
                queued_->fetch_add(1);
                emit frame_ready(safe_process(processor_.get(), img));
            }
            // else: GUI is behind — drop this frame (drop-oldest).

            // Cap the display rate; chunked sleep stays responsive to stop().
            auto remaining = interval - (steady_clock::now() - t0);
            while (remaining > steady_clock::duration::zero() && !stop_.load()) {
                const auto chunk = std::min(remaining, poll);
                precise_sleep(chunk);
                remaining -= chunk;
            }
        }
        cap.release();

        if (stop_.load()) break;
        // Reached only on a mid-stream drop: signal offline and back off before
        // reopening.
        emit status_changed(static_cast<int>(Status::Offline));
        backoff_ms = next_backoff_ms(backoff_ms);
        if (!wait_or_stop(backoff_ms, stop_)) break;
    }
}

} // namespace denso::ui
