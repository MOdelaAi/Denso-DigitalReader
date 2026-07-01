# GPU detection setup (TensorRT / CUDA execution providers)

The detection runtime uses **ONNX Runtime 1.27 (GPU build)** with an execution-provider
fallback chain: **TensorRT → CUDA → CPU**. `OrtEngine` picks the first tier whose
session builds, so the same `denso.exe` runs GPU-accelerated where the runtime DLLs
are present and silently falls back to CPU where they are not.

ORT's provider DLLs pull in vendor runtimes that Windows must resolve **beside the
exe** (or on `PATH`). Those runtimes are large and machine/driver-specific, so they
are **not committed** — they are provisioned locally into `third_party/gpu_ep/`
(git-ignored) and copied next to `denso.exe` by a `POST_BUILD` step in
`src/app/CMakeLists.txt`.

## Prerequisites

- NVIDIA GPU + driver new enough for CUDA 12 (verify with `nvidia-smi`).
- **CUDA 12 Toolkit** installed and its `bin` on `PATH` (provides `cudart64_12.dll`,
  `cublas64_12.dll`, `cublasLt64_12.dll`). Verified with CUDA 12.6.
- **TensorRT 10.x** for CUDA 12, unzipped somewhere (e.g. `D:\src\TensorRT-10.4.0.26`).
  The DLLs live in its `lib/` (not `bin/`).

## Provision `third_party/gpu_ep/`

Copy these DLLs into `third_party/gpu_ep/` (create the dir if missing):

**From TensorRT `lib/`:**
- `nvinfer_10.dll`
- `nvinfer_plugin_10.dll`
- `nvinfer_builder_resource_10.dll`  (large — required for engine building on first inference)
- `nvonnxparser_10.dll`

**cuDNN 9 (`cudnn*64_9.dll`)** — easiest source is the pip wheel (no NVIDIA login):

```bash
python -m pip install nvidia-cudnn-cu12          # cuDNN 9.x for CUDA 12
# DLLs land in:  <python>/Lib/site-packages/nvidia/cudnn/bin/cudnn*64_9.dll
```

Copy every `cudnn*64_9.dll` from that `bin/` (there are ~10: `cudnn64_9.dll` plus
`cudnn_graph64_9.dll`, `cudnn_ops64_9.dll`, `cudnn_engines_*64_9.dll`,
`cudnn_heuristic64_9.dll`, `cudnn_adv64_9.dll`, `cudnn_cnn64_9.dll`,
`cudnn_ext64_9.dll`).

`cudart`/`cublas` are intentionally **not** staged here — they come from the CUDA
toolkit on `PATH`.

## Build & verify

```bash
export PATH=/c/msys64/ucrt64/bin:$PATH
cmake -S . -B build -G Ninja      # configure logs a notice if gpu_ep/ is empty
cmake --build build               # POST_BUILD copies gpu_ep/*.dll beside denso.exe
```

After building, `build/src/app/` should contain the 4 ONNX Runtime DLLs **and** the
14 provisioned GPU DLLs. If `third_party/gpu_ep/` is empty the build still succeeds
and detection runs on CPU (a `STATUS` message says so at configure time).

To confirm the GPU tiers construct on this machine, the `OrtEngine` constructor logs
`[ort] loaded ... tier N` (0 = TensorRT, 1 = CUDA, 2 = CPU) via the app's message
handler. A missing-DLL failure shows up in the ORT log as
`Error loading "...providers_tensorrt.dll" which depends on "nvinfer_10.dll" which is missing`
(or the `cudnn64_9.dll` equivalent) followed by a fall-through to the next tier.
