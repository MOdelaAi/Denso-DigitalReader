# Denso-DigitalReader

Desktop app (C++ / Qt Widgets / CMake) for reading a 4-digit 7-segment display,
with a settings UI for display resolution, theme, hardware spec, and network
configuration. Single SQLite store (`denso.db`) next to the executable.

Ported 1:1 from a Rust + Slint original; the port history lives on branch
`port/cpp-qt`.

## Commands

Out-of-source build with CMake (needs Qt6: Core, Gui, Sql, Widgets, Multimedia,
Network; OpenCV; and ONNX Runtime for detection). Builds and runs on the MSYS2
UCRT64 toolchain (`pacman -S mingw-w64-ucrt-x86_64-qt6-base
mingw-w64-ucrt-x86_64-qt6-multimedia mingw-w64-ucrt-x86_64-opencv
mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja`). ONNX Runtime is
provisioned into `third_party/onnxruntime/` (git-ignored — see
`docs/GPU_SETUP.md`).

IP-camera streaming uses OpenCV's **GStreamer** backend (low latency — it drops
stale frames, unlike the buffering FFMPEG backend). That needs the decode
plugins present or the app silently falls back to FFMPEG and lag returns:
`pacman -S mingw-w64-ucrt-x86_64-gst-plugins-base
mingw-w64-ucrt-x86_64-gst-plugins-good mingw-w64-ucrt-x86_64-gst-plugins-bad
mingw-w64-ucrt-x86_64-gst-libav` (bad = h264parse/h265parse + d3d12 HW decoder,
libav = avdec software fallback). On the Jetson target these ship with the
NVIDIA GStreamer stack.

| Action | Command |
|---|---|
| Configure | `cmake -S . -B build -G Ninja` |
| Build | `cmake --build build` |
| Test | `ctest --test-dir build` |
| Run | `./build/src/app/denso` (path varies by generator) |

Tests are Catch2 v3, fetched via `FetchContent` at configure time (needs net on
first configure). Platform backend tests are compiled per-OS, so the passing
count differs between Windows and Linux.

## Layout

Two targets wired by a thin top-level `CMakeLists.txt` (`add_subdirectory`):

| Path | Target | Role |
|---|---|---|
| `src/core/` | `denso_core` (lib) | Ported logic + SQLite persistence + the Qt-free domain↔view boundary. `Qt6::Core`/`Sql` only. |
| `src/app/` | `denso` (GUI exe) | Qt Widgets UI + entry-point orchestrator. `Qt6::Widgets`. |
| `tests/` | `denso_tests` | Catch2 unit tests over `denso_core`. |

Each target dir is its own include root, so includes read `network/model.h`,
`ui/convert.h`, `ui/theme.h`, etc.

### `src/core/` (library)

| Path | Responsibility |
|---|---|
| `db/db.{h,cpp}` | SQLite base (`denso.db`, WAL) + the `user_version`-gated migration chain in `run_migrations`. |
| `settings/` | Persisted app settings (window size, theme, fullscreen) + resolution presets. `settings`=type/presets, `repo`=persistence + legacy import. |
| `hardware/` | Read-only host spec via `QSysInfo`/`QStorageInfo` (collected fresh, never stored). `format`=byte formatting, `collect`=the spec. |
| `network/` | `NetworkBackend` base + `reassert` + `NetConfig`/status types + config persistence (`repo`). |
| `network/windows/{netsh,wifi,parse}.*`, `network/linux/nmcli.*` | Pure, unit-tested OS-command helpers (compiled on every OS for off-device testing). |
| `network/windows/windows_backend.cpp`, `network/linux/linux_backend.cpp` | OS backends (`QProcess`); one compiled per platform. |
| `ui/convert.{h,cpp}`, `ui/viewmodel.h` | The **only** domain↔view boundary (Qt-free, testable). |
| `camera/` | Camera inventory: domain structs (`camera.h`: `Camera` + polygon `CameraArea` over normalized `Point`s) + persistence (`repo`, full camera CRUD + ROI-area read/replace). `area_points` (de)serializes a polygon's normalized vertices to the `camera_area.points` TEXT column; `area_geometry` is pure, unit-tested point-in-polygon (`point_in_polygon`/`inside_any_area`) used to confine detection to the ROI. |
| `detection/` | Per-camera detection config domain: `detection.h` structs (`DetectionModel`, `CameraModel`, `ModelClassSelection`, resolved `CameraDetection`) + persistence (`repo`: model catalog, per-camera attachments, and the `detection_for` resolve query) + `class_names` JSON (de)serialization. Qt/OpenCV-free — the ORT inference runtime lives in the app. |
| `util/strutil.h` | Small shared string helpers. |

### `src/app/` (GUI)

