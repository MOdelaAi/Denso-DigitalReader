#include "detection/engine_registry.h"

#include "detection/engine_requirements.h"

#include <QDebug>
#include <QString>

#include <opencv2/core.hpp>

#include <algorithm>
#include <filesystem>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>

namespace denso::ui {

InferenceEngine* EngineRegistry::get(const std::string& filename) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = engines_.find(filename);
    if (it == engines_.end()) {
        auto eng = std::make_unique<BackendEngine>(models_dir_ + "/" + filename,
                                                   cache_dir_);
        it = engines_.emplace(filename, std::move(eng)).first;
    }
    BackendEngine* e = it->second.get();
    return (e && e->ok()) ? e : nullptr;
}

void EngineRegistry::warm_up(std::function<void(const std::string&)> on_model,
                             std::function<void(const std::string&)> on_ready) {
    namespace fs = std::filesystem;
    std::error_code ec;
    std::set<std::string> warmed;  // model files successfully loaded + warmed
    // A missing models dir isn't fatal on its own — it's only a problem if a
    // configured camera needs a model (checked against `required_` below). So we
    // guard the scan rather than early-return, letting the fail-loud check run.
    if (fs::is_directory(models_dir_, ec)) {
    // The TensorRT EP writes/reads its prebuilt engines here — create it up front
    // so the first-run build has somewhere to cache to.
    fs::create_directories(cache_dir_, ec);
    // A black frame exercises the full letterbox → session Run → decode path,
    // which is what triggers the TensorRT engine build (first run) or its load
    // (subsequent runs), plus CUDA kernel init. infer() letterboxes any size to
    // the model input, so the exact size only needs to be non-empty.
    const cv::Mat blank = cv::Mat::zeros(640, 640, CV_8UC3);
    for (const auto& entry : fs::directory_iterator(models_dir_, ec)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        std::string ext = entry.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return std::tolower(c); });
#ifdef _WIN32
        constexpr const char* kModelExt = ".onnx";  // ORT loads .onnx
#else
        constexpr const char* kModelExt = ".engine";  // native TRT loads prebuilt .engine
#endif
        if (ext != kModelExt) {
            continue;
        }
        const std::string filename = entry.path().filename().string();
        const QString name = QString::fromStdString(filename);
        if (on_model) {
            on_model(filename);
        }
#ifdef _WIN32
        // First run per model builds the TensorRT engine — minutes-long — then
        // caches it; later runs just load. Log around it so the freeze isn't
        // mistaken for a hang.
        qInfo().noquote() << "[warmup] preparing" << name
                          << "(first run builds the TensorRT engine — may take minutes)";
#else
        // Native TRT: deserialize the prebuilt engine + warm CUDA kernels (fast).
        qInfo().noquote() << "[warmup] preparing" << name << "(loading prebuilt engine)";
#endif
        if (InferenceEngine* e = get(filename)) {
            e->infer(blank);  // build/load the engine + warm kernels; result discarded
            qInfo().noquote() << "[warmup] ready" << name;
            warmed.insert(filename);
            if (on_ready) {
                on_ready(filename);
            }
        } else {
            qWarning().noquote() << "[warmup] failed to load" << name;
        }
    }
    }  // end if (is_directory)

    // Fail loud, honoring the engine-only/no-fallback contract: every model a
    // configured camera needs must have loaded + warmed. A missing models dir or
    // an absent/failed engine file would otherwise silently demote that camera to
    // no-detection (Windows) or throw from get() on the GUI thread later (Jetson).
    // Throwing here routes to WarmupWorker → app.exit(1), the intended abort.
    const std::vector<std::string> missing =
        missing_required_models(required_, warmed);
    if (!missing.empty()) {
        std::string joined;
        for (const std::string& m : missing) {
            joined += (joined.empty() ? "" : ", ") + m;
        }
        throw std::runtime_error(
            "required detection model(s) missing or failed to load: " + joined);
    }
}

} // namespace denso::ui
