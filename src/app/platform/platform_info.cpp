// Platform-information provider — shared normalization + the production seam.
// The per-platform probe (probe_running_platform) lives in platform_probe_trt.cpp
// (Jetson) / platform_probe_ort.cpp (Windows), selected by CMake like the engine
// backend. This TU is backend-free: no CUDA, no TensorRT — only the pure
// arithmetic and the fail-closed composition, so it compiles identically on both
// platforms and is exhaustively unit-testable without a device.
#include "platform/platform_info.h"

#include "models/model_identity.h"  // active_backend, Backend, PlatformInfo

#include <QDebug>

#include <optional>
#include <string>
#include <utility>

namespace denso::platform {

std::optional<std::string> normalize_trt(int major, int minor) {
    if (major <= 0 || minor < 0) return std::nullopt;
    return std::to_string(major) + "." + std::to_string(minor);
}

std::optional<std::string> normalize_cuda(int runtime_version) {
    if (runtime_version <= 0) return std::nullopt;
    const int major = runtime_version / 1000;
    const int minor = (runtime_version % 1000) / 10;
    if (major <= 0) return std::nullopt;  // e.g. 999 → no major → malformed
    return std::to_string(major) + "." + std::to_string(minor);
}

std::optional<std::string> normalize_sm(int major, int minor) {
    if (major <= 0 || minor < 0) return std::nullopt;
    return std::to_string(major) + std::to_string(minor);
}

std::optional<std::pair<int, int>> decode_trt_lib_version(int lib_version) {
    // getInferLibVersion() encodes (MAJOR*100 + MINOR)*100 + PATCH, i.e.
    // MAJOR*10000 + MINOR*100 + PATCH — per NvInferRuntimeBase.h on the qualified
    // TensorRT 10.3 platform. 100300 → 10.3; 80601 → 8.6. A value too small to
    // carry a major (or non-positive) is rejected (fail closed).
    if (lib_version <= 0) return std::nullopt;
    const int major = lib_version / 10000;
    const int minor = (lib_version / 100) % 100;
    if (major <= 0) return std::nullopt;
    return std::make_pair(major, minor);
}

std::optional<denso::models::PlatformInfo> normalize(const RawPlatform& raw) {
    const auto trt = normalize_trt(raw.trt_major, raw.trt_minor);
    const auto cuda = normalize_cuda(raw.cuda_runtime_version);
    const auto sm = normalize_sm(raw.compute_major, raw.compute_minor);
    if (!trt || !cuda || !sm) return std::nullopt;  // any invalid → fail closed
    return denso::models::PlatformInfo{*trt, *cuda, *sm};
}

std::optional<denso::models::PlatformInfo> resolve_platform_info(const Probe& probe) {
    if (!probe) return std::nullopt;
    const auto raw = probe();
    if (!raw) return std::nullopt;
    return normalize(*raw);
}

std::optional<denso::models::PlatformInfo> resolve_platform_info() {
    return resolve_platform_info(Probe(&probe_running_platform));
}

denso::models::PlatformInfo measured_platform_info() {
    const std::optional<denso::models::PlatformInfo> measured = resolve_platform_info();
    if (measured) return *measured;

    // Fail closed. We do NOT substitute the qualified constants: an empty
    // PlatformInfo corroborates no TensorRT built_for (manifest validation
    // guarantees built_for is non-empty), so every engine is excluded from the
    // warm-up allow-list and required set — nothing incompatible/uncorroborated is
    // ever deserialized. The log fires only where the value is actually read
    // (TensorRt); on ONNX Runtime the empty value is ignored downstream, so a
    // (harmless) probe miss stays quiet.
    if (denso::models::active_backend() == denso::models::Backend::TensorRt) {
        qCritical().noquote()
            << "[platform] the running TensorRT/CUDA platform could not be measured;"
            << "detection engines cannot be corroborated and will not be loaded"
            << "(fail-closed)";
    }
    return denso::models::PlatformInfo{};
}

}  // namespace denso::platform
