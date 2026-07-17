# Architecture

Depth reference for Denso-DigitalReader (C++ / Qt Widgets / CMake). For the
quick map and commands, see the root `CLAUDE.md`.

**Verifying on real hardware:** the parts of this design that only exist on the
target — native TensorRT on `sm_87`, NVDEC/GStreamer, the Linux network backend,
and (for deployment) a real GDM session — cannot be proven on the Windows dev
box. The shared device registry at **`d:\workspace\devices.md`** (outside this
repo) has the Jetson's address, credentials, AnyDesk ID and toolchain versions.

## Project layout

Split by concern, wired by a thin top-level `CMakeLists.txt` via
`add_subdirectory`. The app side is the `denso` exe plus three pure static
**subsystem libs** that both `denso` and `denso_tests` link:

| Path | Target | Depends on |
|---|---|---|
| `src/core/` | `denso_core` (static lib) | `Qt6::Core`, `Qt6::Sql` only |
| `src/app/detection/` | `denso_detection` (static lib) | `denso_core`, `Qt6::Core`, OpenCV |
| `src/app/brazing/` | `denso_brazing` (static lib) | `denso_core`, `Qt6::Core` |
| `src/app/camera/` | `denso_camera` (static lib) | `denso_core`, `denso_detection`, `Qt6::Core`/`Gui`, OpenCV |
| `src/app/` | `denso` (Qt Widgets GUI exe) | the three libs + `Qt6::Widgets`/`Multimedia`/`Network` |
| `tests/` | `denso_tests` (Catch2) | `denso_core` + the three subsystem libs |

Final lib graph: `denso_core ← {denso_detection, denso_brazing, denso_camera
(→denso_detection)} ← denso`. The libs hold the **pure, portable** logic
(inference helpers, reporting logic, non-widget capture infra); backend-coupled
engines, Qt-Network transport, and Qt Widgets stay in `denso`. Linking (rather
than re-compiling the `.cpp`s into `denso_tests`) means the tests validate the
same objects the app ships.

`denso_core` holds the ported logic + SQLite persistence **and** the Qt-free
domain↔view boundary (`core/ui/convert` + `viewmodel`). It never links
`Qt6::Widgets`, so the GUI cannot leak into the testable core. Each target's
directory is its own include root: core headers read `network/model.h` /
`ui/convert.h`, the app's widget headers read `ui/theme.h` /
`ui/camera/camera_dialog.h`, and the app reaches core headers through
`denso_core`'s public include dir.