UI grouped by feature: the **app shell** at `ui/` root, plus `ui/settings/` and
`ui/camera/`.

| Path | Responsibility |
|---|---|
| `main.cpp` | Thin orchestrator: open DB → migrate → import legacy → reassert network → load settings → apply startup → run. |
| `ui/theme.{h,cpp}` | Palette + theme-driven app stylesheet. |
| `ui/mainwindow.{h,cpp}` | Root window (top bar + content); hosts settings-persistence handlers; opens the settings + camera modals. |
| `ui/settings/settings_dialog.{h,cpp}` | Settings modal: nav + 5 panels; owns DB-backed network apply + threaded scan/connect/refresh. |
| `ui/settings/netcard.{h,cpp}` | Per-interface status + editable config + Wi-Fi scan/connect. |
| **`ui/camera/`** | Grouped into two entry points (root) + three layers: `grid/` (live view), `dialog/` (modal), `shared/` (cross-cutting primitives). `shared/` is a leaf; `grid/` and `dialog/` depend only on it, never on each other. |
| `ui/camera/camera_view.{h,cpp}` | **Entry point.** Main content switcher: empty state (+ Add) when 0 cameras, else the live `CameraGrid`. `release_streams()`/`reload()` free + restart capture around the modal. |
| `ui/camera/camera_dialog.{h,cpp}` | **Entry point.** Camera management modal — a thin **coordinator** over a 5-page stack (the `dialog/` pages) run as a guided wizard: list/delete + add (USB scan / IP manufacturer+stream+credentials) → Configure (preview + resolution/fps/rotation/pitch/roll) → Models (attach 1..N detection models + per-class confidence) → Areas (draw ROI polygons; optional). Owns snapshot capture, add/edit DB writes, navigation + sizing. Stepper header + Back/Next/Finish footers; `show_page()` centralizes page+stepper+sizing; the modal grows near-fullscreen on the Areas step. |
| `ui/camera/grid/camera_grid.{h,cpp}` | Live 1–4 grid: a tile + `CameraStream` per camera (first 4 by id), laid out via `grid_dims`; owns start/stop/reload. |
| `ui/camera/grid/camera_stream.{h,cpp}` | Per-camera capture worker (own `std::thread` + `cv::VideoCapture`): read → `FrameProcessor` → queued `frame_ready`/`status_changed`; ~15 fps display cap paced by a high-resolution `precise_sleep` (MinGW `sleep_for` is pinned to the ~15.6 ms tick), clean stop/join. USB opens by index; IP opens via `rtsp_gst_pipeline` on `cv::CAP_GSTREAMER`, falling back to FFMPEG. Capture resolution is set for USB only (setting it on a live GStreamer pipeline segfaults). |
| `ui/camera/grid/camera_tile.{h,cpp}` | One grid cell: paints the latest frame (aspect-fit) + name + status dot + live per-tile FPS + the camera's ROI polygons as gold outlines (`set_areas`); placeholder when connecting/offline. |
| `ui/camera/grid/fps_meter.{h,cpp}` | Pure (unit-tested) EMA-smoothed FPS estimate from frame-arrival timestamps; each tile `tick()`s it per displayed frame. |
| `ui/camera/grid/frame_processor.{h,cpp}` | The per-camera frame seam: `FrameProcessor` interface + `OrientationProcessor` (orientation only) + `DetectionProcessor` (orientation → ONNX inference → per-class conf filter → ROI confinement (keep only boxes whose centre is inside an area; empty areas = whole frame, via `area_geometry`) → draw labelled boxes). `camera_grid` picks per camera: `DetectionProcessor` when the camera has attached, loadable models, else `OrientationProcessor`. |
| `ui/camera/grid/grid_layout.{h,cpp}` | Pure (unit-tested) `grid_dims(n)` → rows/cols (1→1×1, 2→1×2, 3–4→2×2). |
| `ui/camera/dialog/` (pages) | The dialog's five page widgets + shared `page_util` (`dim_label`): `list_page` (`CameraListPage`), `add_page` (Source form + scans), `configure_page` (preview + orientation controls), `models_page` (`ModelsPage`: attach models + per-class conf), `areas_page` (ROI editing). Pages own their controls and emit request signals; the coordinator drives them. |
| `ui/camera/dialog/wizard_stepper.{h,cpp}` | Non-interactive "① Source — ② Configure — ③ Models — ④ Areas" step indicator; `set_current()` emphasizes the active step. |
| `ui/camera/dialog/roi_canvas.{h,cpp}` | Draw-only `QWidget` for one ROI polygon over the oriented snapshot: click to add vertices, click first vertex / Enter to close, Backspace/Esc to undo/clear. |
| `ui/camera/dialog/camera_devices.{h,cpp}` | USB enumeration (Qt Multimedia). |
| `ui/camera/dialog/ip_scan.{h,cpp}` | Subnet RTSP-port scan (Qt Network, threaded). |
| `ui/camera/shared/snapshot.{h,cpp}`, `shared/frame_convert.h` | Grab one preview frame (OpenCV, off-thread) + orient it: `apply_orientation` composes rotation + roll + pitch (perspective warp) for the live Configure preview. |
| `ui/camera/shared/rtsp_templates.{h,cpp}` | Manufacturer → RTSP URL templates (Dahua) + `with_credentials`. |
| `ui/camera/shared/gst_pipeline.{h,cpp}` | Pure (unit-tested) string builder for a low-latency RTSP GStreamer pipeline (explicit depay/parse/`avdec` chain, drop-on-latency `rtspsrc`, leaky queue, shallow dropping appsink) for `cv::CAP_GSTREAMER`. |
| `ui/camera/shared/roi_geometry.{h,cpp}` | Pure (unit-tested) widget↔normalized point mapping + aspect-fit rect for the canvas. |
| `ui/camera/shared/detection/` | Per-camera ONNX detection runtime (app-only: OpenCV + ONNX Runtime). Pure, unit-tested helpers `letterbox` (resize+pad to 640 + inverse box map), `yolo_decode` (`decode_yolo`: raw `[1,4+nc,anchors]` → argmax + conf floor + NMS; `decode_yolo_end2end`: NMS-free `[1,N,6]` → conf floor only — `ort_engine` picks by output shape), `names_metadata` (parse the ONNX `names` dict) feed `inference_engine` (interface) → `ort_engine` (session + TensorRT(FP16, cached)→CUDA→CPU EP fallback) → `engine_registry` (one shared engine per model file; `warm_up()` loads + runs one blank inference on every model at startup so the first real frame — and the minutes-long TensorRT build — never stalls a capture thread). `model_sync` scans `models/*.onnx` into the `model` catalog at startup. |

