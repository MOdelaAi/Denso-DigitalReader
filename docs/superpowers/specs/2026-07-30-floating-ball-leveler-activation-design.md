# Floating Ball Leveler — Production Activation (design)

Status: **APPROVED — operator decisions recorded 2026-07-30.**
Baseline: `b737a96` (== `origin/main`), package `0.1.0+r427.gb737a96`, schema **v13**.
Target hardware: Jetson Orin Nano `192.168.1.15` only. Unix Makefiles only.

**Supersession.** This specification explicitly supersedes the destructive
switch-and-reset behaviour approved in
`docs/superpowers/specs/2026-07-21-operating-modes-switch-and-reset-design.md`.
Mode switching becomes non-destructive (§3.2, §4.1). That earlier document remains
the historical record of the shipped behaviour; where the two disagree, **this
document governs**.

---

## 1. Purpose

Implement the complete Floating Ball Leveler pipeline — configuration, inference,
measurement, annotation, persistence — and only then remove the production guard
`"Floating Ball Leveler setup is not available in this release."`

The guard exists in **three** places, not one. All three are load-bearing and all
three are removed together, last:

| # | Site | What it locks |
|---|------|---------------|
| 1 | `src/app/ui/settings/mode_confirm_text.cpp:79` | the pre-commit confirmation paragraph |
| 2 | `src/app/ui/camera/camera_view.cpp:232` | the retained-connections "unavailable" page |
| 3 | `src/app/ui/mainwindow.cpp:213-214` `apply_camera_button_gate()` | **disables the Camera button entirely in `ball_leveler`** |

Site 3 was not named in the project brief and is the one that actually prevents
the wizard from being opened. Removing only the message strings would leave the
feature unreachable.

`src/core/mode/config.h:29-31` additionally hardcodes `mode_setup_required ==
true` for `ball_leveler` "ALWAYS true this release". That is a fourth lock, on
the status surface.

---

## 2. Verified baseline facts

Established by reading `b737a96`, not assumed.

* Modes live in `settings["mode.target"]`; `digit_reader` is the fail-safe default
  (`src/core/mode/mode.h:26-29`).
* The **central compatibility policy already supports Ball Leveler in full**
  (`src/core/models/compatibility.cpp:32-46`): `float_ball → { ball_leveler }`,
  `digit_numeric → { digit_reader }`, enforced by `static_assert` that no family
  may be allowed in both modes. **This project adds no rule to that matrix and
  does not duplicate it.**
* The runtime model root holds exactly the engine-only three-model set:
  `digitv3`, `float-small` (class `["Small"]`), `float-big` (class `["Big"]`),
  each `<id>.engine` + `<id>.names.json`, plus schema-2 `manifest.json`.
  Verified on `.15`; no `.onnx`/`.pt` present.
* Both Float models are **single-class** per the installed manifest.
* Annotation boundary is proven by the existing zone overlay: orientation →
  `QImage`→BGR `cv::Mat` → worker gets its own copy → draw boxes → draw zone
  overlay → `mat_to_qimage()` (`src/app/camera/frame_processor.cpp:112-153`).

---

## 3. The two load-bearing findings

Both were raised by Codex 5.6 and both were then **verified against source**.

### 3.1 `EngineRegistry` freezes the boot mode's allow-list — VERIFIED

`startup.cpp:196-203` builds the allow-list from
`loadable_model_files(mode, metadata)` using the mode committed **at boot**, and
hands it to one `EngineRegistry` that lives for the process.
`engine_registry.cpp:42-53` **throws `std::logic_error`** for any filename outside
that list, and its own comment states "the allow-list is immutable after
construction". `WarmupState::start()` is one-shot (`warmup_state.cpp:21-24`).
`MainWindow` rebuilds the camera view **in-process** after a switch
(`mainwindow.cpp:324-410`) — it does not restart.

Consequence: boot in `digit_reader`, switch live to a configured `ball_leveler`,
and the first `engines_->get("float-small.engine")` throws. `start_one` catches
it (`camera_grid.cpp:531-538`) so the app survives, but **every Ball camera shows
Offline forever**. The destructive reset hid this, because the destination mode
was always left with nothing configured.

This is the single change without which the feature cannot work at all.

**Decision — replace the registry instance on a committed mode switch.**
*(Operator decision 2, approved 2026-07-30.)*

Required properties of that replacement:

* the GUI stays responsive throughout;
* the existing `Preparing model…` tile state is shown while the destination
  models warm;
