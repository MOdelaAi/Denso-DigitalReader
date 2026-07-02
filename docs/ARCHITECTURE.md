# Architecture

Depth reference for Denso-DigitalReader (C++ / Qt Widgets / CMake). For the
quick map and commands, see the root `CLAUDE.md`.

## Project layout

Two targets, split by concern, wired by a thin top-level `CMakeLists.txt` via
`add_subdirectory`:

| Path | Target | Depends on |
|---|---|---|
| `src/core/` | `denso_core` (static lib) | `Qt6::Core`, `Qt6::Sql` only |
| `src/app/` | `denso` (Qt Widgets GUI exe) | `denso_core`, `Qt6::Widgets` |
| `tests/` | `denso_tests` (Catch2) | `denso_core` |

`denso_core` holds the ported logic + SQLite persistence **and** the Qt-free
domain↔view boundary (`core/ui/convert` + `viewmodel`). It never links
`Qt6::Widgets`, so the GUI cannot leak into the testable core. Each target's
directory is its own include root: core headers read `network/model.h` /
`ui/convert.h`, the app's widget headers read `ui/theme.h` /
`ui/camera/camera_dialog.h`, and the app reaches core headers through
`denso_core`'s public include dir.

## Boot sequence (`src/app/main.cpp`)

A thin orchestrator:

1. `QApplication` is constructed, then `QLocale::setDefault(English/UnitedStates)`
   forces Western Arabic digits in numeric widgets (`QSpinBox`/`QDoubleSpinBox`)
   regardless of the OS regional format — without it a Thai-locale host renders
   spin-box values as Thai numerals (๐–๙).
2. `db::Db::open(db::default_path())` opens `denso.db` (next to the exe) in WAL
   mode; `db::run_migrations` applies the `user_version`-gated chain.
3. `settings::import_legacy` does a one-time import of any pre-SQLite
   `settings.json` sitting beside the DB.
4. `ui::sync_models` scans `models/*.onnx` beside the exe and upserts each into
   the `model` catalog (reading its class names from the ONNX metadata), so
   models dropped into `models/` become selectable in the camera wizard.
5. `network::reassert` re-applies every saved interface config to the OS — the
   app is the source of truth. Best-effort and non-fatal: failures are logged
   via `qWarning`, never block startup.
6. `settings::load` seeds an in-memory `std::shared_ptr<Settings>`.
7. `MainWindow::apply_startup` populates read-only fields + applies settings;
   the window/dialog ctors install the callbacks. `QApplication::exec()`.

`Db` (an `optional<Db>` in `main`) outlives the window, so the connection it
hands the UI stays valid for the whole run.

## UI ↔ domain boundary (`src/core/ui/`)

Feature modules never reference UI view types. `ui/convert.{h,cpp}` is the
single crossing point: `to_*` build view models (`viewmodel.h`:
`NetStatus`/`NetConfigUi`/`WifiRow`) from domain types, `from_ui_config` parses
an editable view model back to a domain `NetConfig` (blank/unparseable fields
become unset). It is `std::string`-only and unit-tested (`test_convert.cpp`);
the widgets convert to/from `QString` at their edge.

A config change travels: UI edit → `SettingsDialog::apply_net_config` →
`from_ui_config` → `network::save` (persist; app owns truth) →
`backend().apply_config` (push to OS) → status string back to the card.

## GUI (`src/app/ui/`)

Grouped by feature so the folder scales: the **app shell** at `ui/` root, with
`ui/settings/` and `ui/camera/` subfolders.

**Shell (`ui/`)**
- `theme.{h,cpp}` — the dark/light palette + a stylesheet builder applied to
  the whole app (the Slint `Theme` global / `Palette.color-scheme` analog).
- `mainwindow.{h,cpp}` — root window: top button bar (Camera / Settings) over
  the content area. Hosts the settings-persistence handlers (resolution / theme
  / fullscreen / reset), since those resize the window and restyle the app, and
  opens the settings + camera modals.

**Settings (`ui/settings/`)**
- `settings_dialog.{h,cpp}` — modal: a left nav over five panels (Appearance,
  Display, System, Network, About). Owns the DB-backed network apply and the
  threaded scan/connect/refresh.
- `netcard.{h,cpp}` — one interface's live status + editable IP/DNS config +
  (Wi-Fi) scan list with per-row connect.

**Camera (`ui/camera/`)**

