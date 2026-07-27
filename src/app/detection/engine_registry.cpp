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
namespace {

// The production factory: build the platform's backend engine and keep it ONLY
// if it loaded (ok()). A failed load returns nullptr, which get() surfaces as
// "did not load" — the exact contract the registry had before the factory seam.
std::unique_ptr<InferenceEngine> default_factory(const std::string& path,
                                                 const std::string& cache_dir) {
    auto e = std::make_unique<BackendEngine>(path, cache_dir);
    if (e && e->ok()) return e;  // implicit upcast unique_ptr<BackendEngine> -> base
    return nullptr;
}

}  // namespace

EngineRegistry::EngineRegistry(std::string models_dir, std::string cache_dir,
                               std::set<std::string> allow_list,
                               std::vector<std::string> required,
                               EngineFactory factory)
    : models_dir_(std::move(models_dir)),
      cache_dir_(std::move(cache_dir)),
      allow_list_(std::move(allow_list)),
      required_(std::move(required)),
      factory_(factory ? std::move(factory) : EngineFactory(&default_factory)) {}

InferenceEngine* EngineRegistry::get(const std::string& filename) {
    // Firewall. A request for a filename the compatibility policy did not allow is
    // a PROGRAMMING error: warm_up() skips such files and every runtime caller
    // resolves through the same policy, so reaching here with a rejected filename
    // means a caller bypassed the allow-list. Fail loud rather than silently
    // deserialize a rejected (e.g. wrong-mode) plan. Checked before the lock — the
    // allow-list is immutable after construction. Survivable at the call site:
    // start_one() already wraps get() in a try/catch (camera_grid.cpp).
    if (allow_list_.find(filename) == allow_list_.end()) {
        throw std::logic_error(
            "EngineRegistry::get() called for a model outside the compatibility "
            "allow-list: " + filename);
    }
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = engines_.find(filename);
    if (it == engines_.end()) {
        it = engines_.emplace(filename,
                              factory_(models_dir_ + "/" + filename, cache_dir_))
                 .first;
    }
    return it->second.get();  // nullptr if the factory reported a failed load
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
        // Firewall: skip every file the compatibility policy did not allow. A
        // rejected model is never handed to get(), so no incompatible plan is
        // deserialized and no rejected engine runs a warm-up inference — including
        // an idle wrong-mode artifact merely present on disk. One redaction-safe
        // informational line per skipped file (a filename is not a credential).
        if (allow_list_.find(filename) == allow_list_.end()) {
            qInfo().noquote() << "[warmup] skipping" << name
                              << "(not permitted by the compatibility policy in the "
                                 "current mode)";
            continue;
        }
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
