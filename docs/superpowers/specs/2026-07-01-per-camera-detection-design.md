# Per-camera ONNX detection overlay — design

Date: 2026-07-01
Status: approved (brainstorming) — awaiting spec review before planning

## Goal

Run YOLOv8 object detection on the live camera grid and draw the results on each
tile. A camera attaches **1..N detection models**; for each attached model the
user picks **which classes to keep** and sets a **confidence threshold per
class**. A camera runs detection iff it has at least one attached model.

Inference runs on ONNX Runtime with an execution-provider fallback chain
(**TensorRT → CUDA → CPU**), so the same code path runs `.onnx` on CPU-only
machines and, on an NVIDIA box, lets the TensorRT EP build and cache a `.engine`
automatically. First target hardware: desktop PC (RTX 4070) and Jetson Orin
Nano.

This first slice is **overlay only** — boxes + `"<class> NN%"` labels on the
tile. No reading persistence, no left-to-right digit assembly.

## Context

The live grid was built with a purpose-made seam for exactly this. From
`src/app/ui/camera/grid/frame_processor.h`:

> Today the only processor is OrientationProcessor... When the detection model
> lands, it becomes another FrameProcessor — selected per camera by a config
> flag — and nothing in the capture loop or the tile has to change.

`CameraStream` reads frames on its own thread and runs each through its
`FrameProcessor` before emitting `frame_ready`. The detection path slots in as a
second `FrameProcessor` implementation; the capture loop and tile are untouched.

The model (`models/denso.pt`, a YOLOv8 digit detector, classes `0`–`9`) has been
exported to `models/denso.onnx` (opset 12) via the training venv at
`D:\workspace\test_model\train_venv`. Confirmed I/O:

- input `images`, shape `(1,3,640,640)`, float32, RGB, `/255`, letterboxed
- output `output0`, shape `(1,14,8400)` — `14 = 4 box (cx,cy,w,h) + 10 classes`;
  export used `nms=False`, so NMS happens in our code
- class names + `imgsz` live in the ONNX metadata (`names`, `imgsz`)

The Python reference (`D:\workspace\test_model\detection_core.py`) is a generic
YOLOv8 detect+draw loop — it draws one labeled box per detection and does **not**
assemble digits into a number. This slice ports that behavior.

## Naming convention (applies to this work)

Core domain type files are named after their module directory: `<module>/<module>.h`.

- `src/core/camera/model.h` → **`src/core/camera/camera.h`** (rename; 13 includes updated)
- module dir `src/core/processor/` → **`src/core/detection/`**
- **`src/core/detection/detection.h`** (types) + `detection/repo.{h,cpp}` (persistence)
- `settings/settings.h` already conforms; `hardware/` is stateless collectors (no single type file); `network/model.h` is left as-is (outside this feature).

## Inference runtime — feasibility (proven)

ONNX Runtime 1.27.0 GPU (`gpu_cuda12`) is installed at
`third_party/onnxruntime/` (`include/` + `lib/`). Verified on this toolchain:

- MinGW GCC 15.2 (MSYS2 UCRT64) compiles `onnxruntime_cxx_api.h` and links
  directly against the MSVC-built `onnxruntime.dll` (clean C ABI).
- At runtime the built exe loads and reports all three providers:
  `TensorrtExecutionProvider`, `CUDAExecutionProvider`, `CPUExecutionProvider`.
- Requirement: the ORT runtime DLLs must sit beside the exe.

This PC: RTX 4070 SUPER, CUDA 12.6, **TensorRT not installed** — so the TRT EP
registration fails and ORT falls through to the CUDA EP (still GPU-accelerated).
Installing TensorRT later enables the cached-`.engine` path with no code change.

## Domain — `src/core/detection/` (Qt/OpenCV-free, unit-tested)

`detection.h`, mirroring `camera.h`'s DB / Runtime split:

