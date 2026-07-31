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

**Not affected — corrected 2026-07-30.** An earlier revision of this table listed
`try_attached_model_filenames` (`core/detection/repo.cpp:89-117`) as letting
wrong-mode rows into the warm-up required set. **That was wrong.** Its SQL is
mode-blind, but line 112 keeps a row only when
`models::model_compatibility(mode, md).allowed()` succeeds, so the set
`startup.cpp:202-203` receives is already mode-filtered. This call site is safe
and needs no change; no reasoning in this document depends on the retracted
claim.
| `evaluate_integrity` | `core/health/integrity.cpp:197-199` | dormant rows judged against the active mode → `--check` exits **10** |
| `load_old_attachment` | `core/detection/migrate.cpp:33-35` | migration matches by `(camera_id, filename)`, no mode |

**Decision — do NOT add `mode` to `camera_model`.** Codex's argument holds and the
table above is the evidence: scoping that column correctly means touching six
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

---

## Lean V1 amendment — operator approved

Approved 2026-07-30, AFTER Slice 1 was implemented. This amendment **supersedes
the process and v1 scope above wherever they conflict**. The historical findings,
decisions and rationale above are deliberately left unrewritten — they record why
the design is what it is, and several of them are still load-bearing.

Goal: deliver a working operator flow sooner, without dropping any
correctness-critical protection.

### Delivery shape

The remaining five implementation slices are replaced by three phases:

| Phase | Contents | Gate |
|---|---|---|
| **A** — Measurement core + calibration UI | former Slices 2 + 3: calibration rectangle, 0%/100% ball-centre lines, percentage calculation clamped to 0-100, rejection of reversed/degenerate/non-finite calibration, one compatible Float model, highest-confidence valid detection inside the rectangle, save/reload/edit, Digital Reader configuration preserved | focused tests |
| **B** — Processor + overlay + mode activation | former Slices 4 + 5: `BallLevelProcessor`, Ball-specific `CameraGrid` branch, OpenCV overlay on the final display Mat (rectangle, reference lines, ball box + centre, percentage), mode activation, correct Float engine loading, **guard removal only once the complete path is green** | Codex processor/threading review |
| **C** — Package + live acceptance | final complete-diff Codex review, `.deb` build + inspection, installed-runtime verification on `.15`, authorised live-camera acceptance, mode-switch and restart-persistence acceptance | final Codex review |

### Runtime state — minimum useful model

```text
Unconfigured | Acquiring | Healthy | Unavailable | CalibrationInvalid
```

`Unavailable` may carry a safe reason code (`camera_offline`, `paused`,
`model_unavailable`, `inhibited`). **An old value must never be displayed as a
current live measurement.**

### EngineRegistry — decision deferred, not designed away

Before Phase B, make a SHORT source-based comparison of (1) replacing
`EngineRegistry` + `WarmupState` in-process versus (2) safely re-executing the
same application binary after the committed mode switch (a self-reexec, which is
NOT the same as exiting and relying on a supervisor — there is no supervisor).
Choose on actual lifecycle, unsaved UI state, lock-file behaviour and failure
recovery. Keep it concise. Mandatory either way: no union allow-list; no
wrong-mode engine reaches warm-up or inference; committed mode and displayed mode
agree; failure is explicit; the operator can switch back.

### Review + test cadence

- Codex gates for the rest of the project: architecture (done), processor/threading
  (end of Phase B), final complete-feature (before package/live acceptance). No
  per-slice or per-UI-edit reviews.
- During development: focused tests only. One full CTest before Codex. After
  Codex, rerun the full suite only if PRODUCTION code changed —
  documentation-only corrections do not require another full run.

### Requirement disposition

**Retained (non-negotiable):** query failure distinct from missing data;
transactional database changes; Ball configuration always uses BallLeveler
compatibility; `digitv3` cannot enter Ball configuration; Float models cannot
enter Digital Reader; invalid class ids rejected; mode switching preserves both
modes' data; invalid calibration rejected; percentage clamped to 0-100;
production guards remain until the complete Ball runtime works; one full suite
before commit; package + installed-runtime testing at the end.

