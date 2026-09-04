# Denso-DigitalReader

Desktop app (C++ / Qt Widgets / CMake) for reading a 4-digit 7-segment display,
with a settings UI for display resolution, theme, hardware spec, and network
configuration. Single SQLite store (`denso.db`) in the data dir — beside the
executable by default, or wherever `$DENSO_DATA_DIR` points (see `denso::paths`).

Ported 1:1 from a Rust + Slint original. The port has landed — `main` **is** the
C++/Qt app; the `port/cpp-qt` branch no longer exists.

## Commands

Out-of-source build with CMake (needs Qt6: Core, Gui, Sql, Widgets, Multimedia,
Network; OpenCV; and a detection backend). The inference backend is
**platform-split** (see *Detection / inference backend* below): Windows/MSYS2 dev
uses ONNX Runtime; the Jetson Orin Nano deployment target uses native TensorRT.
The dev build runs on the MSYS2 UCRT64 toolchain (`pacman -S
mingw-w64-ucrt-x86_64-qt6-base mingw-w64-ucrt-x86_64-qt6-multimedia
mingw-w64-ucrt-x86_64-opencv mingw-w64-ucrt-x86_64-cmake
mingw-w64-ucrt-x86_64-ninja`); ONNX Runtime is provisioned into
`third_party/onnxruntime/` (git-ignored — see `docs/GPU_SETUP.md`). For the Jetson
build/deps see `AGENTS.md`.

Camera capture uses OpenCV's **GStreamer** backend via a capture-backend LADDER
(`gst_pipeline.cpp` + `camera_stream.cpp`): it tries hardware **NVDEC** pipelines
and keeps the first that opens AND reads a frame — RTSP: NVDEC **H.264 → H.265**
→ FFMPEG; USB: MJPEG (`nvv4l2decoder mjpeg=1`) → YUYV → `CAP_ANY`. So mixed
H.264/H.265 fleets auto-discover per camera, and the leaky queue sits **after**
the decoder only (dropping compressed access units would corrupt the stream). On
the Jetson the `nv*` elements ship with JetPack (plus `gstreamer1.0-libav` for
the FFMPEG/avdec fallback). On Windows the `nv*` elements are absent, so the
NVDEC candidates fail to build and the ladder falls through to FFMPEG — install
`pacman -S mingw-w64-ucrt-x86_64-gst-plugins-{base,good,bad}
mingw-w64-ucrt-x86_64-gst-libav`. Inference is decoupled from display (a worker
thread + latest-frame slot; boxes overlaid from a snapshot), so video stays
smooth regardless of model speed.

| Action | Command |
|---|---|
| Configure | `cmake -S . -B build -G Ninja` |
| Build | `cmake --build build` |
| Test | `ctest --test-dir build` |
| Run | `./build/src/app/denso` (path varies by generator) |

**Real-device testing** (Jetson Orin Nano — anything needing `sm_87`, real
TensorRT, NVDEC, or a GDM session): host/credentials/AnyDesk + toolchain versions
are in **`d:\workspace\devices.md`**, the shared registry outside this repo.
Check there before re-deriving how to reach a box.

**Catch2 test names are CLI arguments.** `catch_discover_tests` re-invokes the
binary with each case's literal name, so names must be **ASCII** and must never
start with `--`; a `→` or a leading dash arrives mangled and the case reports
Failed while its logic is fine.

Tests are Catch2 v3, fetched via `FetchContent` at configure time (needs net on
first configure). Platform backend tests are compiled per-OS, so the passing
count differs between Windows and Linux.

## Layout

Wired by a thin top-level `CMakeLists.txt` (`add_subdirectory`). The app is split
into `denso_core`, the `denso` exe, and **three static subsystem libs**
(`denso_detection` / `denso_brazing` / `denso_camera`) that both `denso` and
`denso_tests` link (see `src/app/` below):

| Path | Target | Role |
|---|---|---|
| `src/core/` | `denso_core` (lib) | Ported logic + SQLite persistence + the Qt-free domain↔view boundary. `Qt6::Core`/`Sql` only. |
| `src/app/` | `denso` (GUI exe) + `denso_detection`/`denso_brazing`/`denso_camera` (libs) | Qt Widgets UI + entry-point orchestrator + the pure detection/brazing/camera subsystem libs. `Qt6::Widgets`. |
| `tests/` | `denso_tests` | Catch2 unit tests over `denso_core` + the three subsystem libs (linked, not re-compiled). |

Each target dir is its own include root, so includes read `network/model.h`,
`ui/convert.h`, `ui/theme.h`, etc.

Two trees sit **outside** the CMake graph and so outside `ctest`:

| Path | Role |
|---|---|
| `packaging/`, `tools/` | The POSIX-shell `.deb` ship pipeline: `tools/build_package.sh` (the whole build), `packaging/lib/` (`policy.sh` = the JetPack-damage rule, the forward-only **`db_upgrade_gate`**, the canonical-model-set rule + the `gen_preflight.sh`/`gen_bundle.sh`/`gen_payload.sh` emitters), `packaging/denso-db-helper` (Python 3 stdlib `sqlite3`: online backup / `user_version` / `integrity_check` — **no `sqlite3` CLI dependency**), `packaging/denso-setup`, `packaging/debian/`. |
| `tests/packaging/`, `tests/manual/` | Their harnesses: `run.sh` (312 assertions natively on the Jetson; the file-mode ones are Linux-only and skip elsewhere) and the Jetson-only `repro_build.sh` (19 — proves clean builds are byte-reproducible). |

Run `tests/packaging/run.sh` for any packaging change — a green `ctest` says
nothing about that tree. See **Packaging & ship pipeline** in
`docs/ARCHITECTURE.md` for the design, AGENTS.md for the operator runbook.

### `src/core/` (library)

