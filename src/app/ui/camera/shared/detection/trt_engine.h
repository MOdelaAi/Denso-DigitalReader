// Native TensorRT implementation of InferenceEngine (Jetson). Loads a PREBUILT
// .engine (never builds one) and runs one BGR frame per inference.
//
// Phase A: this is a compile-only STUB so the Linux/Jetson app builds and runs
// without inference wired. Phase B replaces the body with real TensorRT
// (deserialize + per-camera execution context + reuse of letterbox/decode).
#pragma once

#include "ui/camera/shared/detection/inference_engine.h"

#include <string>
#include <vector>

namespace denso::ui {

class TrtEngine : public InferenceEngine {
public:
    // Signature mirrors OrtEngine so EngineRegistry constructs either uniformly.
    // cache_dir is unused for prebuilt engines (kept for a common call site).
    TrtEngine(const std::string& engine_path, const std::string& cache_dir);

    std::vector<Detection> infer(const cv::Mat& bgr) override;
    const std::vector<std::string>& class_names() const override { return names_; }
    bool ok() const { return ok_; }

private:
    std::vector<std::string> names_;
    bool ok_ = true;  // Phase A stub is always "ok"; Phase B sets it on load.
};

} // namespace denso::ui
