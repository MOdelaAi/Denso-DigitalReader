#include "ui/startup_mode.h"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;
using denso::ui::cold_start_needs_splash;

namespace {
// Unique temp workspace per test (models/ + models/trt_cache/); RAII cleanup.
struct TempDirs {
    fs::path root;
    fs::path models;
    fs::path cache;
    explicit TempDirs(const std::string& tag) {
        root = fs::temp_directory_path() / ("denso_startup_mode_" + tag);
        models = root / "models";
        cache = root / "models" / "trt_cache";
        fs::remove_all(root);
        fs::create_directories(models);
    }
    ~TempDirs() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }
    void touch(const fs::path& dir, const std::string& name) const {
        fs::create_directories(dir);
        std::ofstream(dir / name) << "x";
    }
};
}  // namespace

TEST_CASE("no models → not cold (nothing to warm)") {
    TempDirs t("no_models");
    REQUIRE_FALSE(cold_start_needs_splash(t.models.string(), t.cache.string()));
}

TEST_CASE("models present, no engine cache → cold (splash)") {
    TempDirs t("no_cache");
    t.touch(t.models, "denso.onnx");
    REQUIRE(cold_start_needs_splash(t.models.string(), t.cache.string()));
}

TEST_CASE("models present, engine cached → warm (no splash)") {
    TempDirs t("warm");
    t.touch(t.models, "denso.onnx");
    t.touch(t.cache,
            "TensorrtExecutionProvider_TRTKernel_graph_x_fp16_sm89.engine");
    REQUIRE_FALSE(cold_start_needs_splash(t.models.string(), t.cache.string()));
}

TEST_CASE("models present, only timing cache (no engine) → cold") {
    TempDirs t("timing_only");
    t.touch(t.models, "denso.onnx");
    t.touch(t.cache, "TensorrtExecutionProvider_cache_sm89.timing");
    REQUIRE(cold_start_needs_splash(t.models.string(), t.cache.string()));
}

TEST_CASE("onnx extension match is case-insensitive") {
    TempDirs t("case");
    t.touch(t.models, "DENSO.ONNX");
    REQUIRE(cold_start_needs_splash(t.models.string(), t.cache.string()));
}