| Path | Responsibility |
|---|---|
| `db/db.{h,cpp}` | SQLite base (`denso.db`, WAL) + the `user_version`-gated migration chain in `run_migrations`. `run_migrations` **REFUSES** (returns false, mutating nothing) a DB whose `user_version` exceeds `supported_schema_version()` — an older build must never migrate or silently downgrade a DB written by a newer one. `read_user_version()` + `supported_schema_version()` back the read-only boot/`--check` preflight `health::evaluate_db_schema` (SchemaNewer / DbUnopenable). |
| `settings/` | Persisted app settings (window size, theme, fullscreen) + resolution presets. `settings`=type/presets, `repo`=persistence + legacy import. |
| `hardware/` | Read-only host spec via `QSysInfo`/`QStorageInfo` (collected fresh, never stored). `format`=byte formatting, `collect`=the spec. |
| `network/` | `NetworkBackend` base + `reassert` + `NetConfig`/status types + config persistence (`repo`). |
| `network/windows/{netsh,wifi,parse}.*`, `network/linux/nmcli.*` | Pure, unit-tested OS-command helpers (compiled on every OS for off-device testing). |
| `network/windows/windows_backend.cpp`, `network/linux/linux_backend.cpp` | OS backends (`QProcess`); one compiled per platform. Every CLI wait is bounded (`waitForFinished(kNetCmdTimeoutMs=15000)` → `kill()` + `kNetCmdGraceMs=2000` grace) so a stuck `netsh`/`ipconfig`/`nmcli` can't hang the calling thread forever — `run` returns empty, `run_checked` throws a timeout error. |
| `ui/convert.{h,cpp}`, `ui/viewmodel.h` | The **only** domain↔view boundary (Qt-free, testable). |
| `camera/` | Camera inventory: domain structs (`camera.h`: `Camera` — incl. `areas_need_review` — + polygon `CameraArea` over normalized `Point`s) + persistence (`repo`, full camera CRUD + ROI-area read/replace + `set_areas_need_review`). `area_points` (de)serializes a polygon's normalized vertices to the `camera_area.points` TEXT column; `area_geometry` is pure, unit-tested point-in-polygon (`point_in_polygon`/`inside_any_area`) used to confine detection to the ROI. `area_validation` is pure, unit-tested ROI-set validation the **UI** runs before saving (`polygon_area`, `polygon_is_degenerate`, `polygon_self_intersects`, `find_zone_conflict`, `areas_equal`) — `replace_areas` still gates integrity, but it can only answer yes/no, so these let the Areas page name the problem instead of showing a generic failure. `source_change.{h,cpp}` is pure, unit-tested edit-diff logic (`same_effective_source`/`view_geometry_changed`/`requires_area_review`) that decides when a camera edit invalidates its ROIs, plus `view_revision` — an opaque SHA-256 fingerprint of exactly those view-significant fields, whose contract is that two cameras share one precisely when `requires_area_review` is false. It is what `ball_level_calibration.view_revision` stores, so "what makes a view different" has ONE definition; hashed because `rtsp` is operator-editable and could otherwise carry a credential into the database. `camera.areas_need_review` is migration **v11**; `camera.setup_complete` is **v12**. `all()` returns EVERY row (management: list, edit, delete, duplicate-source detection); **`runtime()` is the ONE decision of what may stream** (`active AND setup_complete`, filtered in SQL) — never re-derive it with a local `if (active)`. `mark_setup_complete()` requires exactly one affected row (`exec()` succeeds on an UPDATE matching nothing). |
| `detection/` | Per-camera detection config domain: `detection.h` structs (`DetectionModel`, `CameraModel`, `ModelClassSelection`, resolved `CameraDetection`) + persistence (`repo`: model catalog, per-camera attachments, and the `detection_for` resolve query) + `class_names` JSON (de)serialization. Qt/OpenCV-free — the ORT inference runtime lives in the app. |
| `reading/` | Append-only detection-reading log: `reading.h` (`Reading`: camera_id + ts_ms + value + conf) + `repo` (`insert`/`query` by camera + time range). Consumed by the logging/export feature; written by the app-side reading sink. Migration **v9**. |
| `brazing/` | Brazing HTTP-reporter config: `config.{h,cpp}` (`BrazingConfig{enabled, base_url, api_path}` + `load`/`save`) over the `settings` key/value table (keys `brazing.enabled`/`brazing.base_url`/`brazing.api_path`). A **missing or blank `brazing.api_path` row resolves to `brazing::kDefaultApiPath` (`/api/brazing/update`)** — that is the whole backward-compatibility mechanism for pre-setting installations, and it needed **no migration** because `settings` is key/value. Qt-free of widgets. Per-ROI `zone` number lives on `camera_area` (migration **v10**). |
| `brazing/url.{h,cpp}` | The **ONE** authority on where a report goes: `kDefaultApiPath`, `normalize_base_url(input, api_path)`, `normalize_api_path(input)`, `endpoint_url(base, api_path)` — the single composition site — and `split_base_url`/`compose_base_url`, the ONLY bridge between the stored `brazing.base_url` and the Settings page's protocol/address/port controls (a VIEW of that row; **no per-control column and no migration**, and every accepted base URL round-trips byte-for-byte). The Settings form, its live effective-endpoint preview, the `CameraGrid` gate and `BrazingClient` all go through it; never add a more lenient copy. The base URL's paste tolerance strips the **configured** path, not a constant. Either half being unusable yields an EMPTY endpoint (do not POST), never a partial URL and never a silent fallback to the default. |
| `mode/` | **Operating mode** (which job the appliance does): `mode.h` (`TargetMode{DigitReader,BallLeveler}` + `to_string`/`parse_target_mode`/`from_index` — any absent/unknown/corrupt token resolves to `digit_reader`, **never** the newer mode), `config.{h,cpp}` (the `mode.target` key over the `settings` table + the `mode_setup_required` predicate), and `reset.{h,cpp}` (`switch_mode` — the ONE atomic switch transaction; the destructive `switch_and_reset` + `preview_counts` were DELETED in schema-v14 Slice 1). The `mode.target` key rides `settings`, and is deliberately NOT a `settings::Settings` field (Reset-to-defaults would wipe it). A switch is **NON-DESTRUCTIVE: it deletes nothing** — all 20 `camera` columns survive (**including `setup_complete`/`areas_need_review`**, which the old destructive switch reset), and both modes' configuration persists, so `runtime()` is NOT empty afterwards. See **Operating modes** in `docs/ARCHITECTURE.md`. |
| `util/strutil.h` | Small shared string helpers. |

### `src/app/` (GUI + subsystem libs)

The `denso` exe plus **three static subsystem libs** that both `denso` **and**
`denso_tests` LINK — so the unit tests exercise the *same* objects the app ships,
not a second compile of the sources (the old test/build smell). Widgets and
backend-/Qt-Network-coupled code stay in `denso`; the pure, portable logic lives
in the libs. Final lib graph: `denso_core ← {denso_detection, denso_brazing,
denso_camera (→denso_detection)} ← denso`.

