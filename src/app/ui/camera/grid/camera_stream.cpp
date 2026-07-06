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
            // Prefer GStreamer (drops stale frames, low latency); fall back to
            // FFMPEG if gst can't open (no plugins on the host).
            cap.open(rtsp_gst_pipeline(url), cv::CAP_GSTREAMER);
            if (!cap.isOpened()) {
                qWarning().noquote() << "[stream]" << QString::fromStdString(cam_.name)
                                     << "GStreamer open failed — falling back to FFMPEG";
                cap.open(url, cv::CAP_FFMPEG, params);
            }
        }

        if (!cap.isOpened()) {
            qWarning().noquote() << "[stream]" << QString::fromStdString(cam_.name)
                                 << "failed to open — retrying";
            emit status_changed(static_cast<int>(Status::Offline));
            backoff_ms = next_backoff_ms(backoff_ms);
            if (!wait_or_stop(backoff_ms, stop_)) break;
            continue;  // reconnect
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
