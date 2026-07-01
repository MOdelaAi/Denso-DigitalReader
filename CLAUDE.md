# Denso-DigitalReader

Desktop app (C++ / Qt Widgets / CMake) for reading a 4-digit 7-segment display,
with a settings UI for display resolution, theme, hardware spec, and network
configuration. Single SQLite store (`denso.db`) next to the executable.

Ported 1:1 from a Rust + Slint original; the port history lives on branch
`port/cpp-qt`.

## Commands

Out-of-source build with CMake (needs Qt6: Core, Gui, Sql, Widgets). Builds and
runs on the MSYS2 UCRT64 toolchain (`pacman -S mingw-w64-ucrt-x86_64-qt6-base
mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja`).

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
| `camera/` | Camera inventory: domain structs (`model.h`: `Camera` + polygon `CameraArea`) + persistence (`repo`, full camera CRUD + ROI-area read/replace). `area_points` (de)serializes a polygon's normalized vertices to the `camera_area.points` TEXT column. |
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
| `ui/camera/camera_dialog.{h,cpp}` | **Entry point.** Camera management modal — a thin **coordinator** over a 4-page stack (the `dialog/` pages) run as a guided wizard: list/delete + add (USB scan / IP manufacturer+stream+credentials) → Configure (preview + resolution/fps/rotation/pitch/roll) → Areas (draw ROI polygons; optional). Owns snapshot capture, add/edit DB writes, navigation + sizing. Stepper header + Back/Next/Finish footers; `show_page()` centralizes page+stepper+sizing; the modal grows near-fullscreen on the Areas step. |
| `ui/camera/grid/camera_grid.{h,cpp}` | Live 1–4 grid: a tile + `CameraStream` per camera (first 4 by id), laid out via `grid_dims`; owns start/stop/reload. |
| `ui/camera/grid/camera_stream.{h,cpp}` | Per-camera capture worker (own `std::thread` + `cv::VideoCapture`): read → `FrameProcessor` → queued `frame_ready`/`status_changed`; ~15 fps, clean stop/join. |
| `ui/camera/grid/camera_tile.{h,cpp}` | One grid cell: paints the latest frame (aspect-fit) + name + status dot + the camera's ROI polygons as gold outlines (`set_areas`); placeholder when connecting/offline. |
| `ui/camera/grid/frame_processor.{h,cpp}` | The per-camera frame seam: `FrameProcessor` interface + `OrientationProcessor` (today). The detection model becomes another impl, chosen by a future config flag. |
| `ui/camera/grid/grid_layout.{h,cpp}` | Pure (unit-tested) `grid_dims(n)` → rows/cols (1→1×1, 2→1×2, 3–4→2×2). |
| `ui/camera/dialog/` (pages) | The dialog's four page widgets + shared `page_util` (`dim_label`): `list_page` (`CameraListPage`), `add_page` (Source form + scans), `configure_page` (preview + orientation controls), `areas_page` (ROI editing). Pages own their controls and emit request signals; the coordinator drives them. |
| `ui/camera/dialog/wizard_stepper.{h,cpp}` | Non-interactive "① Source — ② Configure — ③ Areas" step indicator; `set_current()` emphasizes the active step. |
| `ui/camera/dialog/roi_canvas.{h,cpp}` | Draw-only `QWidget` for one ROI polygon over the oriented snapshot: click to add vertices, click first vertex / Enter to close, Backspace/Esc to undo/clear. |
| `ui/camera/dialog/camera_devices.{h,cpp}` | USB enumeration (Qt Multimedia). |
| `ui/camera/dialog/ip_scan.{h,cpp}` | Subnet RTSP-port scan (Qt Network, threaded). |
| `ui/camera/shared/snapshot.{h,cpp}`, `shared/frame_convert.h` | Grab one preview frame (OpenCV, off-thread) + orient it: `apply_orientation` composes rotation + roll + pitch (perspective warp) for the live Configure preview. |
| `ui/camera/shared/rtsp_templates.{h,cpp}` | Manufacturer → RTSP URL templates (Dahua) + `with_credentials`. |
| `ui/camera/shared/roi_geometry.{h,cpp}` | Pure (unit-tested) widget↔normalized point mapping + aspect-fit rect for the canvas. |

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
