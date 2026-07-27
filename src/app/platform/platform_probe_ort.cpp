// Platform probe — Windows / MSYS2 (ONNX Runtime). Compiled only on the Windows
// (OnnxRuntime) build, next to ort_engine.cpp. The ORT resolution path NEVER reads
// PlatformInfo (spec §3.2.1 rule 2 — built_for is TensorRT-local), so there is no
// qualified device to measure here: the probe reports nullopt. The shared seam
// turns that into an empty PlatformInfo, which the ORT provenance check ignores, so
// ONNX Runtime deployments resolve normally. Kept as a genuine per-platform TU (not
// a stub baked into the shared file) so the real Jetson probe is the only place
// TensorRT/CUDA headers are touched.
#include "platform/platform_info.h"

#include <optional>

namespace denso::platform {

std::optional<RawPlatform> probe_running_platform() {
    return std::nullopt;
}

}  // namespace denso::platform
