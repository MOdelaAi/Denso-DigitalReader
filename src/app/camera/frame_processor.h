// The per-camera frame-processing seam for the live grid. A CameraStream reads
// raw frames and runs each through its FrameProcessor before display, so what a
// tile shows is decoupled from how it's produced.
//
// OrientationProcessor applies the configured rotation/pitch/roll (cheap, on the
// display path). DetectionProcessor additionally runs the model — but inference
// runs on its OWN worker thread, off the display path: process() submits the
// newest frame (drop-oldest) and draws the most-recent cached detections, so the
// video stays smooth even when inference is slow. Boxes lag by a frame or two,
// which is the correct real-time-UI trade-off.
#pragma once

#include "camera/camera.h"  // CameraArea
#include "detection/detection.h"
#include "brazing/zone_sink.h"  // ZoneSink (+ ZoneReading)
#include "brazing/zone_runtime.h"  // ZoneRuntimeEntry
#include "detection/inference_engine.h"
#include "detection/merge_detections.h"  // NamedDetection

#include <QImage>

#include <opencv2/core.hpp>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace denso::ui {

class FrameProcessor {
public:
    virtual ~FrameProcessor() = default;

    /// Transform one captured frame for display. Called on the capture thread,
    /// so implementations must not touch the GUI. Must be safe to call rapidly.
    virtual QImage process(const QImage& frame) = 0;
};

/// Supplies THIS camera's zone rows for the frame being drawn, already filtered
/// by camera_id — so a frame can never show a zone number another camera owns.
///
/// CONTRACT: invoked on the CAPTURE THREAD, once per displayed frame. The
/// implementation must be thread-safe and cheap; the wiring binds it to
/// ZoneReporter::runtime_view(), which is mutex-guarded and returns copies.
/// Empty (unset) means this camera draws no zone annotation.
using ZoneViewFn = std::function<std::vector<ZoneRuntimeEntry>()>;

/// Applies the camera's saved orientation (rotation + pitch + roll) to each
/// frame. The no-op case (0/0/0) returns the frame unchanged, so an unoriented
/// camera pays nothing.
///
/// It also carries the zone annotation, so a camera with zone-numbered ROIs but
/// NO detection model still shows its zones (as Acquiring/Paused) instead of
/// silently losing the overlay. The QImage->Mat->QImage round trip happens only
/// when there is actually something to draw.
class OrientationProcessor : public FrameProcessor {
public:
    OrientationProcessor(int degrees, double pitch, double roll,
                         ZoneViewFn zone_view = {});
    QImage process(const QImage& frame) override;

private:
    int degrees_;
    double pitch_;
    double roll_;
    ZoneViewFn zone_view_;
};

/// Optional per-frame capture hook for the detection pipeline. When a
/// DetectionProcessor has a sink, it calls on_reading() with the frame's kept
/// detections so a consumer can record them (the data-logging feature).
///
/// CONTRACT: on_reading() is invoked on the INFERENCE WORKER THREAD. An
/// implementation MUST NOT block or do DB I/O inline — it must hand the data off
/// to a worker (e.g. a queue or common::post_to_gui) and return immediately.
struct ReadingSink {
    virtual ~ReadingSink() = default;
    virtual void on_reading(int64_t camera_id, int64_t ts_ms,
                            const std::vector<NamedDetection>& kept) = 0;
};


/// Runs one or more detection models and draws the results — but decoupled from
/// display. Orientation is applied on the display path; inference runs on a
/// dedicated worker over a drop-oldest latest-frame slot, publishing a detections
/// snapshot that process() overlays on subsequent frames. Each ModelRun pairs a
/// shared engine with the camera's per-class selections (class id → min conf).
///
/// If the camera has ROI `areas`, detection is confined to them: a box is kept
/// only when its center lands inside some area polygon. Empty `areas` means no
/// confinement.
class DetectionProcessor : public FrameProcessor {
public:
    struct ModelRun {
        InferenceEngine* engine;                       // shared, non-owning
        std::vector<std::string> class_names;          // for labels
        std::vector<denso::detection::ModelClassSelection> classes;  // id → conf
    };

    /// Raised when consecutive inference failures cross kInferFailInhibitAfter,
    /// and cleared once inference recovers. CONTRACT: invoked on the INFERENCE
    /// WORKER THREAD — the wiring must marshal to the GUI thread
    /// (common::post_to_gui) before touching ZoneHealth, which is single-threaded
    /// by design. Passed at construction (not via a setter) so it is set before
    /// the worker thread starts: the worker reads it every frame, so a later write
    /// from another thread would be a data race on the std::function.
    using WorkerFailedFn = std::function<void(int64_t camera_id, bool failed)>;

    /// Test-only: how many DetectionProcessors this process has EVER constructed
    /// (monotonic, increment-only). Mirrors CameraStream::constructed_count(). It
    /// is what lets a test prove an inhibited camera built NO detection pipeline,
    /// rather than inferring it from an absence of side effects. No production
    /// behaviour reads it.
    static uint64_t constructed_count();

    DetectionProcessor(int degrees, double pitch, double roll,
                       std::vector<ModelRun> models,
                       std::vector<denso::camera::CameraArea> areas = {},
                       int64_t camera_id = 0, ReadingSink* sink = nullptr,
                       ZoneSink* zone_sink = nullptr,
                       WorkerFailedFn on_worker_failed = {},
                       ZoneViewFn zone_view = {});
    ~DetectionProcessor() override;  // stops + joins the inference worker

    DetectionProcessor(const DetectionProcessor&) = delete;
    DetectionProcessor& operator=(const DetectionProcessor&) = delete;

    QImage process(const QImage& frame) override;  // display path: submit + overlay

private:
    void infer_loop();                              // worker-thread body
    std::vector<NamedDetection> run_inference(const cv::Mat& bgr);  // pool + merge

    int degrees_;
    double pitch_;
    double roll_;
    std::vector<ModelRun> models_;
    std::vector<denso::camera::CameraArea> areas_;  // empty = whole frame
    int64_t camera_id_ = 0;
    ReadingSink* sink_ = nullptr;     // non-owning; null = no reading capture
    ZoneSink* zone_sink_ = nullptr;   // non-owning; null = no zone reporting
    WorkerFailedFn on_worker_failed_; // empty = no inference-failure escalation
    // This camera's zone rows, read once per displayed frame on the capture
    // thread. Empty = no zone annotation for this camera.
    ZoneViewFn zone_view_;

    // Latest-frame slot handed to the inference worker (drop-oldest).
    std::mutex slot_mtx_;
    std::condition_variable slot_cv_;
    cv::Mat pending_;
    bool has_pending_ = false;
    bool stop_ = false;

    // Detections snapshot published by the worker, drawn on the display path.
    std::mutex det_mtx_;
    std::vector<NamedDetection> latest_;
    int det_w_ = 0;  // frame width the boxes were computed at
    int det_h_ = 0;

    // Consecutive inference-failure count (worker thread only; no lock needed).
    // Throttles the failure log to once per this many consecutive throws.
    static constexpr int kInferFailLogEvery = 60;
    // Consecutive inference failures before the camera is escalated as failed
    // (on_worker_failed_ fires once at this exact count; cleared on recovery).
    static constexpr int kInferFailInhibitAfter = 10;
    int infer_fail_streak_ = 0;

    // Process-lifetime construction tally behind constructed_count().
    static std::atomic<uint64_t> s_constructed_;

    std::thread worker_;  // started last in the ctor
};

} // namespace denso::ui
