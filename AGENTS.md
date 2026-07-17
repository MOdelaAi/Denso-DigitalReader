# Denso-DigitalReader — Agent Guide

C++/Qt Widgets desktop app that reads digit displays from 1–4 cameras. Config and
readings live in one SQLite DB (`denso.db`) beside the executable. Two homes:
**Windows dev** (MSYS2 UCRT64) and the real deployment target, an **NVIDIA Jetson
Orin Nano**. (See `CLAUDE.md` for the full source map and hard rules.)

## Build targets (split by concern)

| Target | Location | Role / deps |
|---|---|---|
| `denso_core` | `src/core/` | Testable logic + SQLite persistence. Qt Core/Sql only — **no** Widgets, OpenCV, or inference runtime. |
| `denso_detection` / `denso_brazing` / `denso_camera` | `src/app/{detection,brazing,camera}/` | Pure static subsystem libs (inference helpers / reporting logic / non-widget capture infra). **Both `denso` and `denso_tests` LINK these** — tests validate the shipped objects, not a second compile. Graph: `denso_core ← {detection, brazing, camera(→detection)} ← denso`. |
| `denso` | `src/app/` | Qt Widgets GUI, camera capture, inference backend, orchestration + the subsystem libs. Widgets live under `src/app/ui/camera/`; the subsystems moved out to `src/app/{camera,detection,brazing}/`. |
| `denso_tests` | `tests/` | Catch2 over pure logic + platform-independent helpers (links `denso_core` + the three subsystem libs). |

Keep `main.cpp` a thin orchestrator. Domain code must not depend on GUI types
(the domain↔view boundary lives in `src/core/ui/convert.*`). DB changes are new
version-gated migrations in `db::run_migrations` — never rewrite a shipped one.

## Platform-split inference (`if(WIN32)/else()` in the top `CMakeLists.txt`)

Both backends implement the `InferenceEngine` interface and are selected via the
`BackendEngine` alias in `engine_registry.h`. OpenCV letterbox + `decode_yolo` /
`decode_yolo_end2end` are **shared and identical** across backends.

**Windows / MSYS2 UCRT64** — ONNX Runtime (`third_party/onnxruntime/`,
git-ignored). `OrtEngine`, provider fallback TensorRT→CUDA→CPU, builds+caches its
TRT engine on first run. Warm-up stays off the capture/GUI threads.

**Linux / Jetson Orin Nano (real target)** — native TensorRT, linking `nvinfer`
+ `CUDA::cudart`.
- `TrtEngine` **only deserializes a prebuilt `.engine`** — the operator builds it
  on-device with `trtexec` for TensorRT 10.3 / `sm_87`. **Never build at runtime.**
- **No fallback.** A missing / incompatible / invalid engine **fails loud** at
  startup: the throwing `TrtEngine` ctor → `WarmupWorker` catches → `app.exit(1)`.
- Class names come from a `<engine>.names.json` **sidecar** (TRT engines carry no
  name metadata).

`models/*.engine`, TRT caches, and the local ONNX Runtime drop-in are **not**
repo artifacts.

## Camera capture (`gst_pipeline.cpp` + `camera_stream.cpp`)

RTSP uses **hardware NVDEC** GStreamer pipelines:
`rtspsrc → depay/parse → nvv4l2decoder → queue(leaky) → nvvidconv → BGR appsink`.
The **leaky queue sits after the decoder only** (dropping compressed access units
corrupts the stream).

A capture-backend **ladder** auto-discovers each source by opening candidates and
reading a frame — the first that opens *and reads* wins (remembered for
reconnects):
- RTSP: NVDEC **H.264 → H.265** → OpenCV FFMPEG (handles mixed-codec fleets).
- USB: MJPEG (`nvv4l2decoder mjpeg=1`) → YUYV → `CAP_ANY`.

Inference is **decoupled from display**: `DetectionProcessor` runs the model on a
worker thread over a drop-oldest latest-frame slot and publishes a detections
snapshot the display path overlays — video stays smooth regardless of model speed.
Preserve reconnect/backpressure/clean stop-join in `CameraStream`. **No SQLite on
a capture thread.**

