// One shared inference engine per distinct model file. Cameras that attach the
// same model reuse a single Ort::Session (loaded lazily on first request), so
// N cameras on the same model pay for one load, not N. Owns the engines; hand
// out non-owning pointers (never erased, so the pointers stay valid). get() is
// mutex-guarded: it is called from both the warm-up worker (during warm_up) and
// the GUI thread (starting a camera whose models are ready), so the map must be
// synchronized. infer() holds the raw engine pointer and never touches the
// registry, so there is no per-frame locking.
#pragma once

#include "ui/camera/shared/detection/inference_engine.h"
#ifdef _WIN32
#include "ui/camera/shared/detection/ort_engine.h"
#else
#include "ui/camera/shared/detection/trt_engine.h"
#endif

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <functional>

namespace denso::ui {

// The concrete backend engine per platform. Both expose ok() and a
// (path, cache_dir) constructor, so EngineRegistry builds either uniformly.
#ifdef _WIN32
using BackendEngine = OrtEngine;
#else
using BackendEngine = TrtEngine;
#endif

class EngineRegistry {
public:
    EngineRegistry(std::string models_dir, std::string cache_dir)
        : models_dir_(std::move(models_dir)), cache_dir_(std::move(cache_dir)) {}

    /// Engine for `filename` (resolved under models_dir), or nullptr if it
    /// failed to load. Cached across calls.
    InferenceEngine* get(const std::string& filename);

    /// Load AND warm (one blank inference) every *.onnx in models_dir, on the
    /// warm-up worker thread. `on_model(filename)` fires just before each model is
    /// prepared (progress display); `on_ready(filename)` fires just after it is
    /// successfully loaded + warmed (so the UI can start cameras that use it). A
    /// model that fails to load fires neither on_ready nor a start. Blocking.
    void warm_up(std::function<void(const std::string&)> on_model = {},
                 std::function<void(const std::string&)> on_ready = {});

private:
    std::string models_dir_;
    std::string cache_dir_;
    std::map<std::string, std::unique_ptr<BackendEngine>> engines_;
    std::mutex mutex_;
};

} // namespace denso::ui
