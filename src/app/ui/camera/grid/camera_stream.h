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

#include "camera/model.h"

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

signals:
    void frame_ready(const QImage& frame);
    void status_changed(int status);  // CameraStream::Status as int (queued-safe)

private:
    void run();  // worker-thread body

    camera::Camera cam_;
    std::unique_ptr<FrameProcessor> processor_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
};

} // namespace denso::ui