## Build & test

Windows (MSYS2 UCRT64):
```sh
export PATH=/c/msys64/ucrt64/bin:$PATH
cmake -S . -B build && cmake --build build && ctest --test-dir build
```
Jetson:
```sh
cmake -S . -B build -G "Unix Makefiles" && cmake --build build --target denso -j6
```
Jetson runtime deps: `qt6-base-dev`, `qt6-multimedia-dev`, `libqt6sql6-sqlite`,
`gstreamer1.0-libav` (avdec fallback), OpenCV 4.8, TensorRT 10.3, CUDA 12.6.
Catch2 v3 is fetched at first configure (needs net once).

## Deployment (designed, NOT built)

There is **no packaging or install flow yet** — the app is run from its build dir
on the Jetson. The design is specced at
`docs/superpowers/specs/2026-07-17-build-package-deployment-design.md`: a `.deb`
installed with `apt`, `/opt/denso/data` for mutable state via `$DENSO_DATA_DIR`,
a `QLockFile` single-instance guard, four headless modes (`--version`,
`--check`, `--check-running`, `--check-migrations`), and `denso-setup` for
autostart/autologin. Do not document any of it as working until it lands.

Verified facts that will bite whoever implements it:
- **`libopencv` (NVIDIA's OpenCV) ships no `.shlibs`/`.symbols`**, and
  `dpkg-shlibdeps` errors on `/usr/local/cuda/lib64/libcudart.so.12`. `Depends:`
  must be mixed: `${shlibs:Depends}` + hand-declared `libopencv`,
  `cuda-cudart-12-6`, and the dlopened set (`qt6-qpa-plugins`,
  `libqt6sql6-sqlite`, `gstreamer1.0-*`). Never pass `--ignore-missing-info` — it
  drops deps silently and looks derived while being wrong.
- **Debian policy forbids a package installing to `/usr/local`** — the launcher
  goes in `/usr/bin`.
- The exe is **relocatable**: the only RUNPATH is the absolute
  `/usr/local/cuda/lib64`, so no `LD_LIBRARY_PATH` is needed after copying it out
  of the build tree.
- The Jetson **halts at the GDM greeter on power-on** (no active graphical user
  session), so autostart is inert without autologin.
- `Db::open()` runs `PRAGMA journal_mode = WAL` and `EngineRegistry::warm_up()`
  creates `trt_cache` — so neither can be used by a "non-mutating" check path.

## Gotchas

- Tracked architecture doc is **`docs/ARCHITECTURE.md` (UPPERCASE)** — `git add
  docs/architecture.md` stages nothing.
- Jetson is **gcc 11 / Qt 6.2**: include `<cstddef>` for bare `size_t`; the 3-arg
  `QTransform::rotate` is Qt 6.5+ (keep the version guard).
- `denso.onnx` / `digitv2.onnx` are **static batch=1** — a dynamic `opt=4` engine
  needs re-exporting the ONNX with `dynamic=True`.
- Don't leak Qt Widgets / OpenCV / ORT / CUDA / TensorRT into `denso_core`.
- Keep platform behavior behind the existing interfaces; the shared
  letterbox/decode must stay identical across backends.
- **Never `git add -A` in this repo.** `denso/yolo26n/yolov8n.onnx` are tracked,
  and the 37 MB `digitv2.onnx` + `note.txt` are now git-ignored — but `.gitignore`
  names `models/digitv2.onnx` **specifically**, not `models/*.onnx`. So a *new*
  model dropped into `models/` is untracked and unignored, and a blanket add
  still sweeps a multi-MB blob into a commit, which then blocks `git pull` on the
  Jetson. Use explicit `git add <files>` / `git add -u`.
- The DB migration chain is at **v11** (`camera.areas_need_review`, added for the
  editable-source / ROI-quarantine feature). Add a new migration — never edit a
  shipped one.
- Logging is a bounded rotating file sink (`src/app/logging/`, ~25 MiB cap) meant
  for 24/7 runs; `qDebug/qWarning/qCritical` route through it. `DENSO_LOG_LEVEL`
  sets the floor.