```cpp
namespace denso::detection {

// ─── DB (persisted) ───
struct DetectionModel {              // row in `model`
    int64_t id = 0;
    std::string name;                // display name (default: filename stem)
    std::string filename;            // "denso.onnx", resolved under models/
    std::vector<std::string> class_names;  // index == class_id (cached from onnx)
};

struct ModelClassSelection {         // row in `camera_model_class`
    int   class_id = 0;
    float conf     = 0.5f;
};

struct CameraModel {                 // row in `camera_model` (+ class children)
    int64_t id        = 0;
    int64_t camera_id = 0;           // FK → camera.id
    int64_t model_id  = 0;           // FK → model.id
    std::vector<ModelClassSelection> classes;
};

// ─── Runtime (transient — never stored) ───
struct ResolvedModel {               // one attached model resolved for inference
    std::string filename;                       // which shared engine to run
    std::vector<std::string> class_names;       // for drawing labels
    std::vector<ModelClassSelection> classes;   // selected class_ids + per-class conf
};

struct CameraDetection {             // analog of camera::CameraWithAreas
    int64_t camera_id = 0;
    std::vector<ResolvedModel> models;   // empty == no detection for this camera
};

}
```

### Tables (migrations in `db.cpp`, bump `SCHEMA_VERSION`)

| Table | Columns |
|---|---|
| `model` | `id INTEGER PK, name TEXT, filename TEXT UNIQUE NOT NULL, class_names TEXT` |
| `camera_model` | `id INTEGER PK, camera_id INTEGER NOT NULL, model_id INTEGER NOT NULL` — FK camera(id) ON DELETE CASCADE, FK model(id) |
| `camera_model_class` | `id INTEGER PK, camera_model_id INTEGER NOT NULL, class_id INTEGER NOT NULL, conf REAL NOT NULL` — FK camera_model(id) ON DELETE CASCADE |

`class_names` is serialized TEXT (reuse the codebase's existing point/JSON
serialization style, e.g. a small helper analogous to `area_points`). Core only
persists the TEXT — it never parses ONNX.

### `detection/repo.{h,cpp}` API

- `std::vector<DetectionModel> list_models(db)`
- `std::optional<int64_t> upsert_model(db, DetectionModel)` — by unique `filename`
- `std::vector<CameraModel> models_for(db, camera_id)` — attached models + their class selections
- `bool set_camera_models(db, camera_id, const std::vector<CameraModel>&)` —
  replace-all in one transaction (delete children + rows, re-insert), mirroring
  `camera::replace_areas`
- deletion of a camera already cascades via the camera FK.

## Runtime — `src/app/ui/camera/shared/detection/` (ORT + OpenCV, app layer)

