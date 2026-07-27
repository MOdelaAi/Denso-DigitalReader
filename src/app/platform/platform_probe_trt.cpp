// Platform probe — Jetson (native TensorRT). Compiled only on the non-Windows
// (TensorRt) build, next to trt_engine.cpp. Measures the RUNNING platform:
//   * TensorRT: the loaded runtime library version (getInferLibVersion), decoded
//     — not a compile-time constant divorced from the installed stack.
//   * CUDA runtime version: cudaRuntimeGetVersion (the linked CUDA runtime).
//   * compute capability: cudaGetDeviceProperties(dev 0) (the real device).
// Any query failure, absent device, or malformed version returns nullopt, which
// the shared seam turns into a fail-closed empty PlatformInfo.
#include "platform/platform_info.h"

#include <NvInferRuntime.h>      // pulls NvInferRuntimeBase.h (getInferLibVersion)
#include <NvInferRuntimeBase.h>  // ::getInferLibVersion (extern "C", global scope)
#include <cuda_runtime_api.h>    // cudaRuntimeGetVersion / GetDeviceCount / GetDeviceProperties

#include <optional>

namespace denso::platform {

std::optional<RawPlatform> probe_running_platform() {
    RawPlatform r;

    // Running TensorRT library version — the runtime that will deserialize the
    // engine, not the header the binary happened to see. getInferLibVersion() is
    // declared extern "C" at global scope (NvInferRuntimeBase.h), not in nvinfer1.
    const auto trt_mm = decode_trt_lib_version(::getInferLibVersion());
    if (!trt_mm) return std::nullopt;
    r.trt_major = trt_mm->first;
    r.trt_minor = trt_mm->second;

    // CUDA runtime version (linked runtime).
    int cuda_rt = 0;
    if (cudaRuntimeGetVersion(&cuda_rt) != cudaSuccess) return std::nullopt;
    r.cuda_runtime_version = cuda_rt;

    // A usable CUDA device must exist.
    int device_count = 0;
    if (cudaGetDeviceCount(&device_count) != cudaSuccess || device_count < 1)
        return std::nullopt;

    // Compute capability of device 0 (the device the engine runs on).
    cudaDeviceProp prop{};
    if (cudaGetDeviceProperties(&prop, 0) != cudaSuccess) return std::nullopt;
    r.compute_major = prop.major;
    r.compute_minor = prop.minor;

    return r;
}

}  // namespace denso::platform