`src/core/reading/` is the append-only detection-reading log (migration v9);
see [Reading log](#reading-log) below. On the app side, `src/app/ui/common/`
is a **leaf**: shared dialog chrome (header, async runner, label/row
factories) with no feature dependencies, so `settings_dialog` and
`camera_dialog` build on it instead of each keeping its own copies.

`src/app/ui/camera/` now holds **widgets only**; the non-UI capture, detection,
and brazing subsystems live in the sibling `src/app/{camera,detection,brazing}/`
dirs (the subsystem libs). Note two `camera/` dirs coexist — `src/core/camera/`
(domain) and `src/app/camera/` (runtime) — resolved by filename through the
separate include roots. Bounded 24/7 file logging lives in `src/app/logging/`
(see [Logging](#logging)).

## Boot sequence (`src/app/main.cpp`)

A thin orchestrator:

0. **Headless dispatch, before any Qt application object exists.** `cli::parse`
   (pure, `src/core/cli/`) turns argv into a `Command`; if the mode is headless,
   `main` constructs a **`QCoreApplication`** — never `QApplication` — and hands
   off to `app::run_headless` (`src/app/cli/`). This ordering is the point:
   `QApplication` loads the xcb platform plugin, so an installer running
   `--check` from a display-less root shell would die before reaching it. See
   [Headless modes](#headless-modes).
1. `QApplication` is constructed for the GUI path. It must come first among the
   GUI steps, because `paths::data_dir()`'s fallback needs `applicationDirPath()`.
2. **The single-instance lock is acquired** (`instance::SingleInstance` over
   `paths::lock_file()`) — *before* the DB, the log sink, or any camera. A second
   instance prints to stderr, shows a message box, and exits **3**. The guard is
   an automatic local, deliberately not `static`: it must be destroyed before the
   `QApplication` above it and before the static log sink it may log through.
3. The bounded file-logging handler is installed (`src/app/logging`, see
   [Logging](#logging)) so every later step's `qDebug/qWarning` lands in
   `denso.log` — the Windows GUI subsystem has no console. Then
   `QLocale::setDefault(English/UnitedStates)` forces Western Arabic digits in
   numeric widgets (`QSpinBox`/`QDoubleSpinBox`) regardless of the OS regional
   format — without it a Thai-locale host renders spin-box values as Thai
   numerals (๐–๙).
4. `db::Db::open(paths::db_file())` opens `denso.db` **in the data dir** (see
   [Mutable paths](#mutable-paths)) in WAL mode; `db::run_migrations` applies the
   `user_version`-gated chain. Both failures `qCritical` + `return 1` rather than
   `qFatal`: `qFatal` aborts without unwinding, which would strand the lock file
   with a dead pid and make the operator's next attempt report "already running"
   instead of the real cause.
5. `settings::import_legacy` does a one-time import of any pre-SQLite
   `settings.json` in the data dir (`paths::legacy_settings_json()`).
6. `ui::sync_models` scans the data dir's `models/` (`paths::models_dir()`) and upserts each into
   the `model` catalog, so models dropped there become selectable in the camera
   wizard. **Platform-split** (`model_sync.cpp` — the ONNX branch is
   `#ifdef _WIN32`): Windows catalogs `*.onnx` with class names from the ONNX
   metadata; Linux/Jetson catalogs `*.engine` with class names from a
   `<stem>.names.json` sidecar, skipping any engine whose sidecar is missing or
   unparseable.
7. `network::reassert` re-applies every saved interface config to the OS — the
   app is the source of truth. Best-effort and non-fatal: failures are logged
   via `qWarning`, never block startup. It is **deferred to the first event-loop
   tick** (`QTimer::singleShot(0, …)`, exceptions swallowed) and runs *after*
   `ui::launch`, so the window shows first — a slow or stuck OS CLI (now bounded
   by the backend's `QProcess` timeout) can no longer keep startup from painting.
8. `settings::load` seeds an in-memory `std::shared_ptr<Settings>`.
9. `main` hands off to `ui::launch(app, conn, state)` (`src/app/ui/startup.cpp`),
   which builds the shared `EngineRegistry`, then picks the launch UX via
   `ui/startup_mode`'s `cold_start_needs_splash` — which is **platform-split**,
   so the cold/warm split below is the *Windows* story:

   - **Windows/ORT** (`startup_mode.cpp:36-40`): **Cold** = `*.onnx` present and
     no cached `*.engine` → the first-run TensorRT build takes minutes, so show
     the splash. **Warm** = engine cached → no splash.
   - **Linux/Jetson** (`startup_mode.cpp:47-48`): engines are prebuilt, so
     there's no multi-minute build — but deserialize + warm-up still costs a few
     seconds per model, so it splashes **whenever a `*.engine` exists**. The
     ORT-cache notion of "already warm" does not apply (`cache_dir` is ignored).
     A Jetson with models configured therefore takes the splash path normally.

   **Cold path:** show the `StartupScreen` splash, warm every model on the worker
   while it animates, and on `finished` build + show `MainWindow` (with
   `warmup = nullptr`, so `CameraGrid` starts every camera immediately on
   cache-hits). **Warm path (UI-first):** build + show `MainWindow` immediately
   with a `WarmupState`, then `WarmupState::start()` warms in the background and
   `CameraGrid` starts model-less/ready cameras at once and each pending detection
   camera as its models come ready — no splash.

`Db` (an `optional<Db>` in `main`) outlives the window, so the connection it
hands the UI stays valid for the whole run.

## Mutable paths

`denso::paths` (`src/core/paths/`) is the **only** place that decides where
mutable state lives — database, log + rotated siblings, models, TRT cache, lock,
legacy `settings.json`. It resolves `$DENSO_DATA_DIR` when set and non-empty,
else falls back to `applicationDirPath()` (the historical behavior, so the
Windows dev box and the test suite are unchanged), else `"."`.

Why it exists: an installed build's program dir is root-owned and is **replaced
on upgrade**, so state kept beside the executable would be unwritable at runtime
and destroyed by every package upgrade. The deployment launcher points
`DENSO_DATA_DIR` at `/opt/denso/data`. Derived paths use `QDir::filePath`, not
concatenation — `QDir::cleanPath` keeps the separator on a filesystem root, so
`"/" + "/denso.db"` would yield `"//denso.db"`.

## Headless modes

Four CLI modes, all dispatched **before** `QApplication` (step 0 above) and run
under a `QCoreApplication`, so none needs a display. They are the gates the
`.deb` installer and `denso-setup` call (see the deployment spec in
`docs/superpowers/specs/`).

| Mode | Contract |
|---|---|
| `--version` | prints `APP_VERSION`; takes no lock; no mutation |
| `--check [--engine <file>]...` | validates the data dir + every model the DB references **and** each `--engine` named; **no persistent mutation** |
| `--check-running` | liveness; **the sole mode that takes the lock** — answering requires `tryLock` |
| `--check-migrations <db-path>` | runs the migration chain against **that path only** |

Exit codes (Slice 2's maintainer scripts depend on these): **0** ok — and for
`--check-running`, *an instance is running*; **1** failed — and for
`--check-running`, *nothing is running*; **2** bad usage; **3** the GUI refused
to start because another instance holds the lock.

`--check` deliberately does **not** reuse the normal startup path:
`EngineRegistry::warm_up()` creates the TRT cache dir and `Db::open()` runs
`PRAGMA journal_mode = WAL` — both mutations. It instead opens the DB read-only
(`Db::open_read_only`, which never creates an absent file) and constructs
`BackendEngine` **directly** against a throwaway `QTemporaryDir` cache, so
`OrtEngine` cannot write the real `trt_cache` either. It validates names via
`engine.class_names()` rather than parsing a sidecar, because the backends source
them differently: `TrtEngine`'s ctor reads `<stem>.names.json` itself and throws
without it, while a Windows `.onnx` has no sidecar at all (names live in the ONNX
metadata). Note `TrtEngine::ok()` is a hardcoded `return true` — on the Jetson
the ctor throwing is the only real signal, so both are checked.

A **missing** database is an empty configured-model set (a fresh DB references no
cameras, so it legitimately requires no engines) and is never created; a
**present-but-unreadable** one is a hard failure. Keeping those distinct is why
`detection::try_attached_model_filenames` returns an `optional` — the older
`attached_model_filenames` returns `{}` for both, which would let a corrupt
database pass as a fresh install.

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

**Common (`ui/common/`)**

A **leaf**: three shared dialog primitives, Qt-only with no feature
dependencies, so either dialog can build on them without depending on the
other. `dialog_chrome.{h,cpp}` (`dialog_header`) builds the consistent modal
title bar; `async_runner.{h,cpp}` (`run_on_worker`/`post_to_gui`) wraps the
worker-thread-then-marshal-back pattern each dialog's threaded operations
(scan/connect/refresh, snapshot capture) already needed; `form_widgets.{h,cpp}`
(`eyebrow`/`dim_label`/`spec_row`/`hline`) are the small label/row factories
that used to be copy-pasted between `settings_dialog`'s anonymous namespace and
the camera dialog's `page_util`. `page_util::dim_label` now delegates to
`common::dim_label` instead of keeping its own definition.

**Settings (`ui/settings/`)**
- `settings_dialog.{h,cpp}` — modal: a left nav over five panels (Appearance,
  Display, System, Network, About). The Network panel is extracted into
  `network_panel.{h,cpp}` (`NetworkPanel`), a self-contained widget that owns
  the two `NetCard`s, the DB handle, and the threaded apply/scan/connect/refresh
  handlers (`on_shown()` re-seeds editors + refreshes status, reproducing the
  Slint original's "entering the tab reloads"); `settings_dialog` itself is now
  a thin view over the nav + the four other panels.
- `netcard.{h,cpp}` — one interface's live status + editable IP/DNS config +
  (Wi-Fi) scan list with per-row connect.

**Camera (`ui/camera/` widgets + the `src/app/camera/` runtime lib)**

`ui/camera/` now holds **widgets only**, under two root entry points
(`camera_view`, `camera_dialog`) plus the `wizard_controller`. **`grid/`** is the
live-view widgets (`camera_grid`, `camera_tile`, `grid_layout`). **`dialog/`** is
the modal internals (the five page widgets + `page_util`, plus the dialog-only
`wizard_stepper`, `roi_canvas`, and the source scanners `camera_devices`,
`ip_scan`). **`shared/`** is now just `roi_geometry` (the last cross-cutting UI
primitive). The **non-widget runtime** — `frame_processor`, `fps_meter`,
`stream_pacing`, `warmup_gate`, `zone_assembly`, `safe_process.h`, `snapshot`,
`frame_convert`, `rtsp_templates`, `gst_pipeline` — moved to the sibling
`src/app/camera/` dir (the `denso_camera` lib); `camera_stream` (a `QObject`)
also moved there in path but stays compiled into `denso`. Dependencies flow one
way: the runtime lib + `shared/` are leaves; `grid/` and `dialog/` depend only on
them, never on each other; the root entry points compose all three.

- `camera_view.{h,cpp}` — the main content area: a switcher between the empty
  "no cameras" state (+ Add) and the live **`CameraGrid`**. `release_streams()`
  stops capture while the Camera modal is open (so its snapshot can claim the
  same USB device); `reload()` rebuilds + restarts when the modal closes.
- `camera_grid.{h,cpp}` / `camera_tile.{h,cpp}` / `camera_stream.{h,cpp}` —
  the live 1–4 feed grid. `CameraGrid` lays out one `CameraTile` + one
  `CameraStream` per camera (first four by id; `grid_dims` picks 1 / 1×2 / 2×2).
  Each `CameraStream` runs a `cv::VideoCapture` read loop on its **own
  `std::thread`**, converts each frame (`mat_to_qimage`), runs it through a
  `FrameProcessor` (wrapped in `safe_process` so a throw from a malformed frame
  can't kill the capture thread), and emits `frame_ready`/`status_changed` as
  **queued** signals to its tile (capped ~15 fps; finite open/read timeout so a
  dead camera can't hang teardown; `stop()` joins). `run()` is an **outer
  reconnect loop**: a failed open or a mid-stream read drop no longer ends the
  thread — it emits Offline, backs off (`next_backoff_ms`: 1s→×2→10s cap, reset
  on a live frame) via a stop-responsive `wait_or_stop`, and reopens, so a camera
  that blinks out recovers on its own without an app restart. Each source opens
  via a **capture-backend ladder** — the first candidate that opens AND reads a
  frame wins (remembered for reconnects): RTSP through hardware **NVDEC**
  `rtsp_gst_pipeline` (H.264 → H.265) then FFMPEG; USB MJPEG → YUYV → `CAP_ANY`.
  Mixed-codec fleets auto-discover; GStreamer drops stale frames so
  glass-to-glass lag stays bounded. The
  display cap is paced by a high-resolution waitable timer (`precise_sleep`),
  because MinGW's `std::this_thread::sleep_for` is pinned to the ~15.6 ms OS
  tick and would undershoot the target rate (~9 fps for a 15 fps cap). The
  loop's flow-control policy is factored into pure, unit-tested helpers in
  `stream_pacing.{h,cpp}`: `next_backoff_ms` (reconnect backoff schedule) and
  `should_emit` (drop-oldest backpressure gate). Backpressure is a
  `shared_ptr<atomic<int>>` in-flight counter shared by the stream and its tile:
  the stream only emits when `should_emit(queued, kMaxInFlight=2)` and increments;
  `CameraTile::set_frame` decrements on consume. A GUI that falls behind drops
  frames instead of letting full-res `QImage` events pile up unboundedly (OOM).
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
  orientation-only impl, and `DetectionProcessor` layers detection on top,
  **decoupled from display**: it orients on the display path but runs the platform
  inference backend (ORT or native TensorRT) on a worker thread over a drop-oldest
  latest-frame slot, overlaying the newest boxes (per-class confidence filter →
  ROI confinement → labelled boxes). The ROI step keeps only boxes whose normalized centre lands
  inside one of the camera's area polygons (`camera::inside_any_area`); a camera
  with no areas detects the whole frame.
  `camera_grid` picks per camera: `DetectionProcessor` when the camera has
  attached, loadable models (resolved via `detection::detection_for` +
  `EngineRegistry`), else plain `OrientationProcessor` — the capture loop and
  tile don't change. Every per-frame `process()` call is wrapped in
  `safe_process()` (`safe_process.h`) so a throw from a malformed frame is caught
  on the capture thread and the raw frame is shown instead — one bad frame can't
  `std::terminate()` the process. `grid_layout.{h,cpp}` is the pure, unit-tested
  `grid_dims(n)`.
- `camera_dialog.{h,cpp}` — the camera management hub: a thin **view** over a
  5-page stack run as a guided wizard — list + delete, then **① Source**
  (USB auto-scan, or IP via manufacturer + main/sub stream + credentials with a
  live RTSP-URL preview) → **② Configure** (snapshot preview + resolution / fps /
  rotation / pitch / roll) → **③ Models** (attach 1..N detection models, each with
  per-class confidence) → **④ Areas** (draw ROI polygons). Each page is its
  own widget under `dialog/` (see below), owning its controls and emitting
  request signals; the dialog itself owns only the page stack, the
  `WizardStepper`, and modal sizing (Back/Next/Finish footers; `show_page(index)`
  switches the stack page, drives the stepper, and resizes — the modal grows to
  **near-fullscreen on the Areas step** for drawing room and restores the
  compact size on leaving). All flow-state, the threaded snapshot capture, and
  every DB write (camera insert/update, model attach, ROI replace) live in
  `wizard_controller.{h,cpp}` (`CameraWizardController`, a `QObject`, not a
  widget): the controller never touches the `QStackedWidget` or stepper
  directly — it drives page transitions through an injected `show_page`
  callback and a `request_show_list()` signal for "return to the list", and
  emits `cameras_changed()` for the main view to refresh. The camera is
  inserted/updated when Configure's **Next** is pressed, so both the Models and
  Areas steps attach to a known camera id; Models persists via
  `detection::set_camera_models` on its Next, Areas is **optional** (Skip
  returns without writing ROIs, Finish saves them). Each list row also has an
  **Areas** button to draw/edit later. `showEvent` reopens the reused dialog on
  the list at compact size. Persists through `camera::repo` + `detection::repo`.
- **Editable source + ROI quarantine.** The Source page is editable on an
  existing camera, not just on add. Because moving the lens or swapping the feed
  invalidates ROIs drawn against the old view, `wizard_controller` diffs the edit
  with `camera::requires_area_review` (pure `source_change` logic:
  `same_effective_source` + `view_geometry_changed`) and, when it changed the
  effective source or capture geometry, sets `camera.areas_need_review`
  (migration **v11**). While that flag is set the live grid **excludes those ROIs
  and pauses zone reporting** — the tile shows an "Areas need review" banner
  (`CameraTile::set_review_paused`). Clearing it requires the operator to save the
  Areas step against a **valid live preview** of the new source (saving with no
  preview is refused, so ROIs are never "verified" blind). A cosmetic edit
  (name / credentials) leaves any existing flag untouched. The Source page also
  tags devices already owned by another camera **"(in use)"** (the controller
  pushes the used IP/USB set via `push_used_sources()` before each scan), and the
  Models page has **Select all / Clear** buttons for a model's class list.
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
  its own). The Areas page validates before it emits, so a predictable problem
  is named there instead of arriving as a generic write failure from
  `replace_areas`: an unresolved draw blocks the save, degenerate and
  self-intersecting polygons are refused, and zone clashes are reported with
  their holder (the picker disables zones held by other cameras —
  `zones_owned_by_other_cameras` — and by this camera's other areas). Drawing is
  a locked sub-task: the list and "+ New area" disable until the shape is
  finished or cancelled. Anything that discards work confirms first, including
  `CameraDialog::reject()` so Escape and the window's X can't slip past the
  guard the Back/Exit buttons apply.
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
- `roi_canvas.{h,cpp}` + `roi_geometry.{h,cpp}` — the **Areas** page's editing
  surface. `RoiCanvas` paints the oriented snapshot (reusing `apply_orientation`,
  so ROIs sit on exactly the configured view) plus the camera's *other* areas,
  dimmed and zone-labelled, so coverage and overlap are visible while editing.
  Three modes: **Idle** (view only), **Drawing** (click to add a vertex; click
  the ringed first vertex / double-click / Enter / "Done shape" to close), and
  **Editing** (drag a corner, click an edge to insert one, tap a corner then
  "Remove corner" to drop it — never below 3). It holds vertices **normalized to
  [0,1]** and knows no DB policy — the page loads/persists.
  `roi_geometry` is the pure, unit-tested mapping it builds on: aspect-fit rect,
  widget↔normalized conversion, `image_contains`, `hit_test_vertex` (nearest
  wins), and `nearest_edge_insert_index` (distance to the segment, closing edge
  included).

  Two rules worth knowing before touching this. **A click is gated by
  `image_contains`; a drag is not.** The clamp in `to_normalized` is a safety
  net, not a gate — gating the click stops a tap in the letterbox bars becoming
  a vertex silently pinned to the frame's edge, while a drag that leaves the
  frame is the operator holding the handle and pulling, which reads as "put it
  on the edge". **Because a drag clamps, coincident vertices are manufactured,
  not a fluke:** two corners pulled past the same border land on the identical
  coordinate, and several pulled off one side end up collinear along it. That is
  why `camera::polygon_self_intersects` treats touching and collinear overlap as
  intersections, not just proper crossings — those shapes pinch the polygon shut
  while keeping a healthy area, so the area floor cannot catch them, and
  `point_in_polygon`'s even-odd fill would turn the lobes into holes.
- `wizard_stepper.{h,cpp}` — `WizardStepper`, the non-interactive
  "① Source — ② Configure — ③ Models — ④ Areas" indicator above the page stack;
  `set_current()` emphasizes the active step. Navigation stays with the dialog's
  Back/Next/Finish buttons.

### Threading

All four blocking OS calls — `apply_net_config`, `scan_wifi`, `connect_wifi`,
`refresh_network` — run on a worker `QThread` (`QThread::create`, so QProcess in
the backends has an event dispatcher) and post results back with
`QMetaObject::invokeMethod(this, …, Qt::QueuedConnection)`. This is the Qt
analog of the Rust `std::thread` + `upgrade_in_event_loop`. A fresh
`network::backend()` is created per operation. **Worker lifetime is guarded in
two layers** (a dialog can be closed mid-operation): `run_on_worker` returns the
`QThread*`, which `NetworkPanel` records in `workers_` and `wait()`s in its
destructor; and each worker captures a `QPointer<NetworkPanel>` and skips its
`post_to_gui` if the panel is already gone. Together they close the
use-after-free window on `this`. A per-panel `net_busy_` flag (set at each
handler's entry, cleared in its GUI post) plus a disabled Refresh button
serialize actions, so a rapid double-click can't spawn duplicate workers or
double-apply a config. Moving `apply_net_config` off the GUI thread means a
stuck `netsh` (now bounded by the backend's `QProcess` timeout) can no longer
freeze the UI.

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
`reassert` catches them into non-fatal `(iface, message)` pairs. Every `QProcess`
wait is **bounded** (`waitForFinished(15s)` → `kill()` + 2s grace), never
`-1` — a stuck OS CLI can't wedge the calling thread; `run` returns empty on
timeout, `run_checked` throws a timeout error.

## Detection feature (`src/core/detection/` + `src/app/detection/`)

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

**Runtime** (`src/app/detection/`, OpenCV + a platform-split inference backend —
ONNX Runtime on Windows, native TensorRT on Jetson; app target only). The pure,
backend-free helpers form the `denso_detection` lib (linked by both `denso` and
`denso_tests`); the backend engines + registry + `model_sync` stay in `denso`.
Pure unit-tested helpers: `letterbox` (aspect-preserving resize +
gray pad to 640, plus the inverse box map), `yolo_decode` (two decoders chosen by
output shape — `decode_yolo` for the raw transposed `[1, 4+nc, na]` head via
per-anchor argmax + confidence floor + class-agnostic `cv::dnn::NMSBoxes`, and
`decode_yolo_end2end` for an NMS-free `[1, N, 6]` output where the model already
did NMS so only a confidence floor + inverse box map remain), and
`names_metadata` (parse the ONNX `names` dict). These feed the `InferenceEngine`
interface, implemented **per platform** (selected via the `BackendEngine` alias in
`engine_registry.h`):

- **Windows / dev — `OrtEngine`:** one ORT session with a **TensorRT → CUDA →
  CPU** provider fallback. The TensorRT tier runs FP16 with a serialized engine
  cache (`models/trt_cache/`); its first-run build is minutes-long and
  non-interruptible, so `EngineRegistry::warm_up()` runs one blank inference over
  every `models/*.onnx` on the warm-up worker to absorb it off the hot path.
- **Jetson Orin Nano (real target) — `TrtEngine`:** native TensorRT (`nvinfer` +
  `cudart`) that **deserializes a prebuilt `.engine` only** (built on-device with
  `trtexec` for TRT 10.3 / `sm_87`) — never built at runtime, **no fallback**. A
  missing/incompatible engine **fails loud** (throwing ctor → `WarmupWorker` →
  `app.exit(1)`). Class names come from a `<engine>.names.json` sidecar; inference
  is mutex-guarded across cameras. `warm_up()` scans `models/*.engine`.

Each detection camera starts only after its models finish warming, so warm-up
never lands on a capture thread. `EngineRegistry` keeps one shared engine per
model filename (lazy; failed loads cached as `nullptr`). `model_sync` catalogs
`models/*.onnx` (Windows, names from ONNX metadata) or `models/*.engine` (Jetson,
names from the sidecar) at boot.

Streaming feeds these through a **capture-backend ladder** (`camera_stream.cpp`):
each source tries candidates and keeps the first that opens AND reads a frame —
RTSP via hardware **NVDEC** (`rtsp_gst_pipeline`: `nvv4l2decoder → nvvidconv →
BGR`, per-codec depay/parse) H.264 → H.265 → FFMPEG; USB MJPEG → YUYV → `CAP_ANY`.
Mixed-codec fleets auto-discover per camera; the leaky queue sits after the
decoder only. Inference is **decoupled from display** — `DetectionProcessor` runs
the model on a worker thread over a drop-oldest latest-frame slot and overlays the
newest detections snapshot, so video stays smooth regardless of model speed.

`CameraGrid` chooses per camera: it resolves `detection_for`, asks the registry
for each attached model, and constructs a `DetectionProcessor` (orient → infer →
per-class conf filter → ROI confinement → draw labelled boxes) when ≥1 model
loads, else a plain `OrientationProcessor`. `EngineRegistry::warm_up()` runs on
the warm-up worker thread in the background; `get()` is **mutex-guarded** because
it is now called from both that worker and the UI thread (`CameraGrid` starting a
camera whose models are ready — a cache-hit, never a build). Detection cameras are
gated on readiness (pure `warmup_gate` `PendingStart` + `WarmupState`), so the
engines outlive the capture streams and the build stays off them. ORT +
provider DLLs and every `models/*.onnx` are copied beside the exe by a
`POST_BUILD` step; the GPU provider DLLs come from the git-ignored
`third_party/gpu_ep/` (see `docs/GPU_SETUP.md`), and a missing GPU stack silently
degrades to the CPU provider.

## Reading log

`src/core/reading/` is the append-only log of captured readings, Qt/OpenCV-free
like the rest of `denso_core`: `reading.h` (`Reading`: `id` / `camera_id` /
`ts_ms` / `value` / `conf`) + `repo` (`insert`, and `query(camera_id, from_ms,
to_ms)` ordered by `ts_ms` then `id`). Schema is migration **v9** — a `reading`
table indexed on `(camera_id, ts_ms)` for the by-camera time-range read. It's
append + range-read only; there is no update/delete, since a reading is an
immutable capture.

The write side is not wired up yet. `DetectionProcessor` (`frame_processor.h`)
has a dormant seam for it: an optional `ReadingSink*` (plus a `camera_id`) on
`DetectionProcessor`'s ctor — now 8 params (`degrees, pitch, roll, models, areas,
camera_id, sink, zone_sink`), all passed at its sole call site
(`ui/camera/grid/camera_grid.cpp:195-200`). When a sink is
set, `infer_loop()` calls `sink->on_reading(camera_id, ts_ms, kept)` with the
frame's post-ROI-confinement detections — **not** `process()`, which only submits
the latest frame and overlays the last snapshot. **Threading contract:**
`on_reading`
runs on the **inference worker thread** (`frame_processor.h:59`; the call site is
in `infer_loop()`) — *not* the capture thread, since inference is decoupled from
display. An implementation must not
block or do DB I/O inline; it has to hand the data off to a worker (e.g. via
`common::run_on_worker`/`post_to_gui`) and return immediately, the same rule
`camera_stream` already follows for its own frame processing. Assembling a
sink's kept detections into a `Reading::value` (e.g. digit-string reconstruction
across models) is deferred to the future logging/export feature (Spec 2) — this
task only lands the storage + the capture-thread hook, not a consumer.

## Brazing zone reporting

Pushes each ROI's number to a backend as one combined JSON POST on change.
Config lives in `src/core/brazing/config` (`BrazingConfig{enabled, base_url}` over
the `settings` key/value table); each ROI (`camera_area`) carries a `zone` number
(migration **v10**, nullable — NULL = ROI-only, not reported).

Pipeline, all off the GUI thread until the final POST:

```
capture thread (per camera)
  DetectionProcessor::process
    → submit latest frame to the worker's slot (drop-oldest), overlay snapshot

inference worker thread (per camera)
  DetectionProcessor::infer_loop
    → kept digit boxes (existing detection)
    → group_into_zones(kept, areas, w, h)   [pure: assemble_zone_value sorts
                                             digits left-to-right → int, per zone]
    → ZoneReporter::on_zones(cam_id, zones) [mutex] → ZoneAggregator::observe
         per-zone debounce (kStableFrames=5) + change detection      [pure]
         if a stable value changed: post_to_gui(reporter, submit(snapshot)) ─┐ queued
GUI thread                                                                  ▼
  BrazingReporter::submit → BrazingRetryPolicy decides →
     Send  → BrazingClient::post → POST {base}/api/brazing/update (async, 5 s timeout)
              → done(ok): 2xx → delivered; else arm retry QTimer (1s→×2→30s cap)
     ArmRetry → retry timer → re-send the latest pending snapshot
```

The four pure units (`zone_assembly`, `zone_aggregator`, `brazing_payload`,
`brazing_retry_policy`) are unit-tested; the structural pieces
(reporter/client/wiring) are build + suite + on-device verified. **Delivery is
reliable, latest-value-wins**: every POST carries the full `{zone_no→value}`
snapshot, and `BrazingReporter` keeps retrying the newest snapshot (single-flight,
exponential backoff) until the server 2xx-acks it — so a downed server no longer
drops the last value. New readings coalesce into the pending snapshot in real
time; retry state is in-memory only (no outbox, no idempotency, no persistence —
unlike the DeepStream sibling). Lifetime is safe by teardown order:
`CameraGrid::clear()` stops and **deletes** every `CameraStream` before resetting
`reporter_` then `brazing_reporter_`. Joining the capture threads is *not* what
makes this safe — capture threads never call the reporter. Deleting the stream
destroys the `unique_ptr<FrameProcessor>` it owns (`camera_stream.h:51`), and
`~DetectionProcessor` signals + joins its **inference worker**
(`frame_processor.cpp:39`) — the thread that actually calls `on_zones`. That join
is what guarantees no worker can reach the reporter afterwards. Each
in-flight POST's completion callback is `QPointer`-guarded, so a POST that finishes
after the reporter is torn down is dropped rather than calling into a destroyed
object (it does not rely on transitive `QNetworkAccessManager`/reply ownership).

## Logging

`src/app/logging/` is a **bounded, 24/7-safe** file logger installed as a
`qInstallMessageHandler` at the top of `main` (the Windows GUI subsystem has no
console, so `qDebug/qWarning/qCritical` would otherwise vanish). Design goal: a
box that runs for months must never fill the disk or lose the tail.

- `RotatingLogSink` (`log_sink`) writes to `denso.log` beside the exe and rolls
  at a size cap — **5 files × 5 MiB ≈ 25 MiB total**. Roll = close → shift
  `denso.log.N` → reopen, all under a mutex. Each record is truncated to
  ≤16 KiB, and `write()` is `noexcept` with a catch-all so a logging failure can
  never propagate into the caller. On a disk-full / write error it falls back to
  `stderr`, marks itself degraded, and keeps retrying the reopen.
- `log_rotation` is the **pure, unit-tested** size/roll policy (which file, when
  to roll, how to renumber) — no I/O, so it's testable off-device.
- `log_throttle` (`LogEpisode`) collapses a repeating condition (e.g. a flapping
  camera reconnecting) to a single "opened" + single "closed" line plus a
  periodic count, so a stuck fault can't flood the file.
- `redact` (`sanitize_url`) strips credentials from RTSP URLs before they reach
  the log.
- `qSetMessagePattern` bakes local ISO time + the (fixed-at-start) UTC offset +
  level + category + pid + tid into every line; `DENSO_LOG_LEVEL` sets the
  severity floor; a `SESSION` banner at startup and a 5-minute `HEARTBEAT` line
  mark liveness for after-the-fact log reading.

## Gotchas

- **Never `git add -A` in this repo.** `denso/yolo26n/yolov8n.onnx` are tracked,
  and the 37 MB `digitv2.onnx` + `note.txt` are now git-ignored — but `.gitignore`
  names `models/digitv2.onnx` **specifically**, not `models/*.onnx`, so a *new*
  model dropped into `models/` is untracked and unignored. A blanket add still
  sweeps a multi-MB blob into a commit and then blocks `git pull` on the Jetson
  (the untracked file conflicts with the incoming one). Use explicit
  `git add <files>` / `git add -u`.
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
- `denso.db` (+ `-wal`/`-shm`) is created in the data dir (`denso::paths`) at runtime and
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
  off-Windows or if the timer can't be created. The `thread_local` timer handle
  is held in a small RAII struct so its `CloseHandle` runs at thread exit — one
  kernel handle per capture thread was leaking on every grid reload before.
- **Never call `cap.set(CAP_PROP_FRAME_WIDTH/HEIGHT)` on a live GStreamer
  pipeline** — it reconfigures the pipeline caps and segfaults inside
  `gst_caps_new_simple`. The capture-resolution request is gated to USB devices;
  an RTSP camera dictates its own resolution, and any IP-side reframing belongs
  in the pipeline (`videoscale`), not `VideoCapture::set`.
- **The TensorRT EP's first-run engine build is minutes-long and
  non-interruptible.** It must be triggered from `EngineRegistry::warm_up()` on
  the warm-up worker thread — never lazily on a capture thread, which froze the UI
  and blocked stream `join()` on teardown (the reason TensorRT was dropped once
  before it was re-added behind the warm-up). Startup splits by whether that build
  is needed (`ui/startup_mode`): a **cold** start warms behind the blocking
  `StartupScreen` splash before the window (and any capture thread) exists; a
  **warm** restart is UI-first, creating each detection capture thread only after
  its models finish warming so `get()` there is a cache-hit. Later runs load the
  cached engine from `models/trt_cache/`.
- **IP-camera latency depends on the GStreamer decode plugins being installed.**
  Without them `cv::CAP_GSTREAMER` fails to open and `CameraStream` silently
  falls back to the buffering FFMPEG backend, and RTSP lag returns — install the
  `gst-plugins-{base,good,bad}` + `gst-libav` packages (see `CLAUDE.md`).
