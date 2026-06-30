# Camera management — Slice 2: CRUD + modal + add (scan/manual)

Part of the camera feature roadmap. The app already shows a "no cameras yet"
empty state (Slice 1). This slice makes cameras real: persist them, and turn the
top-bar **Camera** modal into the management hub where you add, list, and delete
cameras. Capture/preview, the configure/draw wizard steps, and detection come in
later slices.

## Roadmap context

1. ✅ Empty-state `CameraView`.
2. **This slice** — camera persistence + Camera modal (list/delete) + Add (scan
   USB / manual IP → save with defaults). No OpenCV.
3. Configure step (snapshot preview, pitch/roll/rotation/res/fps) — adds OpenCV.
4. Draw ROI areas.
5. Live 1–4 preview grid.
6. Detection/model.

The add-camera wizard is **Scan → Configure → Draw → Save**, with Configure and
Draw skippable ("edit later"). This slice builds **Scan → Save**; Configure/Draw
are skipped and persisted as defaults.

## Data model & persistence

The domain types already exist in `core/camera/model.h` (`Camera`,
`CameraArea`, `CameraWithAreas`) — unchanged.

**Migration v4** (`db::run_migrations`, bump `SCHEMA_VERSION` 3→4): create two
tables mirroring the structs (additive; never edits a shipped migration):

- `camera(id INTEGER PK, name TEXT, camera_type TEXT, active INTEGER,
  cam_index INTEGER, ip TEXT, rtsp TEXT, username TEXT, width INTEGER,
  height INTEGER, fps INTEGER, pitch REAL, roll REAL, rotation INTEGER)`
  — optional/type-specific columns are nullable.
- `camera_area(id INTEGER PK, camera_id INTEGER, name TEXT, x1 REAL, y1 REAL,
  x2 REAL, y2 REAL)` — created now; area CRUD lands in Slice 4.

**`core/camera/repo.{h,cpp}`** (denso_core, `Qt6::Sql`, mirrors the existing
network/settings repos — typed NULL binds, function-scoped queries):

- `std::optional<int64_t> insert(db, const Camera&)` — new row, returns its id.
- `bool update(db, const Camera&)` — by id.
- `bool remove(db, int64_t id)` — also deletes the camera's areas.
- `std::optional<Camera> get(db, int64_t id)`.
- `std::vector<Camera> all(db)` — ordered by id.

Tests `tests/test_camera_repo.cpp`: insert→get/all roundtrip (USB and IP rows,
NULL round-trip for the type-specific columns), update, remove (incl. cascade of
areas). Runs under the existing Catch2 harness.

## USB enumeration

App-side helper (keeps `denso_core` free of `Qt6::Multimedia`): list available
USB cameras via `QMediaDevices::videoInputs()` → `(description, position index)`.
The stored `cam_index` is the device's position in that list — the integer
`cv::VideoCapture` will later open. (Order-vs-OpenCV-index mismatch is a known
caveat, revisited in Slice 3.)

## UI — the Camera modal (management hub)

Replace the placeholder `CameraDialog` body with two views in a `QStackedWidget`:

- **List view** (default): one row per camera (name + `USB`/`IP` badge) with a
  **Delete** button; an empty "No cameras — add one to get started" state when
  none; a footer **+ Add Camera** button → wizard. (Edit is a later slice.)
- **Add view** (wizard step 1 — Scan/Source):
  - **USB**: the auto-listed `QMediaDevices` cameras as selectable rows.
  - **IP**: a manual form — name, RTSP URL, username. (No password field this
    slice: the `Camera` model intentionally has no password column, and nothing
    authenticates to the stream until capture lands in Slice 3, where
    credential handling is designed.)
  - **Save** builds a `Camera` (defaults: `active=true`, dims/fps `0`,
    pitch/roll/rotation `0`; USB → `cam_index`; IP → `ip`/`rtsp`/`username`),
    `camera::insert`s it, and returns to the list. **Cancel** returns without
    saving. Configure/Draw are skipped this slice.

The dialog takes the `QSqlDatabase` (like `SettingsDialog`) and emits
`cameras_changed()` whenever the list mutates.

## Main view reflects camera count

`CameraView` (main content) currently always shows the empty state. Give it a
`reload()` that queries `camera::all` count:

- `0` → the existing empty state.
- `>0` → a simple placeholder ("N camera(s) configured — live preview coming
  soon"). The real 1–4 grid is Slice 5.

`MainWindow` owns the wiring: it passes `db_` to the Camera modal, connects
`cameras_changed()` → `CameraView::reload()`, and calls `reload()` at startup.

## Build

- `denso_core`: + `camera/repo.cpp`.
- `denso` (app): + `Qt6::Multimedia` link; + the USB-enumeration unit + modal
  changes + `CameraView::reload`.
- `denso_tests`: + `test_camera_repo.cpp`.

## Out of scope (later slices)

OpenCV / capture / snapshot preview; the Configure and Draw wizard steps; editing
an existing camera; the live preview grid; detection/model; IP credentials /
password handling (designed with capture in Slice 3, headed for the OS secret
store); ONVIF auto-discovery.
