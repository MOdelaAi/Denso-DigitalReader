# Dialog restructure + logging/export seam — design

**Date:** 2026-07-03
**Status:** Approved, ready for implementation plan
**Scope:** Maintainability restructure of the two heaviest dialogs, extraction of
shared dialog primitives, and an additive (dormant) seam for a future data
logging/export feature. **No user-visible behavior change.**

## Motivation

The codebase is already well-structured (clean `denso_core`/`denso` split,
feature-grouped UI, one-way dependency layers, pure unit-tested helpers, largest
file ~410L). The friction is not tangled files — it is a **shared shape problem**
in the two dialogs, and it is exactly what the coming *data logging/export*
feature will trip over:

1. **Each dialog is both a view-builder and a controller.** `CameraDialog` builds
   header chrome + owns wizard flow-state (`editing_id_`, `draft_`,
   `entered_areas_directly_`, `areas_expanded_`) + does threaded snapshot capture
   + does DB writes (`insert`/`update`/`set_camera_models`/`replace_areas`).
   `SettingsDialog` builds five panels + owns network threading
   (`run_async`/`refresh`/`apply`/`scan`/`connect`) + DB writes. View and
   orchestration are fused, so neither is independently testable and both files
   only grow.

2. **Duplicated chrome & helpers.** The header block (title + ✕ + gold
   underline), the `QThread::create` + `invokeMethod` async pattern, and small
   label factories (`eyebrow`/`dim_label`/`spec_row`/`hline`) are copy-pasted
   between the two dialogs (`dim_label` is *also* in camera's `page_util`). A new
   logging/export panel would copy them a third time.

3. **No persistence seam for readings.** Detections are computed in
   `DetectionProcessor` and drawn on the frame, then discarded. Logging/export
   needs them recorded — but there is nowhere for that to land today.

The highest-leverage move is therefore not "chop the big files" but **establish
one reusable dialog pattern (thin view + controller + shared chrome), then make
logging/export the first feature built on it.** This spec delivers the pattern
and the safe half of the logging seam; the logging/export UI is a separate
follow-up spec (Spec 2).

## Guiding principle

**Zero user-visible behavior change.** This is a pure structural refactor plus an
additive, dormant seam. Every flow that works today works identically after. The
only new runtime code path (reading capture) is wired to a *null sink* so it does
nothing until Spec 2 supplies one.

## Deliverables

### 1. New leaf module: `src/app/ui/common/`

A leaf that widgets depend on but which never depends on any feature — the same
architectural role `ui/camera/shared/` plays for the camera surfaces. It removes
the cross-dialog copy-paste.

| File | Contents | Replaces |
|---|---|---|
| `dialog_chrome.{h,cpp}` | `dialog_header(QDialog*, const QString& title)` → the header `QVBoxLayout*` (title label + ✕ close-glyph wired to `reject` + gold underline) | `camera_dialog.cpp`'s anon `header()` + `settings_dialog.cpp`'s inline header block |
| `async_runner.{h,cpp}` | `run_on_worker(std::function<void()>)` (the `QThread::create` + `finished→deleteLater` idiom) and `post_to_gui(QObject* ctx, std::function<void()>)` (wraps `QMetaObject::invokeMethod(ctx, fn, Qt::QueuedConnection)`) | `SettingsDialog::run_async` + the inline `QThread::create` block in `CameraDialog::capture_snapshot` |
| `form_widgets.{h,cpp}` | `eyebrow`, `dim_label`, `spec_row`, `hline` label/row factories | `settings_dialog.cpp`'s anon-namespace copies + `dialog/page_util::dim_label` |

**`page_util`:** keeps only genuinely page-specific bits (error colour). Its
`dim_label` becomes a thin re-export of `ui::common::dim_label` (or callers switch
to the common one) so there is a single definition.

**Dependency rule:** `ui/common/` is a leaf. Nothing in it includes a feature
header. `grid/`, `dialog/`, `settings/`, and the root shells may depend on it.

### 2. `SettingsDialog` → thin view + self-contained `NetworkPanel`

Move the network orchestration into a self-contained widget:

- **New `ui/settings/network_panel.{h,cpp}` (`QWidget`)** owns the two `NetCard`s,
  the Refresh button, its `QSqlDatabase` handle, the `eth_config_`/`wifi_config_`
  view-models, and the four handlers (`refresh`/`apply_net_config`/`scan_wifi`/
  `connect_wifi`) — all async work routed through `ui/common/async_runner`. It
  exposes `on_shown()` (re-seed editors from saved config + refresh status), which
  reproduces the current "entering the Network tab re-seeds the cards" behavior.
- **`SettingsDialog`** shrinks to: build the five panels, the left nav, the footer
  (Reset / Close / Apply), and the read-only seeding setters
  (`set_hardware`/`set_resolution_index`/…). `build_network()` becomes "construct
  and return the `NetworkPanel`"; the nav's `currentRowChanged` slot calls
  `network_panel_->on_shown()` for the Network row.
- The trivial panels (Appearance/Display/System/About — ~10 lines each) **stay
  inline**; they do not earn extraction (YAGNI).

**Result:** `settings_dialog.cpp` ~410 → ~230L; network logic becomes an
independently understandable, independently openable widget.

### 3. `CameraDialog` → thin view + `CameraWizardController`

Split the fused coordinator:

- **New `ui/camera/wizard_controller.{h,cpp}` (`QObject`)** owns the wizard
  flow-state (`editing_id_`, `draft_`, `last_frame_`, `entered_areas_directly_`),
  the threaded snapshot capture (via `async_runner`), and **every** persistence
  call (`camera::insert`/`update`, `detection::set_camera_models`,
  `camera::replace_areas`, `camera::areas_for`). It connects to the page signals,
  drives transitions by calling back into the view's `show_page(int)` + typed page
  accessors, and emits `cameras_changed()`.
- **`CameraDialog`** keeps only: widget construction (the `QStackedWidget` of five
  pages + the `WizardStepper`), the header (via `dialog_chrome`), and the sizing
  logic (`show_page`/`expand_for_areas`/`restore_size`). It exposes `show_page(int)`
  and page accessors to the controller.
- The subtle **Back-routing branches** (editing-vs-add: Configure Back →
  list-or-Source; direct-areas: Areas Back → list-or-Models) move **verbatim**
  into named controller methods. Relocated, not rewritten — behavior is provably
  preserved by inspection.

**Interface between view and controller:** the controller holds a pointer to the
view (or to a minimal interface exposing `show_page(int)` + the page getters). The
view constructs the controller, hands it `db_` + itself, and forwards nothing else.

**Result:** `camera_dialog.cpp` ~327 → ~170L of view; the flow graph lives in one
place.

### 4. Logging/export seam

**Build now — isolated, additive, unit-tested (safe half):**

- **`src/core/reading/reading.h`** — Qt/OpenCV-free domain struct:
  ```
  struct Reading {
      int64_t     id        = 0;
      int64_t     camera_id = 0;   // FK → camera.id
      int64_t     ts_ms     = 0;   // capture time, epoch ms
      std::string value;           // the reading (assembly deferred to Spec 2)
      float       conf      = 0.0f;
  };
  ```
- **`src/core/reading/repo.{h,cpp}`** — `insert(db, const Reading&)` returning the
  new id (or `nullopt` on failure, matching `camera::insert`), and
  `query(db, camera_id, from_ms, to_ms)` returning `std::vector<Reading>` ordered
  by `ts_ms` (the read path the future export UI consumes).
- **Migration v9** in `db::run_migrations` — a new `if (version < 9)` block
  creating the `reading` table (`id` PK, `camera_id` FK, `ts_ms`, `value` TEXT,
  `conf` REAL) with an index on `(camera_id, ts_ms)`. Additive; never edits a
  shipped migration. Bump `SCHEMA_VERSION` to 9.
- **`tests/test_reading_repo.cpp`** — Catch2, mirroring `test_camera_repo`:
  insert → query round-trip, time-range filter, empty result, ordering.

**Add the seam, wired to nothing (dormant):**

- In `ui/camera/grid/frame_processor.h`:
  ```
  struct ReadingSink {
      virtual ~ReadingSink() = default;
      virtual void on_reading(int64_t camera_id, int64_t ts_ms,
                              const std::vector<NamedDetection>& kept) = 0;
  };
  ```
- `DetectionProcessor` gains `int64_t camera_id_ = 0` and
  `ReadingSink* sink_ = nullptr` (both via constructor, defaulted). After
  `merge_detections`, before/after drawing: `if (sink_) sink_->on_reading(camera_id_, now_ms, kept);`
- **No sink is constructed anywhere in this spec.** `camera_grid` continues to
  build `DetectionProcessor` with no sink → the branch is never taken → identical
  runtime behavior today.

**Documented contract (the reason the seam exists):** `DetectionProcessor::process`
runs on the **capture thread**. The eventual sink (Spec 2) **must hand off** to a
persistence worker (a thread-safe queue or `post_to_gui`) — it must never do DB
I/O inline, which would stall the capture loop and block stream teardown (the same
hazard that got TensorRT moved off the capture thread). The seam interface only
passes data; it imposes no threading of its own.

**Explicitly deferred to Spec 2 (product decisions, not plumbing):** how a set of
detected digit boxes assembles into a single reading `value` (left-to-right box
ordering? per-frame vs. debounced-on-change? confidence aggregation?), the sink
implementation, the persistence worker, and the logging/export UI panel (built on
the new thin-view + controller pattern from §2–3).

### 5. Health-audit wins folded in

- The helper de-dup (via `ui/common`, §1) — the primary structural win.
- **`.clang-format`** at repo root matching the existing style (4-space indent,
  ~100 col, attach braces). Does **not** reformat existing code; keeps future
  edits consistent. Verified `--dry-run` clean on the new files only.

**Deferred backlog (recorded, NOT done here — avoids scope creep):**

- No CI pipeline (build + `ctest` on push).
- No app-layer / Qt-widget tests (only `denso_core` is covered today).
- No `clang-tidy` config.

These are listed so they are on the record; each is its own future slice.

### 6. Documentation (first-class deliverable)

- **`CLAUDE.md`** — Layout table gains `ui/common/` and `core/reading/` rows; the
  camera + settings rows note the view/controller split. Hard rules gains:
  *"Dialogs are a thin view + a controller/panel that owns flow-state, async work,
  and persistence. Shared dialog chrome (header, async runner, label factories)
  lives in `ui/common/` and is never re-copied into a feature."*
- **`ARCHITECTURE.md`** — new sections for `ui/common/`, `NetworkPanel`,
  `CameraWizardController`, and the `core/reading/` module; document the reading
  seam and its capture-thread threading contract; note migration v9.

## Testing & verification

- **New `reading` repo:** Catch2 unit tests; `ctest --test-dir build` stays green
  (54 → 55+ test cases).
- **Dialog refactors:** behavior-preserving. Verified by clean build + **manual
  smoke** of both dialogs' full flows: Settings (theme/resolution/fullscreen
  apply; Network refresh + apply + Wi-Fi scan/connect); Camera (add USB, add IP,
  Configure capture, Models attach, Areas draw/skip, per-row Areas, delete). No
  new automated tests (Qt widget flows are not in the suite), no regressions.