## Detection / ONNX Runtime

Per-camera YOLO detection is an **app-only** feature — the domain config lives in
`src/core/detection/` (Qt/OpenCV-free, unit-tested), the inference runtime in
`src/app/ui/camera/shared/detection/` (OpenCV + ORT). Key facts:

- The ORT GPU build lives in `third_party/onnxruntime/` (git-ignored; provision
  per platform — the Jetson build drops in its own aarch64 ORT). See
  `docs/GPU_SETUP.md`.
- ORT + provider DLLs and `models/denso.onnx` are copied **beside the exe** by a
  `POST_BUILD` step (the app resolves both relative to the executable dir).
- Execution-provider fallback is **TensorRT → CUDA → CPU**; a missing GPU stack
  degrades to CPU, never a hard failure. GPU provider DLLs are staged into
  `third_party/gpu_ep/` (git-ignored) and glob-copied beside the exe when present.
- The TensorRT EP builds an optimized engine on first run (FP16, cached under
  `models/trt_cache/`) — a **minutes-long, non-interruptible** build. It must run
  during the startup `EngineRegistry::warm_up()` (main thread, before any capture
  thread exists), never lazily on a capture thread where it froze the UI and
  blocked stream teardown — the reason TensorRT was dropped once before.
  `tools/build_trt_engine.sh` builds standalone `trtexec` engines offline;
  `models/*.engine` and `models/trt_cache/` are git-ignored.
- `models/*.onnx` are synced into the `model` catalog at startup (`model_sync`),
  so dropping a new `.onnx` in `models/` makes it selectable next launch.
- `denso_core` **never** links OpenCV/ORT — only `Qt6::Core`/`Sql`.

## Hard rules

- Domain/feature types **never** see UI view types. The only boundary is
  `src/core/ui/convert.{h,cpp}`.
- `main.cpp` stays a thin orchestrator — no business logic.
- Each feature is split header/source by responsibility (type / persistence /
  OS access). Access policy is the `repo`'s API surface, not SQL grants.
- Persistence is one SQLite file with version-gated migrations in
  `db::run_migrations` — add a migration, never edit a shipped one.
- OS-specific work sits behind `NetworkBackend` (`network/backends/`). Keep both
  platforms in sync.
- `denso_core` must not link `Qt6::Widgets` — the GUI cannot leak into the
  testable core.
- `build/` and `*.png` are git-ignored (see `.gitignore`); `assets/icon.png`
  predates the `*.png` rule and stays tracked as a committed source asset.

## Workflow

Superpowers SDD: specs in `docs/superpowers/specs/`, plans in
`docs/superpowers/plans/`, progress ledger in `.superpowers/sdd/progress.md`.

See `docs/ARCHITECTURE.md` for the boot sequence, data flow, threading model,
persistence model, and gotchas (including the QSQLITE read-cursor-before-DDL
locking rule).