**Simplified:** detection tie-break — a simple stable deterministic rule replaces
the exhaustive ordering hierarchy; runtime state machine — five states replace
eight; slice structure — three phases replace five slices.

**Deferred to a follow-up project** (unless live acceptance proves them
necessary): `HoldingLastValid` behaviour and its hold timer; the status-file
`level` array; Backend level reporting; historical level persistence; the
exhaustive eight-state runtime machine; exhaustive deterministic detection
ordering; multiple independent tanks per camera; automatic selection or merging
of Float models.

**Waived:** mutations 6-12 as executable apply/build/restore cycles. The same
risks are instead pinned by named automated tests (one Ball configuration per
camera; invalid writes leave no partial row; Digital Reader data survives
switching; Ball calibration survives switching; mode and reporting settings roll
back atomically; inactive-mode data does not degrade the active mode; all
production guards present). Mutations 1-5 were executed and killed and remain
valid evidence. *(Historical note: 6-12 were in fact also executed and killed
before the waiver arrived, so the evidence exists — the waiver removes the
obligation, not the result.)*

### Not implemented

Nothing in Phases A-C is implemented yet. Slice 1 delivered PERSISTENCE ONLY:
schema v14 `ball_level_calibration`, the write chokepoint, mode-scoped integrity,
and the non-destructive switch. The Ball Leveler operator surface remains
guarded.

---

## 9. EngineRegistry lifecycle — the Phase-B comparison and decision

The Lean V1 amendment required a SHORT source-based comparison before Phase B of
(A) replacing `EngineRegistry` + `WarmupState` in-process after a committed
switch, versus (B) safely re-executing the same application binary. This is that
comparison. It is deliberately confined to what the source and the device say.

### Evidence

| Dimension | Source | A — in-process replace | B — self-reexec |
|---|---|---|---|
| Single-instance lock | `core/instance/single_instance.{h,cpp}`, `app/main.cpp:151-168` | untouched | **disqualifying — measured, not reasoned** (below) |
| Supervisor | no systemd unit exists on `.15`; `/usr/share/applications/com.denso.DigitalReader.desktop`; `packaging/denso-setup:107-118` (XDG autostart fires only after a graphical *login*) | n/a | nothing restarts a dark appliance |
| Startup ownership | `ui/startup.cpp:193-215` builds the allow-list + required set + one registry; `launch_warm_ui_first:99-127` holds `WarmupState` and `MainWindow` as **stack locals** | needs an owner for the replacement — a real but bounded refactor | free (boot re-runs verbatim) |
| Mode-switch flow | `ui/mainwindow.cpp:257-397`: teardown → transaction → adopt mode → reload, all synchronous on the GUI thread | slots in directly after `TransactionCommitted`, before `reload()` | would have to replace steps 4-6 with an exec |
| Teardown | `CameraView::teardown_for_switch()` → `CameraGrid::teardown()` → `clear()` joins capture **and** inference threads; `~WarmupState` joins the warm-up thread; `~SettingsDialog` `wait()`s the network workers | every destructor runs, in order | `execv` runs **no** destructors and kills every other thread instantly |
| Unsaved UI state | `settings_->setModal(true)`; a switch is raised from the open Settings modal | preserved | destroyed |
| Post-commit failure | §7.3-7.5; boot wires `WarmupState::failed` → `app.exit(1)` (`startup.cpp:115-123`) | the NEW session's `failed` must route to a camera-level fault instead — an explicit, testable difference | a re-exec'd boot hits the boot handler → `app.exit(1)` → dark appliance, violating §7.3 and §7.5 |
| Mode purity | `detection/engine_registry.cpp:42-54` throws outside the allow-list; "immutable after construction" | preserved — we swap the object, never the list | preserved trivially |