- **`.clang-format`:** `clang-format --dry-run --Werror` clean on all new files.

## Risks

- **Camera Back-routing branches** — the highest-risk bit. Mitigation: relocate
  the branch code verbatim into the controller; diff-review that no condition
  changed.
- **Qt object ownership** when moving widgets into `NetworkPanel` / the controller
  — ensure correct parent so nothing is double-freed and no widget is orphaned.
  Mitigation: parent widgets to the panel/dialog as today; the controller
  (`QObject`) is parented to the dialog.
- **Include-cycle / leak of feature headers into `ui/common`** — guard by keeping
  `ui/common` headers free of any `camera/`, `network/`, or `detection/` include.

## Non-goals

- No change to any user-visible behavior, styling, or flow.
- No logging/export UI, no live reading persistence, no reading-assembly logic
  (all Spec 2).
- No CI, app-layer tests, or clang-tidy (deferred backlog).
- No touching the network backends, detection runtime, or capture/threading model
  beyond adding the dormant sink pointer.

## Follow-up: Spec 2 (data logging/export)

Built on this foundation: a `ReadingSink` implementation that assembles readings
and hands them to a persistence worker writing via `reading::insert`; a Logs UI
panel (thin view + controller, using `ui/common`) that queries `reading::query`
with camera + time-range filters and exports (CSV). Out of scope here.
