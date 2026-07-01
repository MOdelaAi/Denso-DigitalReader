// The inference boundary the DetectionProcessor depends on. An InferenceEngine
// turns one BGR frame into detections; the ONNX Runtime implementation lives in
// ort_engine.{h,cpp}. Keeping this an interface means the runtime backend (ORT
// today, a different one later) is swappable without touching the camera/UI.
#pragma once

#include <opencv2/core.hpp>

#include <vector>

namespace denso::ui {

/// One detection: box in original-frame pixels, class id, confidence.
struct Detection {
    cv::Rect box;
    int class_id = 0;
    float conf = 0.0f;
};

class InferenceEngine {
public:
    virtual ~InferenceEngine() = default;

    /// Run the model on one BGR frame; returns all detections above the
    /// engine's internal conf floor (per-class filtering is the caller's job).
    virtual std::vector<Detection> infer(const cv::Mat& bgr) = 0;

    /// Class names indexed by class id (from the model's metadata).
    virtual const std::vector<std::string>& class_names() const = 0;
};

} // namespace denso::ui
