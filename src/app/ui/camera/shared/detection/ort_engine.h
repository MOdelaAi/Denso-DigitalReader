// ONNX Runtime implementation of InferenceEngine. Owns one Ort::Env + Session
// loaded once from a model file, with an execution-provider fallback chain
// (TensorRT → CUDA → CPU): the first tier whose session builds wins, so the
// same binary runs GPU-accelerated where available and CPU otherwise. On a
// TensorRT tier, engines are cached under engine_cache_dir. infer() letterboxes
// the frame, runs the model, and decodes to Detections at a low conf floor;
// per-class filtering is the caller's job.
#pragma once

#include "ui/camera/shared/detection/inference_engine.h"

#include <onnxruntime_cxx_api.h>

#include <memory>
#include <string>
#include <vector>

namespace denso::ui {

class OrtEngine : public InferenceEngine {
public:
    OrtEngine(const std::string& model_path, const std::string& engine_cache_dir);

    std::vector<Detection> infer(const cv::Mat& bgr) override;
    const std::vector<std::string>& class_names() const override { return names_; }
    bool ok() const { return static_cast<bool>(session_); }

    /// Read the `names` metadata from a model without keeping a session (used by
    /// the startup catalog sync). Returns empty on failure.
    static std::vector<std::string> read_names(const std::string& model_path);

private:
    Ort::Env env_;
    std::unique_ptr<Ort::Session> session_;
    Ort::AllocatorWithDefaultOptions alloc_;
    std::string input_name_;
    std::string output_name_;
    std::vector<std::string> names_;

    static constexpr int kSize = 640;
    static constexpr float kConfFloor = 0.25f;
    static constexpr float kNmsIou = 0.45f;
};

} // namespace denso::ui
