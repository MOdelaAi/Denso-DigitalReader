// Native TensorRT implementation of InferenceEngine (Jetson). Loads a PREBUILT
// .engine (never builds one) via nvinfer1::IRuntime and runs one BGR frame per
// inference through an IExecutionContext, reusing the shared letterbox + YOLO
// decode. One instance per model file is shared across cameras; infer() is
// mutex-guarded because a TensorRT execution context is not thread-safe.
//
// Engine-only, fail-loud: the constructor THROWS std::runtime_error (with the
// engine path + reason) if the engine is missing/incompatible, has an
// unexpected I/O layout, cannot admit batch=1, or has no class-names sidecar —
// which aborts warm-up and, in turn, startup. There is no fallback.
#pragma once

#include "detection/inference_engine.h"
#include "detection/trt_logger.h"

#include <NvInferRuntime.h>
#include <cuda_runtime_api.h>

#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace denso::ui {

class TrtEngine final : public InferenceEngine {
public:
    // cache_dir is intentionally unused: this backend only deserializes a
    // prebuilt TensorRT engine and never builds or caches one. The signature
    // mirrors OrtEngine so EngineRegistry constructs either uniformly.
    TrtEngine(const std::string& engine_path, const std::string& cache_dir);
    ~TrtEngine() override;

    TrtEngine(const TrtEngine&) = delete;
    TrtEngine& operator=(const TrtEngine&) = delete;
    TrtEngine(TrtEngine&&) = delete;
    TrtEngine& operator=(TrtEngine&&) = delete;

    std::vector<Detection> infer(const cv::Mat& bgr) override;
    const std::vector<std::string>& class_names() const override { return names_; }
    bool ok() const { return true; }  // ctor either succeeds or throws.

private:
    void release_cuda() noexcept;

    static constexpr int kSize = 640;
    static constexpr float kConfFloor = 0.25f;
    static constexpr float kNmsIou = 0.45f;

    std::string engine_path_;
    TrtLogger logger_;

    std::unique_ptr<nvinfer1::IRuntime> runtime_;
    std::unique_ptr<nvinfer1::ICudaEngine> engine_;
    std::unique_ptr<nvinfer1::IExecutionContext> context_;

    std::string input_name_;
    std::string output_name_;
    nvinfer1::Dims output_shape_{};

    cudaStream_t stream_ = nullptr;
    void* device_input_ = nullptr;
    void* device_output_ = nullptr;

    std::vector<float> host_input_;
    std::vector<float> host_output_;
    std::vector<std::string> names_;

    std::mutex infer_mutex_;
};

} // namespace denso::ui
