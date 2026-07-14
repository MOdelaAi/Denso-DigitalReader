# Denso Native-TensorRT (Engine-Only) on Jetson — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make Denso-DigitalReader build and run on a Jetson Orin Nano by replacing ONNX Runtime with native TensorRT that loads a pre-built `.engine` only (no runtime build, no silent fallback), at batch=1.

**Architecture:** Keep the existing `InferenceEngine` interface, OpenCV preprocessing, and the app's own `decode_yolo`/NMS + DB class-names path — they are backend-agnostic. Swap only the concrete engine: a new `TrtEngine` deserializes a `.engine` via `nvinfer1::IRuntime`, runs one image per `IExecutionContext` (one context+stream+buffers per camera; the `ICudaEngine` is shared), and reuses the existing letterbox + decode. CMake gains a platform split: Windows keeps ORT (unchanged), Linux uses TensorRT + CUDA. Missing/incompatible/unwarmable engines abort startup.

**Tech Stack:** C++20, Qt6 Widgets/Sql, OpenCV 4.8, TensorRT 10.3, CUDA 12.6, CMake ≥3.21, Catch2 v3. Dev/build on Jetson (aarch64, gcc 11.4, JetPack 6.2); Windows/MSYS2 build stays ORT-based and must keep compiling.

## Global Constraints

- **Target device:** Jetson Orin Nano, JetPack 6.2 / L4T R36.5, CUDA 12.6, TensorRT **10.3.0.30**, GPU **sm_87**. Engines are built by the user on-device with `trtexec` (present at `/usr/src/tensorrt/bin/trtexec`); the app NEVER builds one.
- **Engine export profile:** `min=1 / opt=4 / max=4` dynamic batch (real deployment is always 4 cameras). Phase A/B run batch=1 (a legal shape within that profile); batching is Phase C.
- **No silent fallback:** on Linux there is no CUDA/CPU/ONNX fallback. A missing, version-incompatible, or unwarmable engine must abort startup with a clear message (engine path + underlying error) and prevent ALL camera threads from starting.
- **Windows build must keep working:** all Linux changes are `if(WIN32)/else()` gated or `#ifdef`-guarded. The existing 184-test Catch2 suite must stay green on the dev PC.
- **No new fallback engine keys:** remove `fallback_*` engine behavior for the Linux/TRT path.
- **Dev workflow:** edit on the Windows dev PC (`d:/workspace/Denso-DigitalReader`), commit/push, `git pull` on the Jetson (`modela@192.168.1.15`, cloned at `~/Denso-DigitalReader`), build + smoke-test on-device. Build: `cmake -S . -B build -G Ninja` (or Unix Makefiles) `&& cmake --build build`.

---

## File Structure

**New files:**
- `src/app/ui/camera/shared/detection/trt_engine.h` — `TrtEngine : public InferenceEngine`; owns a shared `ICudaEngine` handle + a per-instance `IExecutionContext`, CUDA stream, and device/host buffers.
- `src/app/ui/camera/shared/detection/trt_engine.cpp` — deserialize, validate, preprocess (reuse letterbox), enqueue, decode (reuse yolo_decode), warmup.
- `src/app/ui/camera/shared/detection/trt_logger.h` — a small `nvinfer1::ILogger` that routes TRT messages to the app log.
- `src/app/ui/camera/shared/detection/class_names_sidecar.h` / `.cpp` — pure parser: read `<model>.names.json` next to an engine into `std::vector<std::string>`.
- `tests/test_class_names_sidecar.cpp` — Catch2 for the sidecar parser (pure, unit-testable).
- `cmake/FindTensorRT.cmake` — locate `nvinfer` headers/libs on Linux (or inline in CMakeLists; see Task A1).