### The measured disqualifier for B

`QLockFile` reclaims a lock only when the recorded pid is gone
(`single_instance.cpp:12-16`). `execv` **keeps the pid** and runs no destructor,
so the lock file survives holding the re-exec'd process's own live pid. Measured
on `.15` with a minimal Qt6 program that locks, `execv`s itself, and re-locks:

```text
PARENT: pid=12510 tryLock=OK   error=0
CHILD : pid=12510 tryLock=FAIL error=1      # 1 == QLockFile::LockFailedError
```

Mapped onto `main.cpp:152-159`, the re-exec'd appliance prints "another instance
is already running", shows a message box and returns 3 — and with no supervisor
it stays dark. Rescuing B means deliberately releasing the single-instance guard
before the exec, i.e. opening a window in which two processes may share one
SQLite file, one camera set and one rotating log — the exact hazard
`single_instance.h` was written to close. It also would not fix the destructor
problem: capture threads holding GStreamer/NVDEC pipelines, the inference worker,
the warm-up thread, in-flight `QProcess` network workers, the WAL connection and
the log sink's fd are all abandoned mid-flight, and non-`CLOEXEC` descriptors leak
into the new image.

### Decision — (A) replace `EngineRegistry` + `WarmupState` in-process

B is higher risk on every dimension that matters and its only advantage — mode
purity by construction — is one A already keeps: each registry instance stays
immutable and mode-pure for its whole life, because the *object* is swapped and
the *list* never is.

Implementation obligations that follow, all of them testable:

1. The allow-list + required-set construction gets ONE definition, shared by boot
   and by the switch, so the two can never build different sets for the same
   mode. No union allow-list, ever.
2. The replacement happens **after** `TransactionCommitted` and **after** the
   existing teardown (which has already joined every capture and inference
   thread), and before `reload()`.
3. The new session's `WarmupState::failed` is a **camera-level** fault
   (§7.3) — it must NOT reach `app.exit(1)`, which is boot-only semantics.
4. The mode the registry is built from is the committed mode re-read after the
   transaction, so persisted mode and running mode cannot disagree.
5. The operator can always switch back (§7.5): a degraded destination mode
   leaves the mode control live.

---

## 10. Amendment — Multi-zone Ball Leveler (operator-approved 2026-07-31)

Supersedes the one-tank-per-camera assumption throughout §1–§9. Ball Leveler
becomes another PRODUCER of per-zone numeric values on the Digital Number zone
infrastructure; only the zone-processing logic differs.

    Digit zone processor: detections -> digit assembly     -> numeric zone value
    Ball  zone processor: detections -> ball selection
                                     -> percentage mapping -> numeric zone value

### 10.1 Model ownership

One camera binds exactly ONE Float model, shared by every Ball zone on that
camera. There is no per-zone model selection and no per-zone inference. Per
camera frame: one model, one inference execution, one detection set, evaluated
independently for each configured zone.

### 10.2 Zone numbering — the EXISTING machine-wide authority, unchanged

Zone numbers stay machine-wide unique across all cameras AND both modes, over
the range `camera_area.zone` already supports. The reason is unchanged and
structural: `build_brazing_payload` keys by zone number alone and carries no
camera identity, so two claimants of one number are two writers of one backend
field. Ball reuses `camera::find_zone_conflict` and the picker; the ONE
ownership query (`zones_owned_by_other_cameras`) is widened to see BOTH the
digit `camera_area` rows and the Ball zone rows, so it remains the single
authority rather than gaining a Ball-specific twin.

A Ball camera owns 1..4 zones. That cap is BALL-SPECIFIC and lives in the Ball
validation path — it is deliberately NOT pushed into `camera::area_validation`,
which the digit reader shares and which has no such limit.

Storage identity is `(camera_id, zone_no)`. Numbering does NOT restart per
camera: e.g. Camera 1 -> 1,2,3,4; Camera 2 -> 5,6; Camera 3 -> 7,8,9,10.

