#include "ui/camera/shared/detection/engine_registry.h"

#include <QDebug>
#include <QString>

#include <opencv2/core.hpp>

#include <algorithm>
#include <filesystem>

namespace denso::ui {

InferenceEngine* EngineRegistry::get(const std::string& filename) {
    auto it = engines_.find(filename);
    if (it == engines_.end()) {
        auto eng = std::make_unique<OrtEngine>(models_dir_ + "/" + filename,
                                               cache_dir_);
        it = engines_.emplace(filename, std::move(eng)).first;
    }
    OrtEngine* e = it->second.get();
    return (e && e->ok()) ? e : nullptr;
}

void EngineRegistry::warm_up(std::function<void(const std::string&)> on_model) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::is_directory(models_dir_, ec)) {
        return;
    }
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
        if (ext != ".onnx") {
            continue;
        }
        const std::string filename = entry.path().filename().string();
        const QString name = QString::fromStdString(filename);
        if (on_model) {
            on_model(filename);
        }
        // First run per model builds the TensorRT engine — minutes-long — then
        // caches it; later runs just load. Log around it so the freeze isn't
        // mistaken for a hang.
        qInfo().noquote() << "[warmup] preparing" << name
                          << "(first run builds the TensorRT engine — may take minutes)";
        if (InferenceEngine* e = get(filename)) {
            e->infer(blank);  // build/load the engine + warm kernels; result discarded
            qInfo().noquote() << "[warmup] ready" << name;
        } else {
            qWarning().noquote() << "[warmup] failed to load" << name;
        }
    }
}

} // namespace denso::ui