**Modified files:**
- `CMakeLists.txt` — platform split: Windows→ORT (unchanged), Linux→TensorRT+CUDA; gate the ORT imported target to `WIN32`.
- `src/app/CMakeLists.txt` — link `onnxruntime` (Win) vs `nvinfer`+`CUDA::cudart` (Linux); compile `ort_engine.cpp` only on Win, `trt_engine.cpp` only on Linux; drop ORT DLL-copy steps on Linux.
- `src/app/ui/camera/shared/detection/engine_registry.h/.cpp` — hold `InferenceEngine` (not `OrtEngine`) and construct the platform engine; scan `*.engine` (Linux) vs `*.onnx` (Win) in `warm_up`.
- `src/app/ui/camera/shared/detection/model_sync.cpp` — class-name source: `.onnx` metadata (Win) vs sidecar json (Linux).
- `src/app/ui/startup_mode.cpp` — `cold_start_needs_splash` must not treat a shipped `.engine` as "already warm".
- `tests/CMakeLists.txt` — add `test_class_names_sidecar.cpp` + the sidecar source.

---

## Phase A — Linux buildability (app runs on Jetson, inference stubbed)

Goal: the whole Qt/DB/camera/UI app configures, links, and launches on the Jetson with a **stub** engine (returns no detections), isolating the port from the TRT work.

### Task A1: CMake platform split — gate ORT to Windows, wire TensorRT+CUDA on Linux

**Files:**
- Modify: `CMakeLists.txt:18-27` (the ORT imported-target block)
- Modify: `src/app/CMakeLists.txt` (link + DLL-copy sections)

**Interfaces:**
- Produces: on Linux, an imported/known set of link targets — the variables `TRT_LIBS` (`nvinfer`) and `CUDA::cudart` — for `src/app` to link. On Windows, the existing `onnxruntime` target is unchanged.

- [ ] **Step 1: Gate the ORT block in top `CMakeLists.txt`.** Wrap the existing ORT imported target so it only exists on Windows, and set up TRT/CUDA on Linux:

```cmake
if(WIN32)
    # ONNX Runtime (GPU build) — Windows only. (existing block, unchanged)
    set(ORT_DIR ${CMAKE_SOURCE_DIR}/third_party/onnxruntime)
    add_library(onnxruntime SHARED IMPORTED GLOBAL)
    set_target_properties(onnxruntime PROPERTIES
        IMPORTED_LOCATION ${ORT_DIR}/lib/onnxruntime.dll
        IMPORTED_IMPLIB   ${ORT_DIR}/lib/onnxruntime.dll
        INTERFACE_INCLUDE_DIRECTORIES ${ORT_DIR}/include)
else()
    # Native TensorRT + CUDA (Jetson). Engines are prebuilt; we link the
    # runtime only (no nvonnxparser needed at runtime).
    find_package(CUDAToolkit REQUIRED)              # provides CUDA::cudart
    find_library(TRT_NVINFER nvinfer REQUIRED)
    find_path(TRT_INCLUDE_DIR NvInfer.h
        PATHS /usr/include/aarch64-linux-gnu /usr/include)
    if(NOT TRT_INCLUDE_DIR)
        message(FATAL_ERROR "NvInfer.h not found — install libnvinfer-headers-dev")
    endif()
endif()
```

- [ ] **Step 2: Split link + sources in `src/app/CMakeLists.txt`.** Link ORT on Win, TRT/CUDA on Linux; compile the matching engine source:

```cmake
if(WIN32)
    target_link_libraries(denso PRIVATE onnxruntime)
    target_sources(denso PRIVATE ui/camera/shared/detection/ort_engine.cpp)
else()
    target_link_libraries(denso PRIVATE ${TRT_NVINFER} CUDA::cudart)
    target_include_directories(denso PRIVATE ${TRT_INCLUDE_DIR})
    target_sources(denso PRIVATE ui/camera/shared/detection/trt_engine.cpp)
endif()
```
(Remove `ort_engine.cpp` from any unconditional `target_sources`/glob so it is Windows-only.)

- [ ] **Step 3: Guard the ORT DLL-copy `add_custom_command` blocks** (`src/app/CMakeLists.txt:88-118`) inside `if(WIN32)` so Linux doesn't try to stage `.dll`s.

