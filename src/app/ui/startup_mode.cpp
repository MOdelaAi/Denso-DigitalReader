#include "ui/startup_mode.h"

#include <algorithm>
#include <cctype>
#include <filesystem>

namespace denso::ui {
namespace {
namespace fs = std::filesystem;

std::string lower_ext(const fs::path& p) {
    std::string ext = p.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return ext;
}

// True if `dir` exists and holds ≥1 regular file whose lower-cased extension is
// `want_ext` (e.g. ".onnx", ".engine").
bool dir_has_ext(const std::string& dir, const std::string& want_ext) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) {
        return false;
    }
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (entry.is_regular_file(ec) && lower_ext(entry.path()) == want_ext) {
            return true;
        }
    }
    return false;
}
}  // namespace

bool cold_start_needs_splash(const std::string& models_dir,
                             const std::string& cache_dir) {
#ifdef _WIN32
    // Windows/ORT builds + caches the TensorRT engine on first run (minutes), so
    // splash only when we have models but no cached engine yet.
    const bool has_models = dir_has_ext(models_dir, ".onnx");
    const bool has_engine = dir_has_ext(cache_dir, ".engine");
    return has_models && !has_engine;
#else
    // Linux/Jetson: engines are PREBUILT (no multi-minute build), but
    // deserialize + warm-up still takes a few seconds per model, so show the
    // splash whenever there is an engine to prepare. The ORT-cache notion of
    // "already warm" does not apply.
    (void)cache_dir;
    return dir_has_ext(models_dir, ".engine");
#endif
}

}  // namespace denso::ui