| Lib | Holds (all pure / backend-free) |
|---|---|
| `denso_detection` (`detection/`) | inference helpers: `letterbox`, `yolo_decode`, `merge_detections`, `names_metadata`, `class_names_sidecar` |
| `denso_brazing` (`brazing/`) | reporting logic: `brazing_payload`, `brazing_retry_policy`, `zone_aggregator`, `zone_reporter` (the `ZoneSink` impl — zero Qt deps) (+ contracts `zone_reading.h`, `zone_sink.h`, `brazing_transport.h`) |
| `denso_camera` (`camera/`) | non-widget capture/frame infra: `frame_convert`, `gst_pipeline`, `rtsp_templates`, `snapshot`, `fps_meter`, `stream_pacing`, `warmup_gate`, `preview_gate`, `zone_assembly`, `frame_processor` |

UI is grouped by feature: the **app shell** at `ui/` root, plus `ui/common/`,
`ui/settings/`, and `ui/camera/` — the latter now holds **widgets only** (the
non-UI camera / detection / brazing subsystems moved out to
`src/app/{camera,detection,brazing}/`). Note there are now two `camera/` dirs —
`src/core/camera/` (domain) vs `src/app/camera/` (runtime) — resolved by filename
via separate include roots.

| Path | Responsibility |
|---|---|
| `main.cpp` | Thin orchestrator: install the logging handler → **DB-stage readiness preflight** (`health::evaluate_db_schema`; SchemaNewer/DbUnopenable → `status.json` + EX_CONFIG 78 before the window) → open DB → migrate (MigrationFailed → 78) → import legacy → sync models → load settings → apply startup → run. Network **reassert** is deferred to the first event-loop tick (`QTimer::singleShot(0, …)`, best-effort/exceptions swallowed) so a slow/stuck CLI can't keep the window from appearing. |
| `logging/` | **24/7 bounded file logging** (installed as a `qInstallMessageHandler` — GUI subsystem has no console). `log_sink` (`RotatingLogSink`: 5 × 5 MiB ≈ 25 MiB cap; close→roll `.N`→reopen under a mutex; records truncated to ≤16 KiB; `write()` is `noexcept`; disk-full → stderr fallback + degraded mode + reopen retry), `log_rotation` (pure, unit-tested size/roll policy), `log_throttle` (`LogEpisode` — collapse a repeating condition to one opened/one closed line + a periodic count so a flapping camera can't flood the file), `redact` (`sanitize_url` strips RTSP credentials). `qSetMessagePattern` bakes local ISO time + UTC offset + level + category + pid + tid; `DENSO_LOG_LEVEL` floors severity; a SESSION banner + 5-min HEARTBEAT mark liveness. |
| `ui/common/` | **Leaf** shared dialog primitives (Qt-only, no feature deps): `dialog_chrome` (`dialog_header`), `async_runner` (`run_on_worker` — **returns the `QThread*`** so an owner can track/`wait()` it on teardown — + `post_to_gui`), `form_widgets` (`eyebrow`/`dim_label`/`spec_row`/`hline`). Both dialogs build on these instead of re-copying chrome. |
| `ui/theme.{h,cpp}` | Palette + theme-driven app stylesheet. |
| `ui/mainwindow.{h,cpp}` | Root window (top bar + content); hosts settings-persistence handlers; opens the settings + camera modals. |
| `ui/settings/settings_dialog.{h,cpp}` | Settings modal: a thin **view** — nav + 5 panels + footer. The Network page is a self-contained `ui/settings/network_panel` (`NetworkPanel`) that owns its cards, the DB-backed network apply, and the threaded scan/connect/refresh; the dialog just hosts it. **All four blocking handlers (apply/scan/connect/refresh) run on a worker** so a stuck CLI can't freeze the UI. Each worker is tracked in `workers_` (`QPointer<QThread>`), guarded with a `QPointer<NetworkPanel>` before its GUI post, and `wait()`ed in the destructor — so a dialog closed mid-operation can't use-after-free `this`. A `net_busy_` flag serializes actions so a double-click can't double-apply or spawn duplicate workers. |
| `ui/settings/netcard.{h,cpp}` | Per-interface status + editable config + Wi-Fi scan/connect. |
| **`ui/camera/`** | **Widgets only** now. Two entry points (root: `camera_view`, `camera_dialog`, `wizard_controller`) + `grid/` (live-view widgets) + `dialog/` (modal pages) + `shared/` (`roi_geometry`, the one remaining cross-cutting UI primitive). The capture/detection/brazing subsystems live at `src/app/{camera,detection,brazing}/`. |
| `ui/camera/camera_view.{h,cpp}` | **Entry point.** Main content switcher: empty state (+ Add) when 0 cameras, else the live `CameraGrid`. `release_streams()`/`reload()` free + restart capture around the modal. |
| `ui/camera/camera_dialog.{h,cpp}` | **Entry point.** Camera management modal — a thin **view** over a 5-page stack (the `dialog/` pages) run as a guided wizard: list/delete + add (USB scan / IP manufacturer+stream+credentials) → Configure (preview + resolution/fps/rotation/pitch/roll) → Models (attach 1..N detection models + per-class confidence) → Areas (draw ROI polygons; optional). The dialog owns only the page stack, the stepper header + Back/Next/Finish footers, and modal sizing; `show_page()` centralizes page+stepper+sizing and grows the modal near-fullscreen on the Areas step. Flow-state, threaded snapshot capture, and all add/edit DB writes live in `ui/camera/wizard_controller`. |
| `ui/camera/wizard_controller.{h,cpp}` | `CameraWizardController` — drives the wizard via an injected `show_page` callback + a `request_show_list()` signal. **Source is editable in both add and edit.** On an edit that changes the effective source or capture geometry (`camera::requires_area_review`) it **quarantines the ROIs** (sets `areas_need_review`), so the reloaded grid excludes those areas + pauses zone reporting until the operator re-verifies them on the **Areas** step (saving areas clears the flag; a **live** preview whose **aspect still matches** the capture is required first — `PreviewGate::has_live_frame()` + `camera::aspect_changed`, since rotation/pitch/roll are re-applied to the raw frame but an aspect change makes it the wrong shape to confirm normalized ROIs against — else reporting stays paused). **The row is inserted UNFINISHED** (`setup_complete=false`) at the Configure step, because attaching models needs a real id: only an explicit terminal action (Models' *Finish — use whole frame*, or Areas' *Save areas & finish setup*) completes it, so backing out leaves an inert draft rather than a live model-less camera. `finish_and_leave()` is the ONE owner of complete→notify→navigate-only-on-success; `finish_setup()` is only ever called AFTER the write it completes has succeeded. `push_used_sources()` feeds the Source page each *other* camera's IP/USB index so a rescan can tag already-owned devices **"(in use)"**. |
| `ui/camera/grid/camera_grid.{h,cpp}` | Live 1–4 grid: a tile + `CameraStream` per **`camera::runtime()`** camera (first 4 by id), laid out via `grid_dims`; the filter is in SQL so an unfinished camera can't eat a tile slot; owns start/stop/reload. When brazing is enabled it also owns the machine-wide `ZoneReporter` + `BrazingReporter` (retry coordinator over a `BrazingClient`) and passes the reporter as each `DetectionProcessor`'s `ZoneSink` (torn down after streams join, in `clear()`). **UI-first start:** model-less cameras start immediately; a detection camera's stream is deferred (tile shows "Preparing model…") until its models warm — tracked by the pure `camera/warmup_gate` (`PendingStart`), driven by `WarmupState`'s `model_ready`, then built via `start_one`, which constructs the inference engine **on the GUI thread** (a cache-hit, never a build) so engine construction never lands on a capture thread; the remainder flushes on warm-up `finished` with the orientation fallback. Owns a GUI-thread `health::ZoneHealth` (no mutex): per-camera inhibit causes (AreasNeedReview at boot, CaptureOffline from `status_changed`, InferenceWorkerFailed marshalled off the inference worker, ModelUnavailable from the boot integrity verdict) gate `ZoneReporter::set_camera_inhibited`, paint the tile's inhibit banner (`set_inhibited`), and rewrite `status.json`. The snapshot callback carries a monotonic sequence with a drop-stale guard (`last_applied_seq_`). A quarantined camera (`areas_need_review`) boots inhibited and its zone reporting is held. |
| `camera/camera_stream.{h,cpp}` | Per-camera capture worker (own `std::thread` + `cv::VideoCapture`; a `QObject`, so it stays in `denso` not the lib): read → `safe_process(FrameProcessor)` → queued `frame_ready`/`status_changed`; ~15 fps display cap paced by a high-resolution `precise_sleep` (MinGW `sleep_for` is pinned to the ~15.6 ms tick; the timer handle is RAII-held so it closes at thread exit), clean stop/join. `run()` is an **outer reconnect loop**: a failed open or mid-stream drop emits Offline, backs off (`next_backoff_ms`, stop-responsive `wait_or_stop`), and reopens — a camera that blinks out recovers without an app restart. USB opens by index; IP opens via `rtsp_gst_pipeline` on `cv::CAP_GSTREAMER`, falling back to FFMPEG. Capture resolution is set for USB only (setting it on a live GStreamer pipeline segfaults). **Drop-oldest backpressure:** a `shared_ptr<atomic<int>>` in-flight counter (`frame_counter()`) is gated by `should_emit` before each emit (`kMaxInFlight = 2`) and decremented by the tile on consume, so a lagging GUI drops frames instead of unboundedly queuing full-res `QImage`s. |
| `ui/camera/grid/camera_tile.{h,cpp}` | One grid cell: paints the latest frame (aspect-fit) + name + status dot + live per-tile FPS + the camera's ROI polygons as gold outlines (`set_areas`); placeholder when connecting/offline, or "Preparing model…" while its detection model warms (`set_preparing`). Shows a persistent "Areas need review — reporting paused" banner (`set_review_paused`) while the ROIs are quarantined after a source edit. Holds the stream's shared backpressure counter (`set_frame_counter`) and decrements it as each frame is consumed. |
| `camera/fps_meter.{h,cpp}` | Pure (unit-tested) EMA-smoothed FPS estimate from frame-arrival timestamps; each tile `tick()`s it per displayed frame. |
| `camera/safe_process.h` | Header-only exception firewall: `safe_process(FrameProcessor*, QImage)` runs `process()` in a try/catch and returns the input frame unchanged on any throw (or a null processor). Stops one malformed frame from escaping the raw capture thread → `std::terminate()`. Unit-tested with a throwing stub. |
| `camera/frame_processor.{h,cpp}` | The per-camera frame seam: `FrameProcessor` interface + `OrientationProcessor` (orientation only) + `DetectionProcessor` (orientation → inference → per-class conf filter → ROI confinement (keep only boxes whose centre is inside an area; empty areas = whole frame, via `area_geometry`) → draw labelled boxes). `camera_grid` picks per camera: `DetectionProcessor` when the camera has attached, loadable models, else `OrientationProcessor`. `DetectionProcessor` also carries an optional `ZoneSink*` (brazing): after the kept boxes, it calls `group_into_zones` and feeds the `ZoneReporter`. |
| `ui/camera/grid/grid_layout.{h,cpp}` | Pure (unit-tested) `grid_dims(n)` → rows/cols (1→1×1, 2→1×2, 3–4→2×2). |
| `camera/stream_pacing.{h,cpp}` | Pure (unit-tested) capture-loop flow control: `next_backoff_ms` (reconnect backoff 1s→×2→10s cap; reset on a live frame) + `should_emit` (drop-oldest backpressure gate, `in_flight < max`). No Qt/OpenCV. Used by `CameraStream`. |
| `camera/warmup_gate.{h,cpp}` | Pure (unit-tested) `PendingStart` gate: tracks which detection cameras are still waiting on a model, so `CameraGrid` starts each one exactly once as its models come ready. |
| `camera/preview_gate.{h,cpp}` | Pure (unit-tested) state for the wizard's snapshot preview: `begin`/`settle`/`invalidate`/`has_live_frame`. Owns **supersede** (only the newest capture applies — a late result from a previous source must not become the image ROIs are verified against) **and liveness** (the LAST settled attempt succeeded). Liveness drops the instant a refresh `begin()`s, not when it settles: otherwise a failed refresh leaves the previous success on screen and the operator can still "verify" quarantined ROIs against it — the gate bypassing itself through its own Refresh button. The displayed image is the controller's `last_frame_`, deliberately separate. |
| `camera/zone_assembly.{h,cpp}` | Pure (unit-tested) brazing digit→number step: `assemble_zone_value` (sort digits left-to-right by box x-center, concat → int) + `group_into_zones` (assign kept digits to each ROI's `zone`, assemble). `ZoneReading{zone_no,value,conf}` in `zone_reading.h`. (Stays camera-side — consumed by `frame_processor`, not brazing-only.) |
| `brazing/zone_aggregator.{h,cpp}` | Pure (unit-tested) machine-wide zone state: per-zone debounce (`kStableFrames=5`) + change detection; `observe()` returns the full `{zone_no→value}` snapshot when a stable value changed. Absent zones retain their value. |
| `brazing/zone_reporter.{h,cpp}` | `ZoneSink` impl (pure logic, in `denso_brazing`): every camera's `DetectionProcessor` feeds zones here from its **inference worker thread** (`frame_processor.cpp`, inside `infer_loop`) → mutex-guarded `ZoneAggregator`; on change invokes a callback with the snapshot + a monotonic sequence number (wired to marshal via `post_to_gui` to `BrazingReporter`, with a drop-stale guard). `set_camera_inhibited` drops a whole camera's observations under the same mutex (kills the resurrection race) and evicts its recorded zones. |
| `brazing/brazing_payload.{h,cpp}` | Pure (unit-tested) `build_brazing_payload` → `{"zoneN":v,...}` (ascending order). |
| `brazing/brazing_retry_policy.{h,cpp}` | Pure (unit-tested) retry state machine: holds pending/delivered/in-flight snapshots + backoff (1s→×2→30s cap); `submit`/`on_result`/`on_retry_tick` each return the next `RetryAction` (Send/ArmRetry/None). Single-flight + latest-value-wins. |
| `brazing/brazing_reporter.{h,cpp}` | GUI-thread shell over `BrazingRetryPolicy` + a single-shot retry `QTimer` + a `BrazingTransport` (stays in `denso`). `submit()` (marshaled from `ZoneReporter`) drives reliable, coalescing delivery — a downed server is retried until it 2xx-acks the latest snapshot. In-memory only. |
| `brazing/brazing_client.{h,cpp}` | `BrazingTransport` impl (stays in `denso`): one async POST of the snapshot to `{base_url}{api_path}` (`QNetworkAccessManager`, 5 s timeout), composed once in the ctor via `brazing::endpoint_url`; reports 2xx-or-not via a `done(ok)` callback. No retry state (that's the reporter's). |
| `ui/camera/dialog/` (pages) | The dialog's six page widgets + shared `page_util` (`dim_label`): `list_page` (`CameraListPage`), `add_page` (Source form + scans; tags already-owned devices **"(in use)"** via the used-source set the controller pushes), `configure_page` (preview + orientation controls), `models_page` (`ModelsPage`), `areas_page` (`CameraAreasPage`: ROI editing; shows the review-required prompt under quarantine), `level_calibration_page` (`LevelCalibrationPage`). `ModelsPage` renders in **two shapes**, set by the controller from the committed mode: **Ensemble** (N models, a shared class checklist with per-class conf, **Select all / Clear**) and **Single** (radio buttons — exactly one model and one class, no conf, because that threshold belongs to the binding's own configuration). The shape is binding CARDINALITY, never authorization: the offered list is still `detection::evaluated_models` in both. Pages own their controls and emit request signals; the `CameraWizardController` drives them. |
| `ui/camera/dialog/areas_page.{h,cpp}` | The **Areas** step. Validates the set BEFORE asking for a save so a deterministic problem is named here rather than surfacing as a generic write failure: an unfinished/cleared draw blocks the save, degenerate + self-intersecting polygons are refused, and zone clashes are named ("Zone 4 is already used by …") with taken zones **disabled** in the picker (`zones_owned_by_other_cameras` + this camera's other areas). Drawing is a **locked sub-task** — the list and "+ New area" disable until the operator finishes or Cancels, so a half-drawn polygon can't evaporate. Leaving dirty warns (incl. via `CameraDialog::reject()`, so Escape/X can't bypass it); Delete and a destructive empty-save confirm. Metadata typed before the shape exists is held in a draft and applied on close. |
| `ui/camera/dialog/wizard_stepper.{h,cpp}` | Non-interactive "① Source — ② Configure — ③ Models — ④ Areas" step indicator; `set_current()` emphasizes the active step. |
| `ui/camera/dialog/roi_canvas.{h,cpp}` | ROI **editing** canvas over the oriented snapshot, with the camera's other areas drawn dimmed + zone-labelled for context. Three modes: **Idle** (view), **Drawing** (click to add vertices; click the ringed first vertex / Enter / "Done shape" to close), **Editing** (drag a corner, click an edge to insert one, tap a corner + "Remove corner" to drop it). Clicks off the frame are **rejected**, not clamped (a drag still clamps — the operator is holding the handle). Every gesture has a page button; keyboard + right-click are accelerators only (the panel may be touch). |
| `ui/camera/dialog/level_canvas.{h,cpp}` | Ball Leveler calibration canvas over the oriented snapshot: ONE axis-aligned measurement rectangle (rubber-banded out) + TWO draggable horizontal reference lines, reusing the same `roi_geometry` mapping and the same rejected-off-frame / clamped-mid-drag rule as `roi_canvas`. Holds **no** rule and **no** data — it emits normalized requests and paints what it is given, so no constraint can exist in the painter that the reload and write paths lack. The page owns the mode; the canvas never changes it. |
| `ui/camera/dialog/level_calibration_page.{h,cpp}` | The Ball Leveler **Level calibration** step: a thin driver over `level::CalibrationDraft`. Turns canvas gestures and the conf/hold spin boxes into draft mutations and paints `draft()` back; Save is enabled by the draft's `check()`, which is the SAME validator `level::save_level_configuration` runs, so Save can never be offered for a calibration the write would refuse. Display setters are signal-blocked — writing them back would round a stored value through the widget's decimals, i.e. opening the page would edit it. Labels both lines as **ball-centre** positions, not liquid-surface lines. |
| `ui/camera/dialog/camera_devices.{h,cpp}` | USB enumeration (Qt Multimedia). |
| `ui/camera/dialog/ip_scan.{h,cpp}` | Subnet RTSP-port scan (Qt Network, threaded). |
| `camera/snapshot.{h,cpp}`, `camera/frame_convert.h` | Grab one preview frame (OpenCV, off-thread) + orient it: `apply_orientation` composes rotation + roll + pitch (perspective warp) for the live Configure preview. |
| `camera/rtsp_templates.{h,cpp}` | Manufacturer → RTSP URL templates (Dahua) + `with_credentials`. |
| `camera/gst_pipeline.{h,cpp}` | Pure (unit-tested) string builders for low-latency GStreamer pipelines (`cv::CAP_GSTREAMER`): RTSP via hardware **NVDEC** (`nvv4l2decoder → nvvidconv → BGR`, per-codec depay/parse, drop-on-latency `rtspsrc`, leaky queue after the decoder), plus USB MJPEG/YUYV builders. `camera_stream.cpp` tries them as a ladder (first that opens+reads wins). |
| `ui/camera/shared/roi_geometry.{h,cpp}` | Pure (unit-tested) widget↔normalized point mapping + aspect-fit rect for the canvas. (The one UI primitive left in `shared/`.) |
| `detection/` | Per-camera detection runtime (app-only: OpenCV + inference backend). The **pure, backend-free** helpers are the `denso_detection` lib — `letterbox` (resize+pad to 640 + inverse box map), `yolo_decode` (`decode_yolo`: raw `[1,4+nc,anchors]` → argmax + conf floor + NMS; `decode_yolo_end2end`: NMS-free `[1,N,6]` → conf floor only — the engine picks by output shape), `merge_detections`, `names_metadata` (parse the ONNX `names` dict), `class_names_sidecar` (`<engine>.names.json`). The **backend-coupled** runtime stays in `denso`: `inference_engine` (interface) → `ort_engine` [Win] / `trt_engine` [Jetson] → `engine_registry` (one shared engine per model file; `get()` is **mutex-guarded** — called from both the warm-up worker and the GUI thread; `warm_up()` loads + runs one blank inference on every model on the background worker and fires an `on_ready(filename)` per warmed model that drives the UI-first per-camera start, so the first real frame — and the minutes-long TensorRT build — never stalls a capture thread) + `model_sync` (scans `models/*.onnx` / `models/*.engine` into the catalog at startup). |

## Detection / inference backend (platform-split)

Per-camera YOLO detection is an **app-only** feature — the domain config lives in
`src/core/detection/` (Qt/OpenCV-free, unit-tested); the inference runtime in
`src/app/detection/` (pure helpers in the `denso_detection` lib, backend engines
in `denso`). The backend is **platform-split** behind
the `InferenceEngine` interface (selected via the `BackendEngine` alias in
`engine_registry.h`); OpenCV letterbox + `decode_yolo`/`decode_yolo_end2end` are
shared and identical across backends.

**Windows / MSYS2 (dev) — ONNX Runtime (`OrtEngine`):** the ORT GPU build lives
in `third_party/onnxruntime/` (git-ignored; see `docs/GPU_SETUP.md`). ORT +
provider DLLs and `models/*.onnx` are copied beside the exe by a `POST_BUILD`
step. Provider fallback is **TensorRT → CUDA → CPU** (missing GPU stack → CPU,
never a hard fail); provider DLLs are staged in `third_party/gpu_ep/`. The
TensorRT EP builds an optimized engine on first run (FP16, cached under
`models/trt_cache/`) — a minutes-long, non-interruptible build that runs on the
warm-up worker (`EngineRegistry::warm_up()`), never a capture thread. `model_sync`
catalogs `models/*.onnx` (class names from ONNX metadata).

**Linux / Jetson Orin Nano (real target) — native TensorRT (`TrtEngine`, links
`nvinfer` + `CUDA::cudart`):** loads a **prebuilt `.engine` ONLY** — built with
`trtexec` on the aarch64 build host for TRT 10.3 / `sm_87` and then **shipped
inside the `.deb`** (not rebuilt per appliance), so it is qualified only for the
supported platform; see *Engine compatibility* in `AGENTS.md`. The app **never builds
one at runtime and has NO fallback**: a missing/incompatible/invalid engine
**fails loud** at startup (throwing `TrtEngine` ctor → `WarmupWorker` catches →
`app.exit(1)`). Class names come from a `<engine>.names.json` **sidecar**
(TRT engines carry no name metadata); `model_sync` catalogs `models/*.engine` via
those sidecars. Serialized-frame inference is mutex-guarded across cameras.

**Startup** (`ui/startup_mode` `cold_start_needs_splash`): a **cold** start shows
the blocking `StartupScreen` splash and warms behind it (Windows: the ORT build;
Jetson: deserialize + warm each prebuilt engine), then builds the window; a
**warm** restart (Windows, cached engine) is **UI-first** — the window shows
immediately and each detection `CameraStream` starts only **after its model(s)
finish warming**. Warm-up never lands on a capture thread.

`denso_core` **never** links OpenCV/ORT/TensorRT — only `Qt6::Core`/`Sql`.
`models/*.engine`, `models/*.names.json` and `models/trt_cache/` are git-ignored:
a sidecar is generated on-device beside its engine and is exactly as
device-specific as it is.

## Model / operating-mode compatibility

Which model may load in which mode. **Full reference:
[`docs/MODEL_COMPATIBILITY.md`](docs/MODEL_COMPATIBILITY.md)** — read it before
touching anything below. The rules, condensed:

- **Identity is DECLARED in the schema-2 `manifest.json`**, never inferred from a
  filename, display name or class signature. A catalog row joins to a generation
  by the *active backend's* declared artifact filename; the identity returned is
  the generation's. Artifacts only *corroborate* (SHA-256 + ordered class names +
  `built_for`) — they never declare.
- **ONE compiled policy decides authorization**:
  `models::model_compatibility()` in `src/core/models/compatibility.cpp`. The
  family→modes matrix lives in that TU and nowhere else — not in the manifest, a
  SQL `WHERE`, a UI `if`, or a shell test. Adding/widening a model is a **code
  change** with tests and review.
- **Five enforcement paths, all calling that one policy**: (1) the warm-up
  allow-list (`loadable_model_files` → `EngineRegistry`), (2) the mode-filtered
  fail-loud required set (`attached_model_filenames`), (3) the selectable-model
  list (`selectable_models`), (4) attachment write + runtime resolution
  (`set_camera_models` / `detection_for`), (5) boot/integrity
  (`health::evaluate_integrity`). None of them may hold a rule of its own, and
  `mode`/`view`/`platform` have **no defaults** — a forgotten call site must fail
  to compile, never silently authorize.
- **The manifest carries NO `allowed_modes`** — there is no field to parse one
  into, so a stray key is inert and can never become a second authority.
- **Reason codes are a FILE FORMAT** (they reach `status.json`): never rename,
  reuse or renumber. First-failure-wins order is a contract —
  `model_undeclared` → `model_unknown_id` → `model_family_mismatch` →
  `model_shape_unsupported` → `model_classes_mismatch` →
  `model_provenance_failed` → `model_mode_incompatible` → `model_allowed`. Only a
  genuine valid wrong-mode model may report `model_mode_incompatible`.
- **A rejected ATTACHED model inhibits only its camera**: Degraded/exit 10, never
  Blocked/78; no `DetectionProcessor`, no `get()`, siblings keep running. It
  reuses `ZoneCause::ModelUnavailable` — **no new `ZoneCause` bit**.
- **An idle wrong-mode artifact is a NORMAL state.** Declared, valid, unattached:
  skipped by warm-up, never deserialized, not in the required set, no camera
  issue — the appliance stays **Ready / exit 0**. A `digit_reader` box carries all
  three model pairs and loads only `digitv3`.
- **Jetson `built_for` is NORMALIZED**: `trt=10.3 / cuda=12.6 / sm=87`, compared
  **exactly** against `platform::measured_platform_info()`. The raw provenance
  strings (`10.3.0.30`, `12.6.68`) in `runtime.tensorrt.built_for` produce
  `model_provenance_failed` — measured, not theoretical. Never add fuzzy matching.
- **`seed-manifest` vs `verify`**: `seed-manifest` is explicit, manifest-only,
  atomic, idempotent, accepts canonical equivalence, refuses a differing manifest
  or mismatched artifacts, and **never** replaces a model. `verify` only
  *observes* — no `--repair` exists, and it changes nothing under `models/`. It is
  **not** globally read-only (it writes a DB backup dir outside `models/`); don't
  document it as such.
- **Model artifacts are protected and staged explicitly.** `models/` is
  git-ignored by pattern; never `git add -A`. `packaging/models.approved` approves
  the engine **and** sidecar as a pair — a `float-*` stem may only be approved
  once the warm-up allow-list exists (machine-enforced by
  `assert_float_seeding_guarded`).
- **Ball Leveler is ACTIVATED (Phase B).** All three production guards came
  down together, once the whole runtime path was green: the mode is selectable,
  the Camera Wizard's Ball branch is reachable, and a configured camera measures.
  A camera gets a `BallLevelProcessor` only with a stored calibration that still
  validates against the CURRENT view (`camera::view_revision`) and a bound model
  the central policy authorizes for `BallLeveler`; anything less is an explicit
  `Unconfigured` / `CalibrationInvalid` / `Unavailable` tile, never a silent one.
  Mode purity is unchanged and still absolute — a `ball_leveler` grid builds NO
  `ZoneHealth`, NO zone/brazing reporter and NO `DetectionProcessor`, reads no
  `camera_area`, and asks only for its one Float engine. Still deliberately
  absent (Lean V1): `HoldingLastValid`, the status-file `level` array, backend
  level reporting, historical persistence.

## Deployment model — manual `.deb` upgrades

This is an **embedded appliance**. Updates are **administrator-managed MANUAL
`.deb` upgrades**; there is **no automatic updater and none is wanted**:

```
development → tests → clean commit → clean .deb build → remote maintenance
→ stop Denso → preflight → sudo apt install ./new.deb → verify → start Denso
```

**Do not propose or recreate** a transactional automatic-update architecture,
automatic rollback, or unattended crash recovery. They were considered and
deliberately rejected — recovery is manual and explicit.

- **Install and upgrade with `sudo apt install ./denso-digitalreader_<version>_arm64.deb`
  — that one command is the whole procedure**, fresh or upgrade. There is no
  mandatory `denso-setup configure` step afterwards. Never remove the old package
  first, never make `dpkg -i` the normal workflow, never `apt purge` when
  operator data must survive.
- **The operator user is resolved, never assumed, and never hardcoded.**
  `resolve_operator_user` (policy.sh) applies one precedence: an existing
  recorded user → `SUDO_USER` → exactly one acceptable local, active,
  non-remote session user. root, `nobody`, system/service accounts, `nologin`
  shells, uids outside `[1000,60000]` and unknown names are refused, and
  **ambiguity fails the install rather than guessing**. A recorded user that is
  no longer valid is a hard failure, *not* a fall-through — falling through is
  how an upgrade would silently hand the appliance to whoever ran sudo.
- **Autostart is enabled automatically; autologin is never touched.** A fresh
  install enables the user unit for the resolved operator by creating the same
  `graphical-session.target.wants` symlink `systemctl --user enable` would —
  that user is normally not logged in during `apt install`, so there is no user
  manager to talk to. Nothing in the package edits GDM.
- **systemd `--user` is the SOLE autostart authority.** There is no XDG
  autostart entry; `denso-digitalreader.service` carries
  `[Install] WantedBy=graphical-session.target`, so
  `systemctl --user enable|disable` is meaningful and `disable` genuinely stops
  Denso starting at login. Startup is
  `graphical login → systemd --user → denso-digitalreader.service →
  /usr/bin/denso-digitalreader → /opt/denso/bin/denso`. The desktop/menu entry
  runs `systemctl --user start denso-digitalreader.service`, i.e. the *same
  unit* — starting an already-active unit is a no-op, so there is exactly one
  process authority. **Never reintroduce an XDG autostart entry**: a second
  authority would make `disable` a lie.
- **An upgrade must never re-enable a disabled service.** The legacy XDG→systemd
  migration is guarded by `install-state/autostart-migrated`. Legacy entry
  present → remove it and enable the unit; legacy entry absent → autostart was
  deliberately off, so leave the unit disabled; marker present → do nothing. A
  *fresh* install is a separate path and always enables.
- **`denso-digitalreader.service` is a systemd USER unit, never a system/root
  service** — Denso is a GUI that must live and die with the operator's
  graphical session. `Restart=no` (single-instance; a restart loop against a
  held lock buries the real fault). No `DISPLAY`/`WAYLAND_DISPLAY` is ever
  invented: `denso-session-check` fails first, with the reason.
- **Lifecycle and health are different questions.** `systemctl --user
  status|start|stop|restart denso-digitalreader` reports process/service
  lifecycle. `denso-digitalreader --check-running` (lock, tri-state) and
  `--check` (application/database/model health, 0/10/78) report application
  health. Never substitute one for the other.
- **Logs.** This appliance runs a volatile journal with no per-user journal
  files, so the supported command is
  `sudo journalctl _SYSTEMD_USER_UNIT=denso-digitalreader.service -f`
  (`-n 100` for recent). `journalctl --user -u denso-digitalreader` works only
  where persistent per-user journals are configured; the package must never
  require or configure that.
- **Operator data is `/opt/denso/data`** (primary DB `denso.db`). The package
  **must not own** the database, its WAL/SHM, or generated migration backups.
  Database files are operated **as the resolved operator user, never as root** —
  a root-owned
  WAL/SHM/lock in an operator-owned data dir is how this appliance breaks.
- **Migration is forward-only and fail-closed.** `postinst` runs
  `db_upgrade_gate` (`packaging/lib/policy.sh`): confirm stopped → classify
  schema → refuse a DB newer than the binary → one verified pre-migration backup
  (`denso.db.pre-v<schema>`, reused on `dpkg --configure -a` retries, never
  overwritten) → `--apply-migrations` → verify schema/integrity → `denso --check`
  (accept **0** Ready and **10** Degraded; refuse **78** Blocked and anything
  unmodelled) → leave Denso **stopped**. No automatic rollback.
- **`--check-migrations` is COPY-ONLY** — never document or run it against the
  live production database. **`--apply-migrations`** is the narrow production
  entry point; it takes no path and resolves the primary DB from
  `DENSO_DATA_DIR`.
- **Six canonical model files**, TensorRT `.engine` only (never `.pt`/`.onnx`):
  `digitv3`, `float-small`, `float-big` — each `.engine` + `.names.json`. Digital
  Number Reader accepts **digitv3 only**; Floating Ball Leveler accepts
  **float-small and float-big only**. Never offer a model across modes.
- **Release build:** `tools/build_package.sh --models-dir models` from a **clean
  commit**; never `--allow-dirty` for a release artifact. The canonical input is
  the **repository** `models/` dir, never the installed `/opt/denso/models`. The
  build fails if the set is incomplete or an unexpected model artifact appears.
- **`192.168.1.15`** is the one validated Jetson — development, build, package
  install, runtime test and acceptance. **`192.168.1.81` is retired** and is no
  longer an acceptance device; do not plan around it. Do not revive the abandoned
  EXEC-SPEC / Revision-6.x exercise unless explicitly asked.

## Hard rules

- Domain/feature types **never** see UI view types. The only boundary is
  `src/core/ui/convert.{h,cpp}`.
- `main.cpp` stays a thin orchestrator — no business logic.
- Each feature is split header/source by responsibility (type / persistence /
  OS access). Access policy is the `repo`'s API surface, not SQL grants.
- Persistence is one SQLite file with version-gated migrations in
  `db::run_migrations` — add a migration, never edit a shipped one.
- OS-specific work sits behind `NetworkBackend` (`network/backend.{h,cpp}` +
  `network/windows/`, `network/linux/`). Keep both platforms in sync.
- `denso_core` must not link `Qt6::Widgets` — the GUI cannot leak into the
  testable core.
- `build/` and `*.png` are git-ignored (see `.gitignore`); `assets/icon.png`
  predates the `*.png` rule and stays tracked as a committed source asset.
- Dialogs are a **thin view + a controller/panel** that owns flow-state, async
  work, and persistence. Shared dialog chrome (header, async runner, label
  factories) lives in `ui/common/` and is never re-copied into a feature. The
  detection pipeline's `ReadingSink` hook must hand off to a worker — never do
  DB I/O inline. Since inference was decoupled from display, `on_reading()` fires
  on the **inference worker thread**, not the capture thread
  (`camera/frame_processor.h:59` is the authoritative contract).
- **Operating-mode switch ordering is load-bearing.** Teardown must call the ONE
  authoritative primitive (`CameraGrid::teardown()` → `clear()`) and must run
  **before** the reset transaction — never `CameraView::reload()`, which
  re-queries `runtime()` (still the *old* mode's rows) and restarts the pipeline,
  and never `release_streams()`, which joins only capture threads. The in-memory
  mode is assigned **only after the commit**; a rollback re-reads it from the DB
  and rebuilds the old pipeline. Both modes are now real destinations, so a
  committed switch must also **replace the inference session** — `EngineRegistry`
  is immutable and mode-pure for its whole life, so the boot registry can never
  load the other mode's engines. Build the replacement through the ONE builder
  (`ui/engine_session.h`), after the commit and after teardown, never a union
  allow-list; retire the outgoing `WarmupState` as a consequence of the COMMIT
  (its boot-wired `failed` means `app.exit(1)`, which is boot-only semantics).
- **`192.168.1.81` is retired** — it is not an acceptance device any more. All
  on-device validation happens on `192.168.1.15`.
- Packaging keeps **one** definition of each rule: `packaging/lib/policy.sh` is
  the sole JetPack-damage check and is *concatenated verbatim* into the generated
  preflight guard — never hand-copy it, and never add a second emitter, or the
  standalone guard and `denso-setup preflight` can silently disagree.
- A **clean** build must stay byte-reproducible (the artifact name carries no
  content hash, so a rebuild would otherwise replace a shipped artifact under an
  identical filename). Anything a build writes must therefore be derived from the
  commit, not the clock, the umask, or the process — see the variance sources in
  `docs/ARCHITECTURE.md`. Gate: `tests/manual/repro_build.sh`, which rebuilds the
  same commit under two umasks. If you add a **payload** file written by `>`
  rather than `install -m`, chmod it: `dpkg-deb` normalizes control-archive modes
  but leaves data-archive modes exactly as staged.

## Workflow

Superpowers SDD: specs in `docs/superpowers/specs/`, plans in
`docs/superpowers/plans/`, progress ledger in `.superpowers/sdd/progress.md`.

See `docs/ARCHITECTURE.md` for the boot sequence, data flow, threading model,
persistence model, and gotchas (including the QSQLITE read-cursor-before-DDL
locking rule).
