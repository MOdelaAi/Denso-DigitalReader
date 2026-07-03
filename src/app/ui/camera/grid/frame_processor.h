// The per-camera frame-processing seam for the live grid. A CameraStream reads
// raw frames and runs each through its FrameProcessor before display, so what a
// tile shows is decoupled from how it's produced.
//
// Today the only processor is OrientationProcessor (applies the configured
// rotation/pitch/roll). When the detection model lands, it becomes another
// FrameProcessor — selected per camera by a config flag — and nothing in the
// capture loop or the tile has to change.
#pragma once

#include "camera/camera.h"  // CameraArea
#include "detection/detection.h"
#include "ui/camera/shared/detection/inference_engine.h"
#include "ui/camera/shared/detection/merge_detections.h"  // NamedDetection

#include <QImage>

#include <cstdint>
#include <string>
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

/// Applies the camera's saved orientation (rotation + pitch + roll) to each
/// frame. The no-op case (0/0/0) returns the frame unchanged, so an unoriented
/// camera pays nothing.
class OrientationProcessor : public FrameProcessor {
public:
    OrientationProcessor(int degrees, double pitch, double roll);
    QImage process(const QImage& frame) override;

private:
    int degrees_;
    double pitch_;
    double roll_;
};

/// Optional per-frame capture hook for the detection pipeline. When a
/// DetectionProcessor has a sink, it calls on_reading() with the frame's kept
/// detections so a consumer can record them (the data-logging feature).
///
/// CONTRACT: on_reading() is invoked on the CAPTURE THREAD, in the hot path. An
/// implementation MUST NOT block or do DB I/O inline — it must hand the data off
/// to a worker (e.g. a queue or common::post_to_gui) and return immediately.
struct ReadingSink {
    virtual ~ReadingSink() = default;
    virtual void on_reading(int64_t camera_id, int64_t ts_ms,
                            const std::vector<NamedDetection>& kept) = 0;
};

/// Runs one or more detection models on each frame and draws the results. Each
/// entry pairs a shared engine with the camera's per-class selections (class id
/// → min confidence). Orientation is applied first (so a detection tile matches
/// the others), then detection is drawn on the oriented frame.
///
/// If the camera has ROI `areas`, detection is confined to them: a box is drawn
/// only when its center lands inside some area polygon. Empty `areas` means no
/// confinement — every detection is drawn, as before. The displayed frame is
/// unchanged either way; only which boxes are drawn differs.
class DetectionProcessor : public FrameProcessor {
public:
    struct ModelRun {
        InferenceEngine* engine;                       // shared, non-owning
        std::vector<std::string> class_names;          // for labels
        std::vector<denso::detection::ModelClassSelection> classes;  // id → conf
    };

    DetectionProcessor(int degrees, double pitch, double roll,
                       std::vector<ModelRun> models,
                       std::vector<denso::camera::CameraArea> areas = {},
                       int64_t camera_id = 0, ReadingSink* sink = nullptr);
    QImage process(const QImage& frame) override;

private:
    int degrees_;
    double pitch_;
    double roll_;
    std::vector<ModelRun> models_;
    std::vector<denso::camera::CameraArea> areas_;  // empty = whole frame
    int64_t camera_id_ = 0;
    ReadingSink* sink_ = nullptr;  // non-owning; null = no reading capture
};

} // namespace denso::ui