- [ ] **Step 4: Configure on the Jetson to verify the split.**

Run (on Jetson): `cd ~/Denso-DigitalReader && cmake -S . -B build -G "Unix Makefiles"`
Expected: configures past Qt6/OpenCV/CUDA/TensorRT discovery with no ORT error. It will still FAIL to build later because `trt_engine.cpp` doesn't exist yet — that's Task A2.

- [ ] **Step 5: Commit.**

```bash
git add CMakeLists.txt src/app/CMakeLists.txt
git commit -m "build: platform-split inference backend (ORT on Windows, TensorRT on Linux)"
```

### Task A2: Stub `TrtEngine` so the app builds + launches on Jetson

**Files:**
- Create: `src/app/ui/camera/shared/detection/trt_engine.h`
- Create: `src/app/ui/camera/shared/detection/trt_engine.cpp`
- Modify: `src/app/ui/camera/shared/detection/engine_registry.h:42` (member type), `engine_registry.cpp:14-24` (construct platform engine)

**Interfaces:**
- Consumes: `InferenceEngine` (`inference_engine.h:20`, virtual `std::vector<Detection> infer(const cv::Mat& bgr)` + `class_names()`).
- Produces: `class TrtEngine : public InferenceEngine` with ctor `TrtEngine(const std::filesystem::path& engine_path)` and `class_names()` returning `const std::vector<std::string>&`. In Phase A `infer()` returns `{}`.

- [ ] **Step 1: Write the stub header** `trt_engine.h`:

```cpp
#pragma once
#include "ui/camera/shared/detection/inference_engine.h"
#include <filesystem>
#include <string>
#include <vector>

namespace denso::ui {

// Phase A: compile-only stub so the Linux app builds + runs without inference.
// Phase B replaces the body with real TensorRT.
class TrtEngine : public InferenceEngine {
public:
    explicit TrtEngine(const std::filesystem::path& engine_path);
    std::vector<Detection> infer(const cv::Mat& bgr) override;
    const std::vector<std::string>& class_names() const override { return names_; }
private:
    std::vector<std::string> names_;
};

} // namespace denso::ui
```

- [ ] **Step 2: Write the stub impl** `trt_engine.cpp`:

```cpp
#include "ui/camera/shared/detection/trt_engine.h"

namespace denso::ui {
TrtEngine::TrtEngine(const std::filesystem::path&) {}
std::vector<Detection> TrtEngine::infer(const cv::Mat&) { return {}; }
} // namespace denso::ui
```

- [ ] **Step 3: Make `EngineRegistry` hold `InferenceEngine` and build the platform engine.** In `engine_registry.h:42` change the map value to `std::unique_ptr<InferenceEngine>`. In `engine_registry.cpp` `get()`:

```cpp
#ifdef _WIN32
    auto eng = std::make_unique<OrtEngine>(models_dir_ / filename);
#else
    auto eng = std::make_unique<TrtEngine>(models_dir_ / filename);
#endif
```
Add the matching `#include` for each engine under the same guards.

- [ ] **Step 4: Build + launch on Jetson.**

Run (Jetson): `cmake --build build -j6 && ./build/src/app/denso` (with a display attached / `DISPLAY=:0`).
Expected: app compiles, links against `nvinfer`+`cudart`, window opens, camera tiles show (no detections). Existing dev-PC Windows build still compiles (ORT path untouched).

- [ ] **Step 5: Commit.**

```bash
git add src/app/ui/camera/shared/detection/trt_engine.* src/app/ui/camera/shared/detection/engine_registry.*
git commit -m "feat(linux): stub TrtEngine so the app builds and runs on Jetson"
```

---

## Phase B — Native `TrtEngine` (batch=1), engine-only, fail-loud

Goal: real detection on-device using a user-built `.engine`, one image per inference, one execution context per camera, no fallback.

### Task B1: Class-name sidecar parser (pure, TDD)