### 10.3 Percentage representation

Ball arithmetic stays `double` end to end. The overlay may show one decimal
(`LEVEL 24.5%`). At the `ZoneReading` seam the value is clamped to [0,100] and
quantized to the NEAREST INTEGER, because the whole delivery stack — `ZoneReading::value`,
the aggregator snapshot, `build_brazing_payload` — is integer-valued and the
backend contract is `{"zoneN": <int>}`. Widening that stack to `double` is a
backend-contract change, not a reuse adaptation, and is out of scope for this
release. Operator-approved: the backend zone fields carry the Ball percentage as
an integer 0..100.

### 10.4 Aggregator policy — parameterized, not duplicated

Ball reuses `ZoneSink -> ZoneReporter -> ZoneAggregator -> BrazingReporter`
whole. `ZoneAggregator`'s hold timeout becomes a constructor parameter exactly
as `stable_frames` already is. Ball Leveler Lean V1 constructs it with:

  * `hold_timeout_ms = 0` — an incomplete or unavailable zone never keeps an old
    percentage alive. This is what preserves §10.6's no-stale invariant through
    an aggregator that was written to hold digit readings for 30 s.
  * `stable_frames  = 1`  — a continuous quantized measurement will not reliably
    produce five consecutive identical integers.

Both are PARAMETERS of the one aggregator. A second aggregator, a second
reporter, a second retry policy or a second payload builder is forbidden.

NO median filter, deadband or smoothing subsystem ships in the first cut.
Integer quantization plus `stable_frames = 1` is the initial behaviour; noise
filtering is considered only if live-camera acceptance proves it necessary.

### 10.5 Persistence — v15

`ball_level_calibration` (v14) held one binding AND one geometry in one
`camera_id PRIMARY KEY` row. v15 splits the two so the camera-level model is not
duplicated per zone:

  * `ball_level_binding(camera_id PK, model_id, class_id)`
  * `ball_level_zone(camera_id, zone_no, rect_*, y_100, y_0, conf, hold_ms,
     view_revision, PRIMARY KEY(camera_id, zone_no))`

The v14 row migrates forward to a binding plus **Zone 1** — or, where zone 1 is
already claimed, the lowest free zone number, since numbering is machine-wide.
v14 data is NOT discarded: the old table is left in place untouched, additively,
per the migration idiom already in `db.cpp`.

`save_level_configuration` remains the ONE Ball write chokepoint. It now takes
the binding plus the COMPLETE 1..4 zone set, validates every zone before writing
anything, and commits once — one invalid zone rolls the whole camera save back.
There is deliberately no per-zone save entry point.

### 10.6 What is folded, what stays separate

Folded into the Digital Number zone infrastructure (the duplicate subsystems the
one-zone design introduced):

  * `level::LevelRuntimeEntry` / `LevelState` -> the shared zone runtime
    projection. `level/runtime.h`'s "there is no zone concept in this mode"
    premise is now obsolete.
  * `LevelStateProcessor` -> the shared tile/zone status path.
  * `level_overlay` orchestration -> the existing zone annotation composition
    boundary; only the Ball GEOMETRY drawing survives.
  * `CameraGrid::reload_ball()`'s bypass of the zone/reporting subsystem -> it
    now builds the SAME `ZoneReporter` / `BrazingReporter` the digit path builds.

Stays Ball-specific: `BallLevelProcessor`, `level::select_ball` /
`level::level_percent`, `LevelCalibration` validation and editing, and
`LevelCanvas`.

### 10.7 Invariants this amendment must not break

  * A zone with no detection must not erase a healthy sibling zone.
  * A camera failure may make all ITS zones unavailable and must not stop other
    cameras.
  * No old percentage is ever annotated or reported as a current live value.
  * Digital Number area editing, annotation and backend reporting do not change.
  * Switching modes preserves BOTH modes' saved zone configurations.
