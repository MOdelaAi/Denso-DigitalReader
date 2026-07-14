// One camera's live capture, off the GUI thread. Owns a cv::VideoCapture and a
// worker thread that reads frames in a loop, runs each through a FrameProcessor
// (orientation today, the detection model later), and emits the result. Frames
// and status are delivered via queued signals, so a tile just connects and
// paints. Throttled to a modest display rate so several feeds stay light.
//
// Lifecycle: construct → start() → … → stop() (or destruction). stop() is
// idempotent and joins the worker; the capture's finite open/read timeout keeps
// teardown from hanging on a dead camera.
#pragma once

#include "camera/camera.h"

#include <QImage>
#include <QObject>

#include <atomic>
#include <memory>
#include <thread>

namespace denso::ui {

class FrameProcessor;

class CameraStream : public QObject {
    Q_OBJECT

public:
    enum class Status { Connecting, Live, Offline };

    CameraStream(camera::Camera cam, std::unique_ptr<FrameProcessor> processor,
                 QObject* parent = nullptr);
    ~CameraStream() override;

    void start();  // launch the capture thread (no-op if already running)
    void stop();   // signal stop + join; idempotent

    /// Shared drop-oldest backpressure counter: frames emitted but not yet
    /// consumed by the tile. Handed to the paired CameraTile so it can decrement.
    std::shared_ptr<std::atomic<int>> frame_counter() const { return queued_; }

signals:
    void frame_ready(const QImage& frame);
    void status_changed(int status);  // CameraStream::Status as int (queued-safe)

private:
    void run();  // worker-thread body

    camera::Camera cam_;
    std::unique_ptr<FrameProcessor> processor_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    int preferred_source_ = 0;  // last capture backend that worked; tried first on reconnect
    std::shared_ptr<std::atomic<int>> queued_{
        std::make_shared<std::atomic<int>>(0)};
};

} // namespace denso::ui
