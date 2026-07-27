// Platform-information provider — Release B, Slice 7.
//
// The ONE production seam that measures the running platform and normalizes it to
// the manifest `built_for` representation, so the TensorRT provenance check
// (spec §3.2.1 rule 2) corroborates against the ACTUAL device rather than a
// hand-typed constant. Both startup.cpp and run_headless.cpp obtain their
// denso::models::PlatformInfo from here — probing and normalization are defined
// exactly ONCE.
//
// This translation unit lives in the application layer (denso_app), NOT in
// denso_core: denso_core must never probe CUDA/TensorRT, shell out, or inspect the
// device (it takes PlatformInfo as caller-supplied data — model_identity.h). The
// real probe is per-platform, selected by CMake exactly like the inference engine
// backend (OrtEngine / TrtEngine): the Jetson probe reads the TensorRT runtime
// version + the CUDA Runtime API; the Windows probe has no qualified device to
// measure and reports nullopt (harmless — ONNX Runtime never reads PlatformInfo).
//
// Fail-closed by construction: a malformed or unavailable measurement yields
// nullopt, and the shared production entry point substitutes an EMPTY PlatformInfo
// (never the qualified constants). An empty platform corroborates no TensorRT
// built_for (manifest validation guarantees built_for is non-empty), so every
// engine is excluded from the warm-up allow-list and required set — nothing is
// deserialized. On ONNX Runtime the field is ignored downstream, so empty is safe.
#pragma once

#include "models/model_identity.h"  // denso::models::PlatformInfo

#include <functional>
#include <optional>
#include <string>
#include <utility>

namespace denso::platform {

// ── Pure normalization to the manifest built_for representation ────────────────
// One implementation, shared by both call sites, GPU-free and unit-testable.
// Each returns nullopt on malformed / missing / out-of-range input (fail closed),
// so a bad measurement can never be turned into a valid-looking version string.

/// TensorRT major.minor → "major.minor". (10,3) → "10.3"; (8,6) → "8.6".
/// Rejects a non-positive major or a negative minor.
std::optional<std::string> normalize_trt(int major, int minor);

/// CUDA Runtime API version (cudaRuntimeGetVersion form: 1000*major + 10*minor)
/// → "major.minor". 12060 → "12.6"; 12040 → "12.4". Rejects a non-positive or
/// structurally impossible (major == 0) version.
std::optional<std::string> normalize_cuda(int runtime_version);

/// CUDA compute capability major/minor → concatenated "majorminor".
/// (8,7) → "87"; (7,5) → "75". Rejects a non-positive major or a negative minor.
std::optional<std::string> normalize_sm(int major, int minor);

/// Decode getInferLibVersion() (the RUNNING TensorRT library version) to
/// (major, minor). The encoding is (MAJOR*100 + MINOR)*100 + PATCH
/// (== MAJOR*10000 + MINOR*100 + PATCH), per NvInferRuntimeBase.h: 100300 → (10,3);
/// 80601 → (8,6). Rejects a non-positive or too-small (no major) version.
std::optional<std::pair<int, int>> decode_trt_lib_version(int lib_version);

// ── The probe seam ────────────────────────────────────────────────────────────

/// Raw measurements from a probe, kept separate from normalization so the probe
/// stays a thin seam and the arithmetic is testable without a device.
struct RawPlatform {
    int trt_major = 0;
    int trt_minor = 0;
    int cuda_runtime_version = 0;  // cudaRuntimeGetVersion form, e.g. 12060
    int compute_major = 0;
    int compute_minor = 0;
};

/// raw → PlatformInfo, or nullopt if ANY field is invalid (fail closed).
std::optional<denso::models::PlatformInfo> normalize(const RawPlatform& raw);

/// Measures the running platform, or nullopt when it cannot (no CUDA device,
/// query failure, unavailable version). Injectable for tests.
using Probe = std::function<std::optional<RawPlatform>()>;

/// The build's REAL probe. Per-platform, selected by CMake like the engine
/// backend: Jetson reads the TensorRT runtime version + the CUDA Runtime API;
/// Windows returns nullopt (no qualified device; ONNX Runtime ignores the field).
std::optional<RawPlatform> probe_running_platform();

/// Measured PlatformInfo, or nullopt on probe/normalization failure.
/// Backend-agnostic (always probe → normalize) so BOTH platforms' behaviour is
/// unit-testable off-host through an injected probe.
std::optional<denso::models::PlatformInfo> resolve_platform_info(const Probe& probe);
std::optional<denso::models::PlatformInfo> resolve_platform_info();

/// THE shared production seam both startup.cpp and run_headless.cpp call. Runs the
/// real probe; on failure it logs (only where the value matters — the TensorRt
/// backend) and returns a fail-closed EMPTY PlatformInfo. It NEVER substitutes the
/// qualified constants. An empty platform corroborates no TensorRT built_for, so
/// every engine is excluded from the warm-up allow-list and required set (nothing
/// is deserialized); on ONNX Runtime the field is ignored, so empty is harmless.
denso::models::PlatformInfo measured_platform_info();

}  // namespace denso::platform
