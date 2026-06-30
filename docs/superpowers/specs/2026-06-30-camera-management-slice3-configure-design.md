# Camera management — Slice 3: Configure step (snapshot + capture settings)

**Date:** 2026-06-30
**Status:** Approved design — ready for implementation plan

Part of the camera feature roadmap (see
`2026-06-30-camera-management-slice2-design.md`). Slice 2 made cameras real
(persist + list/delete + add via scan/manual). This slice adds the **Configure**
wizard step: capture a live snapshot from the camera and set its capture
settings (resolution, fps, rotation) and angle (pitch, roll). It introduces
OpenCV. Draw ROI areas (Slice 4), the live preview grid (Slice 5), and
detection (Slice 6) come later.

## Roadmap context

1. ✅ Empty-state `CameraView`.
2. ✅ Camera persistence + Camera modal (list/delete) + Add (scan/manual).
   (Extended since: IP channel + structured stream/manufacturer fields.)
3. **This slice** — Configure step: OpenCV snapshot preview + resolution / fps /
   rotation / pitch / roll, reachable from the add wizard **and** from existing
   rows (doubles as edit). 
4. Draw ROI areas.
5. Live 1–4 preview grid.
6. Detection/model.

## Decisions (locked)

- **Flow:** Configure is an add-wizard step **and** the edit entry for existing
  cameras.
- **Capture:** OpenCV `cv::VideoCapture` (added now — the capture/detection
  slices need it anyway). Confirmed available in the MSYS2 UCRT64 toolchain.
- **Resolution:** a fixed **preset list**.
- **Controls:** all five — snapshot preview + resolution + fps + rotation +
  pitch + roll.

## Data model & persistence

No schema change. The `camera` table and `Camera` struct already carry `width`,
`height`, `fps`, `pitch`, `roll`, `rotation` (filled with defaults by Slice 2's
quick-save). This slice populates them through the Configure UI via the existing
`camera::insert` / `camera::update`.

## Build wiring

- Root `CMakeLists.txt`: `find_package(OpenCV REQUIRED)`.
- `src/app/CMakeLists.txt` (`denso` target only): add the snapshot unit to the
  sources, `target_include_directories(... ${OpenCV_INCLUDE_DIRS})`,
  `target_link_libraries(... ${OpenCV_LIBS})`. **`denso_core` stays
  Qt6::Sql-only** — OpenCV is app-side, mirroring how Qt Multimedia is app-side.
- `tests/CMakeLists.txt`: link OpenCV into `denso_tests` for the pure
  conversion/rotation tests, and compile the snapshot unit's testable pieces in
  (see Testing).

## Unit 1 — snapshot helper (`src/app/ui/camera/snapshot.{h,cpp}`, new)

`snapshot.h` is **OpenCV-free** so `camera_dialog.cpp` need not include cv
headers:

```cpp
struct Snapshot {
    QImage  image;   // null on failure
    QString error;   // human-readable reason when image is null
};

// Open the USB `index` OR the RTSP `url` (exactly one is set), apply the
// requested width/height + a finite open/read timeout, grab one frame.
Snapshot grab_snapshot(std::optional<int> index, const QString& url,
                       int width, int height);

// Rotate a frame for preview. degrees ∈ {0, 90, 180, 270}; 0 is identity.
QImage apply_rotation(const QImage& src, int degrees);
```

`snapshot.cpp` includes OpenCV and a tiny sibling header
`frame_convert.h` exposing `QImage mat_to_qimage(const cv::Mat& bgr)` (BGR→RGB,
**deep copy** so the QImage owns its bytes). `frame_convert.h` is included only
by `snapshot.cpp` and the test — it keeps the cv type out of the dialog's
include graph while staying unit-testable.

`grab_snapshot` behaviour:
- USB: `cv::VideoCapture(*index)`. IP: `cv::VideoCapture(url.toStdString())`.
- Set a finite open/read timeout (`cv::CAP_PROP_OPEN_TIMEOUT_MSEC` /
  `cv::CAP_PROP_READ_TIMEOUT_MSEC`) so an unreachable RTSP host fails fast
  instead of hanging the worker thread.
- If `width > 0 && height > 0`, set `CAP_PROP_FRAME_WIDTH/HEIGHT` before reading.
- `!isOpened()` → `error = "Could not open the camera."`; empty frame →
  `error = "No frame received from the camera."`.
- Success → `image = mat_to_qimage(frame)`.

## Unit 2 — Configure page (third page in `CameraDialog`'s `QStackedWidget`)

Built in the existing dialog (it already owns the stack, the `field(...)` helper,
and the off-GUI-thread scan pattern). New widgets:

- **Snapshot preview**: a `QLabel` (fixed/min size, holds the pixmap; shows a
  dim "Click Capture to preview" placeholder until first capture) + a
  **Capture / Refresh** `QPushButton`.