The folder is a clean 3-layer stack under two root entry points. **Root** holds
the public surfaces (`camera_view`, `camera_dialog`). **`grid/`** is the live-view
internals (`camera_grid`, `camera_stream`, `camera_tile`, `frame_processor`,
`fps_meter`, `grid_layout`). **`dialog/`** is the modal internals (the five page
widgets + `page_util`, plus the dialog-only `wizard_stepper`, `roi_canvas`, and
the source scanners `camera_devices`, `ip_scan`). **`shared/`** is the
cross-cutting primitives used by *both* surfaces (`snapshot`, `frame_convert`,
`rtsp_templates`, `gst_pipeline`, `roi_geometry`). Dependencies flow one way:
`shared/` is a leaf; `grid/` and
`dialog/` depend only on it, never on each other; the root entry points compose
all three.

- `camera_view.{h,cpp}` — the main content area: a switcher between the empty
  "no cameras" state (+ Add) and the live **`CameraGrid`**. `release_streams()`
  stops capture while the Camera modal is open (so its snapshot can claim the
  same USB device); `reload()` rebuilds + restarts when the modal closes.
- `camera_grid.{h,cpp}` / `camera_tile.{h,cpp}` / `camera_stream.{h,cpp}` —
  the live 1–4 feed grid. `CameraGrid` lays out one `CameraTile` + one
  `CameraStream` per camera (first four by id; `grid_dims` picks 1 / 1×2 / 2×2).
  Each `CameraStream` runs a `cv::VideoCapture` read loop on its **own
  `std::thread`**, converts each frame (`mat_to_qimage`), runs it through a
  `FrameProcessor`, and emits `frame_ready`/`status_changed` as **queued**
  signals to its tile (capped ~15 fps; finite open/read timeout so a dead camera
  can't hang teardown; `stop()` joins). USB cameras open by device index; IP
  cameras open through a low-latency `rtsp_gst_pipeline` on the **GStreamer**
  backend (`cv::CAP_GSTREAMER`) with an **FFMPEG fallback** if GStreamer can't
  open — GStreamer drops stale frames so glass-to-glass lag stays bounded. The
  display cap is paced by a high-resolution waitable timer (`precise_sleep`),
  because MinGW's `std::this_thread::sleep_for` is pinned to the ~15.6 ms OS
  tick and would undershoot the target rate (~9 fps for a 15 fps cap).
  `CameraTile` is a pure view — paints the latest frame aspect-fit with a name,
  status dot, and a live per-tile FPS readout (`FpsMeter`), and overlays the
  camera's saved ROI polygons (`set_areas`) as gold outlines. The overlay maps
  the normalized vertices through the **same** `roi_geometry::fitted_image_rect`
  the frame is drawn into, and the frame is already oriented (the stream's
  processor), so ROIs — stored normalized to the oriented frame — line up
  without extra transform. When detection is active the ROI is also enforced on
  the *pixels*: `DetectionProcessor` keeps only boxes whose centre falls inside
  an area polygon (empty areas = whole frame).
- `frame_processor.{h,cpp}` — the per-camera processing seam. `FrameProcessor`
  is the interface; `OrientationProcessor` (applies rotation/pitch/roll) is the
  orientation-only impl, and `DetectionProcessor` layers ONNX inference on top
  (orient → infer → per-class confidence filter → ROI confinement → draw
  labelled boxes). The ROI step keeps only boxes whose normalized centre lands
  inside one of the camera's area polygons (`camera::inside_any_area`); a camera
  with no areas detects the whole frame.
  `camera_grid` picks per camera: `DetectionProcessor` when the camera has
  attached, loadable models (resolved via `detection::detection_for` +
  `EngineRegistry`), else plain `OrientationProcessor` — the capture loop and
  tile don't change. `grid_layout.{h,cpp}` is the pure, unit-tested
  `grid_dims(n)`.
- `camera_dialog.{h,cpp}` — the camera management hub: a thin **coordinator**
  over a 5-page stack run as a guided wizard — list + delete, then **① Source**
  (USB auto-scan, or IP via manufacturer + main/sub stream + credentials with a
  live RTSP-URL preview) → **② Configure** (snapshot preview + resolution / fps /
  rotation / pitch / roll) → **③ Models** (attach 1..N detection models, each with
  per-class confidence) → **④ Areas** (draw ROI polygons). Each page is its
  own widget under `dialog/` (see below), owning its controls and emitting
  request signals; the coordinator owns the camera source (snapshot capture),
  the add/edit DB writes, wizard navigation and modal sizing. A `WizardStepper`
  header shows the current step; footers are consistent Back / Next / Finish.
  `show_page(index)` is the single entry point that switches the stack page,
  drives the stepper, and resizes — the modal grows to **near-fullscreen on the
  Areas step** for drawing room and restores the compact size on leaving. The
  camera is inserted/updated when Configure's **Next** is pressed, so both the
  Models and Areas steps attach to a known camera id; Models persists via
  `detection::set_camera_models` on its Next, Areas is **optional** (Skip
  returns without writing ROIs, Finish saves them). Each list row also has an
  **Areas** button to draw/edit later. `showEvent` reopens the reused dialog on
  the list at compact size. Persists through `camera::repo` + `detection::repo`.
- `dialog/` — the five page widgets the dialog coordinates, each self-contained
  and DB-light: `page_util` (shared `dim_label` + error colour), `list_page`
  (`CameraListPage`: reads/deletes cameras, emits add/configure/areas requests),
  `add_page` (`CameraAddPage`: the Source form + USB/IP scans, emits the
  assembled draft), `configure_page` (`CameraConfigurePage`: preview +
  orientation controls; the coordinator pushes frames in via `set_frame`),
  `models_page` (`ModelsPage`: lists the model catalog with an attach checkbox +
  per-class select/conf, pre-filled from `models_for`; emits its selections for
  `set_camera_models`), and `areas_page` (`CameraAreasPage`: edits a working ROI
  set over the pushed background frame, emits the set on save — no DB access of
  its own).
- `camera_devices.{h,cpp}` — USB enumeration via Qt Multimedia (`QMediaDevices`).
- `ip_scan.{h,cpp}` — crude IP discovery: a threaded subnet probe for hosts with
  the RTSP port open (Qt Network).
- `rtsp_templates.{h,cpp}` — manufacturer → RTSP URL template map (Dahua for
  now); builds the credential-free URL and injects credentials at capture time.
- `snapshot.{h,cpp}` + `frame_convert.h` — grab one frame for the Configure
  preview (OpenCV `VideoCapture`, off the GUI thread, finite open/read timeout)
  and orient it for display. `apply_orientation(src, degrees, pitch, roll)`
  composes the preset rotation + roll (in-plane, about Z) + pitch (out-of-plane
  tilt about X, rendered as a `QTransform` perspective warp about the image
  centre); the perspective viewer distance is derived from frame size, not
  stored. The rotation combo and the pitch/roll spin boxes all re-render the
  preview live on change. `apply_rotation` (the 0/90/180/270 preset) stays as a
  separately-tested helper.
- `roi_canvas.{h,cpp}` + `roi_geometry.{h,cpp}` — the **Areas** page's drawing
  surface. `RoiCanvas` is a draw-only `QWidget` that paints the oriented
  snapshot (reusing `apply_orientation`, so ROIs sit on exactly the configured
  view) and lets the user click out a polygon of 3+ vertices: click to add,
  click the first vertex / double-click / Enter to close, Backspace to undo, Esc
  to clear. It holds vertices **normalized to [0,1]** and knows no DB policy —
  the dialog loads/persists. `roi_geometry` is the pure, unit-tested mapping
  (aspect-fit rect + widget↔normalized conversion with clamping) it builds on.
- `wizard_stepper.{h,cpp}` — `WizardStepper`, the non-interactive
  "① Source — ② Configure — ③ Models — ④ Areas" indicator above the page stack;
  `set_current()` emphasizes the active step. Navigation stays with the dialog's
  Back/Next/Finish buttons.

### Threading

`apply_net_config` runs **synchronously** on the GUI thread (as the Rust
original did). The blocking OS calls — `scan_wifi`, `connect_wifi`,
`refresh_network` — run on a worker `QThread` (`QThread::create`, so QProcess in
the backends has an event dispatcher) and post results back with
`QMetaObject::invokeMethod(this, …, Qt::QueuedConnection)`. This is the Qt
analog of the Rust `std::thread` + `upgrade_in_event_loop`. A fresh
`network::backend()` is created per operation. The settings dialog is created
once and reused, so worker callbacks always have a valid target.

## Persistence model (`src/core/db/`)

One file, `denso.db`, WAL mode so the UI reads while a worker writes. The schema
is an ordered, `user_version`-gated chain inside `db::run_migrations`
(`db/db.cpp`) — add a migration, never edit a shipped one. Each feature's `repo`
exposes only the operations its data policy allows (e.g. `hardware` is not
stored at all). The `settings` table is a typed key/value store; `net_config`
is typed columns, one row per interface.

## Network feature (`src/core/network/`)

Two distinct datasets share the Network tab:
- **Live status** — `snapshot()` reads the OS (`ipconfig`/`netsh` on Windows,
  `nmcli` on Linux) via `QProcess`. Read-only, transient.
- **Config** — `NetConfig` is user-owned, persisted, and reasserted to the OS
  at boot via `reassert`.

OS work sits behind the `NetworkBackend` base class. `backend()` returns the
platform impl (`WindowsBackend`, `LinuxBackend`, or a `NullBackend` fallback);
each platform's code is grouped under `network/windows/` and `network/linux/`,
and exactly one `*_backend.cpp` is compiled per OS (the other
`make_*_backend()` declaration is never odr-used). The pure helpers are
unit-tested off-device: Windows `netsh`/`parse`/`wifi`, Linux `nmcli`. Errors
mirror the Rust `Result::Err(String)` as a thrown `std::runtime_error`;
`reassert` catches them into non-fatal `(iface, message)` pairs.

## Detection feature (`src/core/detection/` + `src/app/ui/camera/shared/detection/`)

Per-camera YOLOv8 detection, split across the same domain/runtime line as the
rest of the app. **Domain** (`src/core/detection/`, Qt/OpenCV-free, unit-tested):
`detection.h` structs (`DetectionModel` catalog rows, `CameraModel` attachments
with per-class `ModelClassSelection`, and the resolved `CameraDetection` bundle),
`class_names` JSON (de)serialization for the `model.class_names` column, and
`repo` — the model catalog (`upsert_model`/`list_models`), per-camera attachments
(`models_for`/`set_camera_models`, replace-all in one transaction), and
`detection_for`, the resolve query that joins a camera's attachments to their
filenames + class names for the runtime. Schema is migration **v8**
(`model` / `camera_model` / `camera_model_class`). ROI confinement rides on the
camera domain: `camera/area_geometry` (`point_in_polygon` / `inside_any_area`,
Qt/OpenCV-free, unit-tested) tests a detection's normalized box centre against a
camera's area polygons.

**Runtime** (`src/app/ui/camera/shared/detection/`, OpenCV + ONNX Runtime, app
target only): pure unit-tested helpers `letterbox` (aspect-preserving resize +
gray pad to 640, plus the inverse box map), `yolo_decode` (two decoders chosen by
output shape — `decode_yolo` for the raw transposed `[1, 4+nc, na]` head via
per-anchor argmax + confidence floor + class-agnostic `cv::dnn::NMSBoxes`, and
`decode_yolo_end2end` for an NMS-free `[1, N, 6]` output where the model already
did NMS so only a confidence floor + inverse box map remain), and
`names_metadata` (parse the ONNX `names` dict). These feed the `InferenceEngine`
interface, implemented by `OrtEngine` (one ORT session with a **TensorRT → CUDA →
CPU** execution-provider fallback). The TensorRT tier runs FP16 with a serialized
engine cache (`models/trt_cache/`); its first-run build is minutes-long and
non-interruptible, so `EngineRegistry::warm_up()` loads **and** runs one blank
inference over every `models/*.onnx` at startup — on the main thread, before any
capture thread — to absorb that build and CUDA kernel init off the hot path.
`EngineRegistry` keeps one shared engine per model filename (lazy, failed loads
cached as `nullptr` so a bad model isn't retried per frame). `model_sync` runs at
boot to keep the catalog in step with `models/*.onnx`.

Streaming feeds these: IP cameras capture through `rtsp_gst_pipeline` (a pure
string builder for an explicit depay/parse/`avdec` GStreamer chain with a
drop-on-latency `rtspsrc`, leaky queue, and shallow dropping appsink) on the
GStreamer backend, falling back to FFMPEG when GStreamer can't open.

`CameraGrid` chooses per camera: it resolves `detection_for`, asks the registry
for each attached model, and constructs a `DetectionProcessor` (orient → infer →
per-class conf filter → ROI confinement → draw labelled boxes) when ≥1 model
loads, else a plain `OrientationProcessor`. The registry (and thus ORT) is
touched only on the UI thread; the engines outlive the capture streams. ORT +
provider DLLs and every `models/*.onnx` are copied beside the exe by a
`POST_BUILD` step; the GPU provider DLLs come from the git-ignored
`third_party/gpu_ep/` (see `docs/GPU_SETUP.md`), and a missing GPU stack silently
degrades to the CPU provider.

## Gotchas

- **QSQLITE keeps a read cursor alive until the `QSqlQuery` is finished or
  destroyed.** A live read cursor (e.g. an un-scoped `PRAGMA user_version`
  query) makes a later schema change on the same connection fail with
  `SQLITE_LOCKED` ("database table is locked"). `run_migrations` reads the
  version in its own scope so the cursor is released before any DDL — keep that
  pattern (finish/scope reads before writes); `rusqlite` finalized this for us,
  QSQLITE does not.
- Builds on the **MSYS2 UCRT64** toolchain (GCC + Qt6 from `pacman`): configure
  with `cmake -S . -B build -G Ninja` — its CMake finds Qt6 with no
  `CMAKE_PREFIX_PATH`. On that toolchain the MSVC `/utf-8` flag is a harmless
  no-op (GCC reads UTF-8 by default).
- MSVC needs `/utf-8` (set in the top-level CMake) so the UI's non-ASCII
  literals (`✕ … — 🔒`) reach the binary byte-for-byte (the sources are UTF-8
  without BOM).
- Linux disk sum over-counts loop/tmpfs/overlay mounts; sub-GB renders "0 GB"
  (embedded MB-range accepted). Verify on a real Linux device.
- `nmcli -t` SSID escaping (`\:`) and VLAN device names (`eth0:0`) are not yet
  handled — deferred to on-device validation.
- Platform backend tests are compiled per-OS, so the passing test count differs
  between Windows and Linux.
- Deferred UI parity nits from the port: the Network cards don't dim while
  loading (only the Refresh label changes); re-clicking the already-active
  Network nav item doesn't re-trigger a refresh (the Refresh button does).
- `denso.db` (+ `-wal`/`-shm`) is created next to the executable at runtime and
  is git-ignored.
- **Frame-pacing duration units:** `CameraStream`'s display-rate cap sleeps in
  chunks computed in the clock's own (nanosecond) `steady_clock::duration`, NOT
  whole milliseconds. `duration_cast<milliseconds>` of a sub-millisecond
  remainder truncates to 0 → a `remaining -= 0` chunk loop busy-spins forever
  and wedges that capture thread. This froze the faster feeds in the live grid
  (the slow USB feed, whose per-frame time exceeded the interval, skipped the
  loop and was the only one that survived) — keep pacing math in the clock's
  duration. `qWarning()` is routed to a `denso.log` file beside the exe (GUI
  subsystem on Windows has no console); `cv::setNumThreads(0)` keeps OpenCV
  conversions inline since each camera already has its own thread.
- **MinGW `sleep_for` is pinned to the ~15.6 ms OS scheduler tick** and ignores
  `timeBeginPeriod`, so a 66 ms (15 fps) pace overshoots to ~100 ms and delivers
  ~9 fps. `CameraStream` sleeps via `precise_sleep`, a high-resolution waitable
  timer (`CREATE_WAITABLE_TIMER_HIGH_RESOLUTION`), falling back to `sleep_for`
  off-Windows or if the timer can't be created.
- **Never call `cap.set(CAP_PROP_FRAME_WIDTH/HEIGHT)` on a live GStreamer
  pipeline** — it reconfigures the pipeline caps and segfaults inside
  `gst_caps_new_simple`. The capture-resolution request is gated to USB devices;
  an RTSP camera dictates its own resolution, and any IP-side reframing belongs
  in the pipeline (`videoscale`), not `VideoCapture::set`.
- **The TensorRT EP's first-run engine build is minutes-long and
  non-interruptible.** It must be triggered from `EngineRegistry::warm_up()` on
  the main thread at startup, never lazily on a capture thread — the latter froze
  the UI and blocked stream `join()` on teardown (the reason TensorRT was dropped
  once before it was re-added behind the warm-up). Later runs load the cached
  engine from `models/trt_cache/`.
- **IP-camera latency depends on the GStreamer decode plugins being installed.**
  Without them `cv::CAP_GSTREAMER` fails to open and `CameraStream` silently
  falls back to the buffering FFMPEG backend, and RTSP lag returns — install the
  `gst-plugins-{base,good,bad}` + `gst-libav` packages (see `CLAUDE.md`).
