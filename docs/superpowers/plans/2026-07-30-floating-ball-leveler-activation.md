# Floating Ball Leveler — Production Activation (implementation plan)

Spec: `docs/superpowers/specs/2026-07-30-floating-ball-leveler-activation-design.md`
Baseline: `b737a96`. Schema v13 → v14. Jetson `192.168.1.15` only, Unix Makefiles.

**The guard remains in place through slices 1–4. Slice 5 removes all guards
together only after slices 1–4 pass their gates.** The feature is fail-closed for
the whole build-out.

Operator decisions 1 and 2 are approved (spec §8). Mode-switch failure semantics
are binding and are specified in spec §7.

## Standing rules

* Tests are written **failing first**, then made to pass.
* Every slice: focused tests → full `ctest` → the slice's mutations → Codex review
  → **explicit** staging (`git add <path>` per file). Never `git add .` / `-A`.
  Never stage `models/`. Never touch the protected service file.
* Build/test on `.15` only:
  ```bash
  cmake -S . -B build -G "Unix Makefiles"
  cmake --build build -- -j4
  QT_QPA_PLATFORM=offscreen ctest --test-dir build -N
  QT_QPA_PLATFORM=offscreen ctest --test-dir build --output-on-failure
  ```
  Record: registered / passed / failed / skipped / duration / Linux symlink-test
  result / schema version.
* Codex `VERDICT: NO-GO` blocks the commit. A Codex timeout is infrastructure
  failure, not a PASS.
* No automated test touches a camera. `192.168.1.81` is never contacted.

---

## Slice 1 — Persistence + schema v14

**Files:** `src/core/db/db.cpp` (v14 block, `SCHEMA_VERSION` → 14),
`src/core/level/{calibration,repo}.{h,cpp}`, `src/core/mode/{reset,config}.{h,cpp}`,
`docs/` schema-version note.

1. `ball_level_calibration` exactly as specced — `camera_id PRIMARY KEY` is the
   one-per-camera guarantee.
2. `save_level_configuration(...)` — the single Ball write chokepoint: resolve the
   model through the manifest, call `models::model_compatibility(BallLeveler, …)`,
   enforce one model + one class, validate the calibration, write one row, all in
   one transaction, refuse the whole request on any failure.
3. `switch_and_reset` → non-destructive `switch_mode`; delete `preview_counts` and
   the `mainwindow.cpp:241-266` gate that blocks a switch when counts are
   unreadable.
4. `mode_setup_required`: drop the `ball_leveler → always true` hardcode.
5. `evaluate_integrity`: scope its `camera_model` scan to `digit_reader` so
   dormant rows cannot degrade `--check`.

**Tests:** 16, 17, 18, 19 from the brief; v13→v14 upgrade from a *populated* DB with
per-table row-count and content assertions; restart re-read; `--check` exits 0 with
both modes configured; wrong-mode / multi-model / multi-class / bad-calibration
writes all refused.
**Mutations killed:** 7 (non-camera key), 13 (erase digit config), 14 (erase Ball
calibration).
**Codex review:** persistence + migration.

## Slice 2 — Mapping, selection, runtime state

**Files:** `src/core/level/{mapping,select,state,runtime}.{h,cpp}`. Pure — no Qt
widgets, no OpenCV, no SQL.

Mapping returns `optional<double>`; never fabricates a number. Selection rejects
malformed first, then requires containment (half-open `[x, x+w)`), then highest
confidence, then the total tie-break `y_centre → x_centre → class_id`. The state
machine is the sole owner of transitions among the eight states, with an injected
clock. All defaults named constants, documented and asserted.

**Tests:** 1–12 from the brief, plus every state transition, plus "`Paused` does not
carry a percentage" and "`CalibrationInvalid` ≠ `Unconfigured`".
**Mutations killed:** 1 (reversed formula), 2 (bbox top not centre), 3 (no clamp),
4 (reversed lines accepted), 5 (detection outside rect accepted), 6 (lowest
confidence chosen).
**Codex review:** calibration + mapping.

## Slice 3 — Calibration UI

**Files:** `src/app/ui/camera/dialog/level_calibration_page.{h,cpp}`, a level canvas,
`models_page` single-select mode, `wizard_controller` + `wizard_stepper` step-4
branch.