**Files:**
- Create: `src/app/ui/camera/shared/detection/class_names_sidecar.h/.cpp`
- Test: `tests/test_class_names_sidecar.cpp`; Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Produces: `std::optional<std::vector<std::string>> denso::ui::read_names_sidecar(const std::filesystem::path& engine_path)` — reads `<engine dir>/<stem>.names.json` (a JSON array of strings) → names; `std::nullopt` if the file is absent; throws `std::runtime_error` on malformed JSON.

- [ ] **Step 1: Write the failing test** `tests/test_class_names_sidecar.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>
#include "ui/camera/shared/detection/class_names_sidecar.h"
#include <fstream>
using denso::ui::read_names_sidecar;

TEST_CASE("sidecar reads a JSON array of class names", "[sidecar]") {
    const auto dir = std::filesystem::temp_directory_path();
    const auto engine = dir / "m.engine";
    std::ofstream(dir / "m.names.json") << R"(["0","1","2","3"])";
    auto names = read_names_sidecar(engine);
    REQUIRE(names.has_value());
    REQUIRE(names->size() == 4);
    CHECK((*names)[2] == "2");
}

TEST_CASE("sidecar returns nullopt when the file is absent", "[sidecar]") {
    CHECK_FALSE(read_names_sidecar("/no/such/x.engine").has_value());
}
```

- [ ] **Step 2: Register the test + source** in `tests/CMakeLists.txt` (add `test_class_names_sidecar.cpp` and `${CMAKE_SOURCE_DIR}/src/app/ui/camera/shared/detection/class_names_sidecar.cpp`).

- [ ] **Step 3: Run it, watch it fail.** Run (dev PC): `ctest --test-dir build -R sidecar` → FAIL (no such header/function).

