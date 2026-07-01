// One shared inference engine per distinct model file. Cameras that attach the
// same model reuse a single Ort::Session (loaded lazily on first request), so
// N cameras on the same model pay for one load, not N. Owns the engines; hand
// out non-owning pointers. Not thread-safe: build/query it from the UI thread
// (CameraGrid::reload), before the capture threads start.
#pragma once

#include "ui/camera/shared/detection/inference_engine.h"
#include "ui/camera/shared/detection/ort_engine.h"

#include <map>
#include <memory>
#include <string>

namespace denso::ui {

class EngineRegistry {
public:
    EngineRegistry(std::string models_dir, std::string cache_dir)
        : models_dir_(std::move(models_dir)), cache_dir_(std::move(cache_dir)) {}

    /// Engine for `filename` (resolved under models_dir), or nullptr if it
    /// failed to load. Cached across calls.
    InferenceEngine* get(const std::string& filename);

    /// Load AND warm (one blank inference) every *.onnx in models_dir. Call once
    /// at startup, before the capture threads run, so the first real frame
    /// doesn't stall on CUDA kernel init / allocation. Engines are cached, so
    /// cameras reuse the already-warm sessions. Blocking; logs per model.
    void warm_up();

private:
    std::string models_dir_;
    std::string cache_dir_;
    std::map<std::string, std::unique_ptr<OrtEngine>> engines_;
};

} // namespace denso::ui