* every old capture and inference thread is **fully stopped and joined** before
  the old registry is released — the existing teardown-before-transaction
  ordering (`mainwindow.cpp:324-339`) already guarantees this and must be kept;
* the replacement is a **new immutable, mode-pure** `EngineRegistry` instance;
* only models the central policy allows in the **committed target mode** are
  warmed;
* **never** a union allow-list;
* **no application restart** is required.

On Jetson `.15` the pause is the cost of **deserialising the approved prebuilt
`.engine` files and running the normal warm-up inference**. No TensorRT engine is
built at runtime and no `trt_cache` is required on the Jetson production path —
the engine bytes are the approved artifact and are loaded as-is.

Rejected alternatives:

* *Union allow-list at boot* — directly violates the required structural property
  "no wrong-mode engine can reach warm-up or inference", and would deserialize all
  three engines every boot on an 8 GB device.
* *Require an application restart* — there is **no supervisor**. The app is
  launched from `/usr/share/applications/com.denso.DigitalReader.desktop`
  (`Exec=/usr/bin/denso-digitalreader`, no `Restart=`); `denso-setup configure
  --autostart` seeds a session autostart, which restores on *session* start, not
  on process exit. Verified on `.15`: no systemd unit for the app exists. So an
  exit would leave the appliance dark.

Replacing the instance **preserves** the security property exactly: each registry
is still immutable and mode-pure for its whole life. We swap the object, never the
list. Ordering is already correct in `mainwindow.cpp`: teardown (joins every
capture + inference thread) happens *before* the transaction, so the old registry
has no live reader when it is dropped.

### 3.2 Mode-blind persistence — VERIFIED, and it is wider than the brief assumes

The current switch is **destructive by design** (`src/core/mode/reset.cpp:80-85`):
it deletes `camera_model_class`, `camera_model`, `camera_area`, `reading`,
`model_migration_receipt` and zeroes `setup_complete` / `areas_need_review`. The
confirmation dialog promises the operator exactly that, in writing, and says
"This cannot be undone".

This document requires the opposite, and **supersedes** the shipped `2026-07-21`
spec on this point *(operator decision 1, approved 2026-07-30)*. A mode change
must preserve **all** of:

* Digital Reader model attachments, class selections, areas and configuration;
* Floating Ball Leveler model binding and calibration;
* camera connections.

This is a deliberate reversal of approved behaviour. The operator-facing
confirmation text must be **rewritten, not reworded**: it must no longer promise
deletion, and must no longer say the operation cannot be undone. It currently
makes a promise of destruction that becomes false.

Every read/write below is mode-blind today (verified line by line):

| Site | File:line | Effect once both modes coexist |
|------|-----------|-------------------------------|
| `camera::runtime()` | `core/camera/repo.cpp:180` | `active=1 AND setup_complete=1` — mode-blind fleet |
| `models_for` | `core/detection/repo.cpp:149-150` | wizard shows the other mode's attachments |
| `set_camera_models` | `core/detection/repo.cpp:264-271, 280` | saving one wizard **deletes the other mode's binding** |
| `detection_for` | `core/detection/repo.cpp:311-313` | one preserved wrong-mode row inhibits the camera |
| `try_attached_model_filenames` | `core/detection/repo.cpp:98` | wrong-mode rows enter the warm-up required set |
| `evaluate_integrity` | `core/health/integrity.cpp:197-199` | dormant rows judged against the active mode → `--check` exits **10** |
| `load_old_attachment` | `core/detection/migrate.cpp:33-35` | migration matches by `(camera_id, filename)`, no mode |

**Decision — do NOT add `mode` to `camera_model`.** Codex's argument holds and the
table above is the evidence: scoping that column correctly means touching seven
call sites including the `--check` exit-code contract and the rollback-receipt
machinery, for no gain. Instead:

* `camera_model` / `camera_model_class` / `camera_area` / `reading` /
  `model_migration_receipt` stay **`digit_reader`-only**, and are simply never
  deleted.
* Ball Leveler gets **one cohesive table** carrying its complete binding *and*
  calibration, and **one** write chokepoint.

This also fixes a real flaw in my first draft, which put `model_id` in *both*
`camera_model` and the calibration row — two durable model authorities. There is
now exactly one.