- [ ] **Step 4: Implement** `class_names_sidecar.{h,cpp}` — parse the JSON string array (a minimal hand parser or Qt's `QJsonDocument`, which is available). Header declares `read_names_sidecar`; cpp reads the file next to the engine (`engine_path` with extension replaced by `.names.json`), returns `nullopt` if missing, parses `["a","b",...]`.

- [ ] **Step 5: Run, watch it pass.** Run: `ctest --test-dir build -R sidecar` → PASS. Full suite still green.

- [ ] **Step 6: Commit.**
```bash
git add src/app/ui/camera/shared/detection/class_names_sidecar.* tests/test_class_names_sidecar.cpp tests/CMakeLists.txt
git commit -m "feat(detection): class-name sidecar (<engine>.names.json) parser"
```

### Task B2: `TrtLogger` + engine deserialize with validation (fail-loud)

**Files:**
- Create: `src/app/ui/camera/shared/detection/trt_logger.h`
- Modify: `trt_engine.h/.cpp`

**Interfaces:**
- Consumes: `read_names_sidecar` (B1), `letterbox` (`shared/detection/letterbox.h`), `decode_yolo`/`decode_yolo_end2end` (`shared/detection/yolo_decode.h`).
- Produces: `TrtEngine` that, on construction, deserializes the engine and THROWS `std::runtime_error` (path + reason) if the file is missing, deserialization fails, the engine has no batch-1-capable profile, or the sidecar names are absent. Getter `class_names()` returns the sidecar names.

- [ ] **Step 1: Write `trt_logger.h`** — an `nvinfer1::ILogger` whose `log()` forwards `kERROR`/`kWARNING` to the app log stream (reuse whatever `ort_engine.cpp` logs to), suppressing `kINFO`/`kVERBOSE`.

- [ ] **Step 2: Implement the real `TrtEngine` ctor** in `trt_engine.cpp`:
  - `if (!std::filesystem::exists(engine_path)) throw std::runtime_error("engine not found: " + path)`.
  - Read the file bytes; `runtime_ = createInferRuntime(logger)`; `engine_ = runtime_->deserializeCudaEngine(bytes.data(), bytes.size())`; throw if null (message: rebuild on-device for TRT 10.3 / sm_87).
  - Query IO tensor names/shapes; validate exactly one input `[?,3,640,640]` and one output; record the output shape to pick the decoder. Validate the input profile admits batch=1 (min dim ≤1≤ max); else throw.
  - `names_ = read_names_sidecar(engine_path).value_or_throw(...)` — engine-only deployments require the sidecar; throw if absent.
  - Create `context_ = engine_->createExecutionContext()`, a `cudaStream_t`, and device buffers for input+output (sized for batch=1 at the 640×640 input and the output volume).

- [ ] **Step 3: Build on Jetson; smoke the fail-loud paths.**
  Run (Jetson): point config at a missing engine → app aborts at startup with the path in the message (NOT a silent orientation-only fallback). Then a garbage file → deserialize error surfaced.
  (No unit test: this is GPU/engine integration, validated by on-device smoke. Record the console output in the commit message.)

- [ ] **Step 4: Commit.**
```bash
git add src/app/ui/camera/shared/detection/trt_logger.h src/app/ui/camera/shared/detection/trt_engine.*
git commit -m "feat(trt): deserialize + validate engine, fail loud (no fallback)"
```

### Task B3: `infer()` — preprocess, enqueue batch=1, decode (reuse existing)

**Files:** Modify `trt_engine.cpp`.

**Interfaces:**
- Produces: `std::vector<Detection> TrtEngine::infer(const cv::Mat& bgr)` matching `OrtEngine::infer` semantics (same letterbox, same NCHW float [0,1], same decode branch on output shape, same conf/NMS constants `0.25`/`0.45`).

- [ ] **Step 1: Implement `infer()`**:
  - Reuse `letterbox(bgr, 640)` → `cvtColor(BGR2RGB)` → `blobFromImage(1/255)` producing the same NCHW blob `ort_engine.cpp:108-121` builds.
  - Set input shape `{1,3,640,640}` on the context (`setInputShape`); `cudaMemcpyAsync` H2D; `context_->enqueueV3(stream_)`; `cudaMemcpyAsync` D2H; `cudaStreamSynchronize`.
  - Feed the host output pointer + recorded shape into the SAME decode branch as ORT (`shape[2]==6 → decode_yolo_end2end` else `decode_yolo` with `num_classes=shape[1]-4`).
  - Thread-safety: each `TrtEngine` instance owns its own context/stream/buffers, so per-camera instances are independent. (EngineRegistry sharing is revisited in Step 3.)

- [ ] **Step 2: On-device parity smoke.**
  Run (Jetson): with a user-built `no-helmet` or digit engine + its `.names.json`, launch the app on a live/looped camera and confirm boxes+labels appear and read the same digits the ORT/Windows build produces on the same frame. Record before/after.

- [ ] **Step 3: Decide engine sharing vs per-camera context.** The current `EngineRegistry` shares ONE engine object across cameras; a TRT `IExecutionContext` is not thread-safe. Change `EngineRegistry` to cache the shared `ICudaEngine` per filename but hand each camera its OWN `TrtEngine` wrapper (context+stream+buffers). Concretely: split `TrtEngine` into a shared `TrtModel` (owns `ICudaEngine`, ref-counted in the registry) and a per-camera `TrtEngine` (owns context/stream/buffers, holds a `shared_ptr<TrtModel>`). Update `engine_registry.*` to return a fresh per-camera `TrtEngine` bound to the cached `TrtModel`.

- [ ] **Step 4: Commit.**
```bash
git add src/app/ui/camera/shared/detection/trt_engine.* src/app/ui/camera/shared/detection/engine_registry.*
git commit -m "feat(trt): batch=1 infer via execution context; per-camera context over shared engine"
```

### Task B4: Warmup + `EngineRegistry`/`model_sync`/`startup_mode` wiring

**Files:** Modify `trt_engine.cpp` (warmup), `engine_registry.cpp` (scan `.engine`), `model_sync.cpp` (names from sidecar on Linux), `startup_mode.cpp`.

**Interfaces:**
- Produces: `TrtEngine::warmup(int)` (override) running N blank `infer()`s; `EngineRegistry::warm_up` scanning `*.engine` on Linux; `model_sync` populating DB class names from sidecars on Linux; `cold_start_needs_splash` returning true whenever any configured engine still needs deserialize+warmup this launch.

- [ ] **Step 1: Implement `TrtEngine::warmup(num_runs)`** — loop `infer(cv::Mat::zeros(640,640,CV_8UC3))` num_runs times (mirrors `ort_engine`/`engine.py` warmup); discard results.

- [ ] **Step 2: `EngineRegistry::warm_up`** — on Linux iterate `*.engine` (not `*.onnx`) in `models_dir_`; construction already fails loud, so a bad engine aborts warmup → aborts startup (replace today's "skip model, orientation-only" at `camera_grid.cpp:165`).

- [ ] **Step 3: `model_sync.cpp`** — under `#ifndef _WIN32`, source class names from `read_names_sidecar` instead of `OrtEngine::read_names` (which needs ORT). Keep the DB the single runtime source of names.

- [ ] **Step 4: `startup_mode.cpp:34` `cold_start_needs_splash`** — it currently returns true only when NO `*.engine` exists in the cache dir (an ORT-cache heuristic). For prebuilt engines that's always "warm". Replace with: true when any configured model exists and warmup has not completed this process launch (i.e., always show the splash until warmup finishes, since deserialize+warm still takes real time). Keep it pure/testable if feasible.

- [ ] **Step 5: On-device full smoke.** Launch: splash shows during warmup, each camera starts as its engine warms, detection runs on all 4; kill an engine file → startup aborts with a clear error. Confirm Windows dev-PC build + full Catch2 suite still green.

- [ ] **Step 6: Commit.**
```bash
git add src/app/ui/camera/shared/detection/trt_engine.* src/app/ui/camera/shared/detection/engine_registry.* src/app/ui/camera/shared/detection/model_sync.cpp src/app/ui/startup_mode.cpp
git commit -m "feat(trt): warmup + engine-scan + sidecar names + startup-splash for prebuilt engines"
```

---

## Out of scope (separate future plan)

**Phase C — fixed-4 cross-camera batching.** An inference coordinator owning one context/stream/buffers; each camera publishes its newest preprocessed frame into a fixed slot; a batch-4 `enqueueV3` runs when all slots are fresh or after a 33–66 ms deadline; results route back by slot. Requires a batched decode (today `decode_yolo` assumes `shape[0]==1`). Gated by an on-device benchmark (batch-4 vs 4×batch-1: GPU time, frame age, memory, desktop/decode stability). Re-export the engine `opt=4`. **Do not start until Phase B is measured.**

## Test Strategy

- **Pure logic → Catch2 on the dev PC:** class-name sidecar (B1); any batched-decode helper (Phase C). Keep the 184-test suite green throughout — every commit runs it on Windows.
- **TRT/CUDA integration → on-device smoke:** deserialize/validate/fail-loud (B2), infer parity (B3), warmup+startup (B4). These can't be unit-tested without a GPU + engine; each task records the on-device console evidence in its commit. This is a deliberate, noted exception to unit-level TDD for GPU integration code.

## Self-Review notes

- Spec coverage: engine-only ✓(B2), no runtime build ✓(A1 drops parser/ORT), no fallback ✓(B2/B4), dynamic batch profile ✓(Global Constraints; batch=1 runs, opt=4 exported), native TRT replaces ORT ✓(A1/A2/B), class names without ORT ✓(B1/B4), startup splash ✓(B4), Windows unaffected ✓(all gated).
- Types consistent: `InferenceEngine`/`TrtEngine`/`TrtModel`/`read_names_sidecar` used identically across tasks.
- Known live-iteration points (validated on-device, not fictionalised here): exact TRT 10.3 tensor-name/`enqueueV3` API calls and buffer sizing in B2/B3.