Source → Configure → Model → Level calibration. Step 3 renders
`detection::selectable_models` only — `float-small` / `float-big` in
`ball_leveler`, never `digitv3`; no model id hard-coded in the UI. The canvas
enforces both lines inside the rect and 100% above 0%. Lines are labelled as
**ball-centre positions**, not liquid-surface lines. Save → return → edit again.

**Tests:** 13, 14, 15, 20; invalid-calibration rejection surfaced with a named
reason; a saved calibration reloads into the editor unchanged.
**Mutations killed:** 10 (`digitv3` allowed in Ball), 11 (Float allowed in Digit).
**Codex review:** UI.

## Slice 4 — Processor + frame annotation

**Files:** `src/app/camera/level_processor.{h,cpp}`,
`src/app/camera/level_overlay.{h,cpp}`, `camera_grid` subsystem branch.

`BallLevelProcessor` copies `DetectionProcessor`'s drop-oldest worker **and its
full ownership contract** (init-before-thread-start, stop+join in dtor, guarded
slot and snapshot, worker-side exception catch, `generation_`-tagged GUI
callbacks, no SQLite off the GUI thread). `CameraGrid::reload()` branches at the
subsystem level: Ball Leveler builds no `ZoneHealth`, no `ZoneReporter`, no
`BrazingReporter`, and reads no `camera_area`. Overlay is drawn on the display-only
Mat immediately before the final `mat_to_qimage()`.

**Tests:** 21–28, 33, 34, 35; annotation asserted on the final `cv::Mat`;
unconfigured / model-less camera constructs no processor and calls no
`engines_->get()` (via the existing `constructed_count()` probe).
**Mutations killed:** 8 (number shown while Acquiring), 9 (stale number after
Offline).
**Codex review:** processor + threading; annotation.

## Slice 5 — Mode activation, engine registry, guard removal

**Files:** `src/app/ui/mainwindow.cpp`, `src/app/ui/startup.cpp`,
`src/app/ui/warmup_state.{h,cpp}`, `camera_view.cpp`, `mode_confirm_text.cpp`.

1. **Replace the `EngineRegistry` instance on a committed switch** — the §3.1 fix,
   without which nothing above works at runtime. New registry + new `WarmupState`
   built from the *new* mode's allow-list and required set, re-injected into
   `CameraView`/`CameraGrid`, after teardown and after the transaction commits.
   Each registry stays immutable and mode-pure for its life.
2. Remove **all three** guards together: `mode_confirm_text.cpp:79`,
   `camera_view.cpp:232`, and `apply_camera_button_gate()` in
   `mainwindow.cpp:213-214`.
3. Rewrite the confirmation dialog: it must no longer promise deletion.

**Tests:** 29, 30, 31, 32; **switching to a configured `ball_leveler` actually loads
the Float engine** (fails today); no wrong-mode engine reaches warm-up or
inference; existing digit behaviour and the Zone overlay do not regress.
**Mutations killed:** 12 (old-mode processor survives), 15 (guard removed before
the runtime is complete).
**Codex review:** final complete-diff review, requiring `VERDICT: PASS`.

## Slice 6 — Package + installed-runtime acceptance

1. Build the `.deb` with all three engines; inspect it; confirm zero `.onnx`,
   `.pt`, `trt_cache`.
2. Run the generated preflight. Back up `/opt/denso/data`.
3. `sudo apt install --no-install-recommends ./<package>.deb` — **never `dpkg -i`**.
   Never install on `.81`.
4. `sudo denso-setup configure --user modela` → `seed-manifest` → `verify`.
5. Test the installed app; confirm mode switching and restart persistence.

## Live-camera acceptance (after slice 5 is green)

Authorised: `192.168.1.185-188` only. Prove: four connections preserved;
independent per-camera calibration; only configured cameras infer; boxes on the
correct camera; both reference lines correct; percentage moves in the correct
direction; no cross-camera value bleed; one unavailable camera does not stop its
siblings; switching back restores Digital Reader. Credentials may be read from the
isolated test DB but are never printed, logged, committed or included in evidence.

## Sequencing note

Slices 1 and 2 are independent of each other and of the UI. Slice 4 depends on 2;
slice 5 depends on 3 and 4. Slice 5 is the only one that changes operator-visible
availability.