Lives under `shared/` (the camera UI's leaf layer). New units:

- **`inference_engine.h`** — interface `std::vector<Detection> infer(const cv::Mat& bgr)`;
  `struct Detection { cv::Rect box; int class_id; float conf; }`. (`Detection`
  uses `cv::Rect`, so it stays in the app layer, not core — same reason
  `apply_orientation` is app-layer.)
- **`letterbox.{h,cpp}`** — pure: aspect-preserving resize+pad to 640×640, and
  the inverse box mapping. Unit-tested.
- **`yolo_decode.{h,cpp}`** — pure: decode `(1,14,8400)` → transpose, per-anchor
  argmax over the class scores, conf-floor filter, `cv::dnn::NMSBoxes`, map boxes
  back through the letterbox transform. Unit-tested with a synthetic tensor.
- **`ort_engine.{h,cpp}`** — `OrtEngine : InferenceEngine`. Owns one `Ort::Env` +
  `Ort::Session` loaded once from a model file. Builds `SessionOptions`
  appending **TensorRT EP → CUDA EP**, each in try/catch so a missing EP is
  non-fatal (CPU is the implicit fallback). TRT EP configured with
  `trt_engine_cache_enable` → `models/trt_cache/`. Exposes the model's
  `class_names` (read from ONNX metadata). `infer` runs at a low conf floor
  (e.g. 0.25) and returns *all* raw detections; per-class/per-camera filtering
  happens in the processor.
- **`engine_registry.{h,cpp}`** — one shared `OrtEngine` per distinct model
  filename, created lazily. Cameras that reuse a model share its engine.

### `DetectionProcessor` (in `grid/frame_processor.{h,cpp}`)

`DetectionProcessor : FrameProcessor`, constructed with the camera's orientation
(degrees/pitch/roll) and its resolved `CameraDetection`. `process(QImage)`:

1. orient via existing `apply_orientation` (detection tiles stay consistent with
   others),
2. `qimage_to_mat` (new inverse added to `shared/frame_convert.h`),
3. for each attached `ResolvedModel`: run its shared engine, keep detections
   where `class_id` is selected **and** `conf ≥ that class's threshold`,
4. draw each kept detection (colored rect + `"<class_name> NN%"`, port of the
   reference `draw_boxes`), back to `QImage`.

### Wiring — `grid/camera_grid.cpp`

At the single processor-construction point (currently line ~68): load the
camera's `CameraDetection` (resolved from `detection::repo` + `EngineRegistry`);
if it has ≥1 model build a `DetectionProcessor`, else the existing
`OrientationProcessor`. The shared `EngineRegistry` is owned by the grid.

## Model registry sync — app layer, at startup

In `main.cpp` (kept thin — delegates to a small app-layer helper): scan
`models/*.onnx`, read each file's `names` metadata via a lightweight loader,
and `upsert_model` into the `model` table (keyed by filename). Keeps the catalog
in sync with the folder; core never touches ONNX.

## UI — new "Models" wizard step

Add a fourth page to the camera dialog wizard between Configure and Areas.
Stepper becomes `① Source — ② Configure — ③ Models — ④ Areas`.

`dialog/models_page.{h,cpp}` (`ModelsPage`): lists available models
(`list_models`) each with an **attach** checkbox; an attached model expands to
its class list, each class with a **select** checkbox + a **conf**
`QDoubleSpinBox` (0.00–1.00). The page emits its selections; the coordinator
(`camera_dialog`) loads existing state via `models_for` and persists via
`set_camera_models` on Finish. Follows the existing page/coordinator pattern
(pages own controls + emit; coordinator drives DB writes).

## Build — `CMakeLists.txt`

- Define an imported target for ORT from `third_party/onnxruntime` (include dir +
  link `lib/onnxruntime.dll`); link it on the **`denso` app target only** (never
  `denso_core`).
- POST_BUILD copy next to the exe: `onnxruntime.dll`,
  `onnxruntime_providers_shared.dll`, `onnxruntime_providers_cuda.dll`,
  `onnxruntime_providers_tensorrt.dll`, and `models/denso.onnx` (matching the
  existing convention that the app finds `denso.db` beside the exe).
- `.gitignore` the ~600MB `third_party/onnxruntime/` binaries; document
  provisioning (the Jetson build drops in its own aarch64 ORT). Commit
  `models/denso.onnx` (11.7MB — consistent with the already-tracked `denso.pt`).

## Testing

- Catch2 unit tests (no GPU/model needed):
  - `detection::repo` — upsert-by-filename, `models_for`, `set_camera_models`
    replace-all round-trip, camera-delete cascade.
  - `letterbox` — resize+pad dims and inverse-map round-trip.
  - `yolo_decode` — synthetic `(1,14,8400)` tensor → expected boxes/classes after
    argmax + NMS.
- Manual on-device smoke: run a `test_model/images` frame through the app and
  confirm digit boxes appear on the tile with CUDA-EP acceleration.

## Non-goals (YAGNI)

- No persistence of readings; no left-to-right 4-digit assembly.
- No per-model global threshold UI beyond the per-class conf.
- No `network/model.h` rename (outside this feature).
- Managing/uploading models through the app — models are dropped into `models/`
  and picked up by the startup sync.

## Rough stage order (for the plan)

1. Rename `camera/model.h` → `camera/camera.h`; dir `processor/` → `detection/`
   (mechanical; keep the build green).
2. Domain + persistence: `detection.h`, tables/migration, `detection/repo` + tests.
3. Inference runtime: `letterbox`, `yolo_decode` (+ tests), `ort_engine`,
   `engine_registry`; ORT wired into CMake + DLL copy.
4. `DetectionProcessor` + `frame_convert` inverse + `camera_grid` wiring.
5. Model registry sync in `main.cpp`.
6. UI: `ModelsPage` wizard step + coordinator persistence.
