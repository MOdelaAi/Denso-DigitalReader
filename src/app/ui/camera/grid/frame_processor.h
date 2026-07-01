// The per-camera frame-processing seam for the live grid. A CameraStream reads
// raw frames and runs each through its FrameProcessor before display, so what a
// tile shows is decoupled from how it's produced.
//
// Today the only processor is OrientationProcessor (applies the configured
// rotation/pitch/roll). When the detection model lands, it becomes another
// FrameProcessor — selected per camera by a config flag — and nothing in the
// capture loop or the tile has to change.
#pragma once

#include "detection/detection.h"
#include "ui/camera/shared/detection/inference_engine.h"

#include <QImage>

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

/// Runs one or more detection models on each frame and draws the results. Each
/// entry pairs a shared engine with the camera's per-class selections (class id
/// → min confidence). Orientation is applied first (so a detection tile matches
/// the others), then detection is drawn on the oriented frame.
class DetectionProcessor : public FrameProcessor {
public:
    struct ModelRun {
        InferenceEngine* engine;                       // shared, non-owning
        std::vector<std::string> class_names;          // for labels
        std::vector<denso::detection::ModelClassSelection> classes;  // id → conf
    };

    DetectionProcessor(int degrees, double pitch, double roll,
                       std::vector<ModelRun> models);
    QImage process(const QImage& frame) override;

private:
    int degrees_;
    double pitch_;
    double roll_;
    std::vector<ModelRun> models_;
};

} // namespace denso::ui