`set_camera_models` is **not** reused for Ball Leveler. Its contract is an
*ensemble* — N models, a per-model class subset, a per-class confidence, and
cross-model merge/NMS (`frame_processor.h:89-103`). Ball Leveler is one model, one
class, one threshold. Reusing it would make five invalid states representable
(zero models, multiple models, zero classes, multiple classes, and a confidence in
`camera_model_class` disagreeing with the calibration's). "One model-binding
authority" means one authoritative *write API per domain*, not one shared table.

---

## 4. Architecture

### 4.1 Persistence — schema v13 → v14

Two additive changes. No existing table is dropped, altered destructively, or
given new semantics.

```sql
CREATE TABLE ball_level_calibration (
    camera_id      INTEGER PRIMARY KEY REFERENCES camera(id),  -- ONE per camera, by schema
    model_id       INTEGER NOT NULL REFERENCES model(id),
    class_id       INTEGER NOT NULL,
    conf           REAL    NOT NULL,
    rect_x         REAL    NOT NULL,   -- normalized [0,1], oriented-frame coords
    rect_y         REAL    NOT NULL,
    rect_w         REAL    NOT NULL,
    rect_h         REAL    NOT NULL,
    y_100          REAL    NOT NULL,   -- normalized [0,1]
    y_0            REAL    NOT NULL,
    hold_ms        INTEGER NOT NULL,
    view_revision  TEXT    NOT NULL    -- fingerprint of view-significant camera fields
);
```

`camera_id PRIMARY KEY` is what makes "one Ball Leveler configuration per camera"
true by construction rather than by a rule someone can forget. It kills the
multi-tank mutation at the schema level.

`class_id` is stored even though both installed Float models are single-class: the
manifest does not *guarantee* single-class for the family, and deriving it would
be a second rule.

`view_revision` closes a hazard Codex identified and I confirm: calibration is
expressed in oriented-frame coordinates, so a change to rotation / pitch / roll /
width / height / source makes the stored geometry refer to a different physical
view. `camera_area` already has this problem and solves it with the
`areas_need_review` quarantine (`camera_grid.cpp:434-440`). Ball Leveler must not
silently keep measuring against stale geometry. On a view-significant edit the
stored revision no longer matches and the camera resolves **`CalibrationInvalid`**
— it does not measure, and it does not delete the operator's work.

Migration v14 requirements: upgrade test from a populated v13 DB, restart test,
explicit assertion that every v13 digit row survives byte-identical, rollback via
the existing `/opt/denso/data` backup step, and a `docs/` schema-version note.

**`switch_and_reset` → `switch_mode`.** Non-destructive: writes `mode.target`,
sets `brazing.enabled = 0`, preserves `brazing.base_url`, deletes nothing, in one
checked transaction. `preview_counts` exists solely to quantify destruction
(`reset.cpp:22-56`) and `mainwindow.cpp:241-266` *blocks the switch* when it
cannot be read — that gate becomes wrong once nothing is destroyed, and is
removed with the counts.

**Runtime admission becomes mode-aware.** `camera::runtime()` stays as-is for
`digit_reader`. Ball Leveler admits `active = 1` cameras and resolves per-camera
state in the grid, so `Unconfigured` is a *visible runtime state* rather than a
row filtered out before it can be shown. This is what makes required test 20
("unconfigured camera remains explicitly unavailable") observable.

`mode_setup_required` loses its `ball_leveler → always true` hardcode and becomes
"no camera has a valid Ball Leveler calibration".

### 4.2 Pure core — `src/core/level/`

No Qt widgets, no OpenCV, no SQL. Exhaustively unit-testable without a GPU or a
display, exactly like `models/compatibility.cpp`.

* `calibration.{h,cpp}` — `LevelCalibration` value type + `validate()` returning a
  stable reason code. Rejects: `y_0 <= y_100`; span below `kMinSpanNorm`;
  degenerate rect; either line outside the rect; any non-finite field.
* `mapping.{h,cpp}` — `std::optional<double> level_percent(y_ball, y_100, y_0)`.
  Image coords, Y down: `((y_0 - y_ball) / (y_0 - y_100)) * 100`, clamped `[0,100]`.
  Returns `nullopt` on non-finite input or an unsafe denominator — it never
  fabricates a number.
* `select.{h,cpp}` — deterministic selection. Reject malformed first (non-finite
  x/y/w/h/conf, non-positive w/h), require the reference point inside the rect
  with a documented boundary convention (`[x, x+w)` half-open), then highest
  confidence, then a total tie-break: smaller `y_centre` → smaller `x_centre` →
  smaller `class_id`. Total ordering, so the result is reproducible.
* `state.{h,cpp}` — the **state machine**, owned in one place. Codex is right that
  this matters more than the enum: without it, transition logic scatters across
  processor, grid, overlay and status writer, creating a second policy authority.
  States: `Unconfigured`, `Acquiring`, `Healthy`, `HoldingLastValid`, `Inhibited`,
  `Paused`, `ModelUnavailable`, `CalibrationInvalid`. Injected clock — no wall time
  read inside.
* `runtime.h` — `LevelRuntimeEntry { camera_id, state, optional<double> percent,
  model_id, ts_ms }`. **Keyed by `camera_id`, never `zone_no`.**
* `repo.{h,cpp}` — one chokepoint `save_level_configuration(...)` that resolves the
  model through the manifest, calls the central
  `models::model_compatibility(BallLeveler, metadata)`, enforces exactly one
  model + one class, validates the calibration, and writes one row — all in one
  transaction, refusing the whole thing on any failure.

`ZoneRuntimeState` is deliberately **not** reused: four states vs eight,
zone-keyed vs camera-keyed, and `ZoneAggregator` owns a debounce/hold policy whose
semantics are wrong here. Reusing it would create the second policy authority the
brief forbids.

Documented, testable defaults (no silent constants): `kMinSpanNorm`,
`kDefaultConf`, `kDefaultHoldMs`, `kAcquireSamples`.

### 4.3 Runtime

`BallLevelProcessor : FrameProcessor` in `src/app/camera/level_processor.{h,cpp}`,
copying `DetectionProcessor`'s drop-oldest worker pattern **and its full ownership
contract**: every member initialized before the thread starts, stop-and-join in the
destructor, mutex-guarded pending slot and published snapshot, exceptions caught
inside the worker, and any GUI-bound callback carrying the grid `generation_` so a
torn-down generation cannot inhibit a rebuilt grid (`camera_grid.cpp:518-526`).
No SQLite on the worker or capture thread.

`CameraGrid::reload()` branches at the **subsystem** level, not inside
`start_one()`. Codex is right and the source confirms it: `reload()` builds
`ZoneHealth` (`camera_grid.cpp:286`) and the brazing/zone reporters
(`camera_grid.cpp:306-327`) before any per-camera work, and `start_one()` reads
`camera_area` and wires zone plumbing (`camera_grid.cpp:434-484`). Branching only
at processor construction would make Ball Leveler inherit digit quarantine and
zone-reporting semantics.

```
digit_reader:  runtime() → ZoneHealth → Zone/Brazing reporters → DetectionProcessor
ball_leveler:  active cameras → calibration resolution → level state owner →
               BallLevelProcessor
               (no camera_area, no ZoneHealth, no ZoneReporter, no BrazingReporter)
```

### 4.4 Annotation

`src/app/camera/level_overlay.{h,cpp}` — pure OpenCV, mirroring `zone_overlay`.
Draws the measurement rectangle, both reference lines, the selected ball box, its
centre marker, `LEVEL 67.4%`, and `STATE …` when not Healthy.

Called from `BallLevelProcessor::process()` **immediately before its final
`mat_to_qimage()`**, on the display-only Mat — after the worker has taken its own
copy. That is the boundary `frame_processor.cpp:142-153` already proves. Drawing
in the worker is wrong (that Mat is never displayed); drawing in a widget after
conversion is forbidden. No second Qt panel.

### 4.5 Calibration UI

Wizard step 4 becomes mode-dependent: **Areas** in `digit_reader`, **Level
calibration** in `ball_leveler`. New `LevelCalibrationPage` + a canvas for one
axis-aligned rect and two draggable horizontal lines, with both lines constrained
inside the rect and 100% above 0%.

The Models step **does** need work — my first draft was wrong and Codex caught it.
`ModelsPage` is an ensemble + per-class-checklist UI (`models_page.h:68-90`); Ball
Leveler needs single-select. It keeps `detection::selectable_models` as its source
(so no model id is hard-coded in the UI and the central policy stays the sole
authority) but renders single-select in `ball_leveler`.

The reference lines are labelled in the UI as **positions of the detected ball
centre**, not liquid-surface lines. With a centre reference point, an operator who
places the lines at the visible liquid surface introduces a constant
radius-sized offset. This is a UX correctness requirement, not a nicety.

Reference point stays the **bbox vertical centre**, as specified. For a roughly
spherical float it is the best image proxy for the physical centre and is
invariant to apparent bbox height; bbox bottom/top move with occlusion, glare,
clipping and detector tightness, and in a sight glass the lower arc is often the
first thing obscured by meniscus or housing.

### 4.6 Status

Extend the **existing** writer `health::write_status_file` with an additive
trailing `level` array — the same additive pattern `zone_inhibit_onsets` already
uses (`status_file.h:41-44`), so every existing call site and document stays
byte-identical. Fields: `camera_id`, `mode`, `state`, optional `level_percent`,
`model_id`, `ts_ms`, safe `reason` code. No second status writer. No credential
can travel this path.

**No Backend wire payload in this release.** The existing brazing contract is
zone-shaped (`{"zoneN": v}`); a level percentage is not a zone reading. Remote
reporting for Ball Leveler is a separate, separately-specified follow-up.

`--check` semantics need one explicit decision, recorded here: an invalid or
missing per-camera Ball calibration is **`Degraded` (exit 10)**, consistent with
how a camera-scoped model fault is already treated; only schema/query faults are
`Blocked` (exit 78). `evaluate_integrity` must scope its attachment scan to
`digit_reader` so preserved dormant rows do not degrade the appliance.

---

## 5. Scope boundaries

Explicitly **not** in this release: multiple tanks or multiple level measurements
per camera; automatic selection between `float-small` and `float-big`; any merging
of the two Float models' outputs; historical level-sample persistence; any Backend
wire change; any engine rebuild; any change to the protected service file.

The engine-only policy is scoped to the **Jetson production payload**, which
`tests/packaging/run.sh:880-883` already enforces. It does not forbid the Windows
ONNX Runtime backend (`engine_registry.h:30-34`), which is a development path.

---

## 6. Test obligations

All 35 required behavioural tests, 10 structural tests and 15 mutations from the
brief, written failing first, with deterministic clocks and synthetic frames. Plus
those the findings above force:

* switching to a configured `ball_leveler` **loads the Float engine** — the direct
  regression test for §3.1; it fails today.
* `--check` still exits 0 with a fully-configured fleet in either mode, with the
  other mode's configuration present and dormant.
* a v13 → v14 upgrade preserves every digit row, and a restart re-reads them.
* a view-significant camera edit moves a calibrated camera to `CalibrationInvalid`
  without deleting its calibration.

No live camera is used by any automated test. `192.168.1.81` is never contacted.

---

## 7. Mode-switch failure semantics

Binding rules. The commit of the mode transaction is the single point after which
the target mode is the truth, and nothing downstream may quietly contradict it.

1. **Transaction failure leaves everything as it was.** If the mode transaction
   fails or rolls back, the previous mode and the previous runtime remain intact.
   No configuration is altered, and the operator is told the switch did not
   happen.
2. **After commit, the target mode is final.** Both the UI mode and the persisted
   mode remain the target mode. Neither reverts on any later failure.
3. **A post-commit warm-up failure is a camera-level fault, not a mode-level
   one.** If the target mode's engines fail to warm after the commit, the affected
   cameras enter an explicit `ModelUnavailable` state and the appliance reports
   `Degraded` (`--check` exit 10). The mode itself does not change.
4. **No silent recovery.** A post-commit warm-up failure must never load an
   old-mode model, and must never switch the persisted mode back. Failing loud in
   the target mode is correct; quietly serving the previous mode is not.
5. **The operator can always switch back explicitly.** A degraded target mode must
   not trap the appliance: the mode control stays available so the operator can
   deliberately return to the previous mode.
6. **One committed mode, reported consistently.** UI, persistence and
   runtime-state reporting must never disagree about which mode is committed. The
   existing re-read-on-failure discipline (`mainwindow.cpp:356-375`) already
   encodes this and is retained: on a rollback or an unknown outcome, the mode is
   re-read from the database rather than assumed.

Each of these six rules gets a named test in slice 5.

## 8. Approval record

* **Decision 1 — approved 2026-07-30.** Non-destructive mode switching supersedes
  the `2026-07-21` switch-and-reset specification. Preserve Digital Reader
  attachments/classes/areas/configuration, Ball Leveler binding and calibration,
  and camera connections. The confirmation dialog no longer promises deletion or
  irreversibility. Proceed with the separate `ball_level_calibration` authority;
  **do not** add `mode` to `camera_model`.
* **Decision 2 — approved 2026-07-30.** A brief in-process re-warm pause on a
  committed switch is acceptable, subject to the properties listed in §3.1.
* Jetson warm-up wording corrected: the production path deserialises approved
  prebuilt `.engine` files and runs the normal warm-up inference. No runtime
  engine build; no `trt_cache` dependency.
* Failure semantics recorded in §7.