- **Resolution** `QComboBox` from the preset table (below).
- **FPS** `QSpinBox` (1–60, default 30).
- **Rotation** `QComboBox`: `0° / 90° / 180° / 270°` (itemData = degrees).
- **Pitch** and **Roll** `QDoubleSpinBox` (range −45.0…45.0, step 0.5, suffix
  `°`, default 0). Themed by the `QAbstractSpinBox` rule added in the channel
  slice.
- Footer: **Back** (→ source page in add mode, → list in edit mode) + **Save**.

**Capture** reuses the IP-scan threading pattern: disable the button + set
"Capturing…", run `grab_snapshot` on a `QThread::create` worker, post the result
back with `QMetaObject::invokeMethod(..., Qt::QueuedConnection)`. On success set
the preview pixmap with `apply_rotation` applied and scaled to the label
(`Qt::KeepAspectRatio`); on failure show `Snapshot::error` in the preview area.
Capture uses the currently-selected resolution; changing rotation re-renders the
existing frame without re-grabbing.

### Preset resolution table (single source of truth)

A `static const std::vector<{QString label; int w; int h;}>` in the dialog
(or a small `resolutions.h`):

| Label | W × H |
|-------|-------|
| 640 × 480 | 640 × 480 |
| 1280 × 720 | 1280 × 720 |
| 1920 × 1080 | 1920 × 1080 |
| 2560 × 1440 | 2560 × 1440 |

Default selection: **1280 × 720**. On edit, if the stored `width×height` matches
no preset, prepend a `"<W> × <H> (custom)"` entry and select it, so an existing
size is never silently lost.

## Flow & integration

**Mode state** on `CameraDialog`: `std::optional<int64_t> editing_id_` and a
`Camera draft_`.

**Add wizard.** The source page's primary button changes from **Save** to
**Configure…**. It validates the source (as today), builds `draft_` from the
chosen source (USB → `cam_index`; IP → `ip`/`rtsp`/`channel`/`stream`/
`manufacturer`/`username`/`password`), sets `editing_id_ = nullopt`, populates
the Configure page with defaults, and switches to it. The standalone quick-save
is dropped — every add passes through Configure, where the capture settings are
pre-filled with defaults, so it is one extra click and no extra required data
entry.

**Edit entry.** Each list row gains a **Configure** button next to Delete →
loads the `Camera` via `camera::get`, sets `editing_id_ = id` and `draft_ = cam`,
populates the Configure controls from it, switches to the Configure page, and
auto-captures from the stored source (RTSP credentials re-injected with
`with_credentials(rtsp, username, password)`).

**Configure Save.** Reads the controls into `draft_` (`width`/`height` from the
preset, `fps`, `rotation`, `pitch`, `roll`), then:
- add mode (`editing_id_` empty) → `camera::insert(draft_)`,
- edit mode → `draft_.id = *editing_id_; camera::update(draft_)`.

On success emit `cameras_changed()` and return to the list; on DB failure show
the existing inline error.

## Preview scope (deliberate cut)

The preview applies **rotation** only. **Pitch/roll are persisted but get no
live perspective-warp preview** this slice — angle correction rendering is
deferred to the draw/detection slice where it belongs. The controls exist and
their values round-trip to the DB.

## Error handling

- Capture failures never block the UI (always off-thread) and surface as text in
  the preview area.
- Unreachable RTSP fails fast via the capture timeout rather than hanging.
- DB write failures reuse the dialog's existing inline error label.

## Testing

**Pure unit tests** (`tests/test_snapshot.cpp`, new — OpenCV linked into
`denso_tests`):
- `mat_to_qimage`: a synthetic `cv::Mat` (known BGR pixels) → `QImage` of the
  same dimensions with channels in **RGB** order (assert a known pixel's
  `qRed/qGreen/qBlue`), and the QImage owns its data (outlives the Mat).
- `apply_rotation`: 90°/270° swap width/height; 0° is identity; a corner pixel
  lands where expected after 90°.

`grab_snapshot` opens real hardware / a real stream, so it is **verified
manually on-device** (USB index path and an RTSP path), not in CI — documented
here and in the plan's manual-verification step.

To keep the pure tests buildable without dragging Qt Widgets into the test
target, the tested functions (`mat_to_qimage`, `apply_rotation`) depend only on
QtGui (`QImage`) + OpenCV, both already linkable; `tests/CMakeLists.txt` compiles
`snapshot.cpp` into `denso_tests` (it has no Qt Widgets dependency) and links
OpenCV + `Qt6::Gui`.

## Out of scope (later slices)

Draw ROI areas; the live 1–4 preview grid; detection/model; pitch/roll
perspective-warp preview; per-device capability enumeration for resolution;
ONVIF auto-discovery; moving IP credentials to the OS secret store.
