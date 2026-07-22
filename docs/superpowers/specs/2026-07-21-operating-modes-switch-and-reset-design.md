# Operating Modes — Digital Number Reader / Floating Ball Leveler, and `Switch and Reset Target Mode`

Status: **APPROVED — Revision 3b.**
Date: 2026-07-21 (Revision 3b editorial erratum: 2026-07-22)
Revision: 3b (decisions A1–A4 remain locked by the product owner; R2 remains a
mandatory prerequisite; 3b is an editorial erratum to §11-R6 only — no change to
scope, decisions, transaction behaviour or acceptance criteria). See the revision
history at the end of this document.
Depends on the shipped readiness/inhibition work
(`2026-07-20-zone-readiness-inhibition-design.md`, schema v13).

---

## 1. Scope

The appliance today does exactly one job: read 4-digit 7-segment displays. This
introduces a second job — **Floating Ball Leveler** — and makes exactly one of
them active at a time, selected by an explicit, destructive operator action.

**In scope:** the mode concept and its persistence; the boundary between shared
camera infrastructure and mode-owned processing; the `Switch and Reset Target
Mode` transaction; the target-mode first-run state; `status.json`; acceptance
criteria; a mandatory prerequisite fix (§10).

**Explicitly out of scope:**

- **The Floating Ball image-processing algorithm.**
- **The Leveler's configuration schema, wizard and reporting** — §2.1 ships it
  as an explicitly unavailable destination rather than half-defining it.
- **Running both modes at once.** See §3.4.
- **Migrating configuration between modes.** A switch resets processing setup;
  it never translates one mode's ROIs or bindings into the other's.
- Any change to the `0/10/78` readiness contract, `sync_models()`'s directory
  scan, or the non-blocking `EnginesUnmanifested` degradation.

### 1.1 What this design does NOT claim

- **It does not claim no POST reaches the server after a switch.** A request
  already handed to `QNetworkAccessManager` cannot be recalled. The guarantee is
  narrower and stated precisely in §6.6.
- It does not claim the switch is fast or non-blocking. §11-R1 is an *analysis*
  of the worst case, not a measurement.
- It does not claim mode isolation is enforced by the database. Foreign keys are
  **off** (§3.3); isolation is one explicit transaction whose correctness is a
  test obligation, not a schema guarantee.
- It does not claim the Leveler works. Nothing here makes the appliance able to
  level anything (§2.1).

---

## 2. The two modes

| | **Digital Number Reader** (`digit_reader`) | **Floating Ball Leveler** (`ball_leveler`) |
|---|---|---|
| Question answered | "what number is displayed?" | "where is the ball?" — algorithm out of scope |
| Per-camera processing config | 1..N detection models + per-class confidence; ROI polygons with a machine-wide `zone` | **undefined in this release** (§2.1) |
| Processing | `DetectionProcessor` (`src/app/camera/frame_processor.h`) | a future sibling `FrameProcessor` |
| Reporting | `ZoneReporter` → `BrazingReporter` → POST `{base_url}/api/brazing/update` | out of scope |

`digit_reader` is the **default and the migration target for every existing
installation** (§8). No existing appliance may change behaviour when this ships.

### 2.1 What `ball_leveler` is in this release

Selecting `ball_leveler` **persists the mode, performs the full reset, retains
every camera connection, and lands on an explicit "Floating Ball Leveler setup is
not available in this release" state.** In that mode this release:

- **retains** all shared camera rows and shows their sources (§7.2);
- creates no ROI/model bindings and offers no processing wizard;
- constructs **no** streams, **no** processor and **no** reporter;
- reports `mode_setup_required: true` permanently;
- keeps the switch back to `digit_reader` available and equally destructive.

Inventing a Leveler persistence model before the algorithm spec exists would
bake in guesses the algorithm would then have to live with. The mode machinery is
what is being built and tested here.

---

## 3. Boundaries

### 3.1 Shared camera / infrastructure boundary — never reset

Properties of *the appliance*, not of the job it is doing:

| Area | Where |
|---|---|
| Capture ladder, RTSP/USB pipelines, reconnect, backpressure | `src/app/camera/camera_stream.*`, `gst_pipeline.*`, `stream_pacing.*` |
| Frame conversion, orientation, snapshot | `src/app/camera/frame_convert.h`, `snapshot.*` |
| Grid layout, tiles, FPS meter | `src/app/ui/camera/grid/*`, `camera/fps_meter.*` |
| Display/theme settings | `settings` keys `width`,`height`,`dark`,`display_mode` |
| Network config | `net_config`; reasserted at boot (`main.cpp:264-275`) |
| Server address | `settings` key `brazing.base_url` (§6.5) |
| Model catalog | `model` + `sync_models()` — describes *files on disk* |
| Engine registry / warm-up | `src/app/detection/engine_registry.*` — cached per filename, never unloaded |
| Logging, paths, single-instance lock | `src/app/logging/*`, `src/core/paths/*` |

### 3.2 The `camera` row is split, not deleted

**Locked decision A1.** A camera's *connection and capture configuration* is
shared infrastructure: the operator must never re-enter an RTSP URL, credential,
USB index, resolution, FPS or orientation because the appliance changed job.
Every `camera` row and **its stable `id` survive a switch.**

The row's 20 columns divide cleanly. Verified against
`src/core/camera/camera.h:15-66` and the `COLUMNS` list at
`src/core/camera/repo.cpp:17-20`:

| Category | Columns | On switch |
|---|---|---|
| **Identity / connection / capture (shared)** | `id`, `name`, `camera_type`, `cam_index`, `ip`, `rtsp`, `username`, `password`, `channel`, `stream`, `manufacturer`, `width`, `height`, `fps`, `pitch`, `roll`, `rotation`, `active` | **preserved, untouched** |
| **Mode-processing state** | `setup_complete`, `areas_need_review` | **reset to 0** |

`active` is an operator *preference* ("I want this camera on"), not processing
setup, so it is preserved. Safety does not depend on it — see §6.4.

`channel`, `stream` and `manufacturer` are preserved because they are inputs to
RTSP URL construction (`camera/rtsp_templates.*`), i.e. connection data.

No column outside those two describes processing setup. This was checked
column-by-column, not assumed.

### 3.3 Mode-specific processing workspace — destroyed on switch

| Table | Added | Why mode-specific |
|---|---|---|
| `camera_area` | v7 (+v10 `zone`) | ROI semantics and `zone` claims are mode-defined |
| `camera_model` | v8 | model attachment is a digit-reader concept |
| `camera_model_class` | v8 | per-class confidence, ditto |
| `reading` | v9 | readings are in the *old* mode's units and would silently mix |
| `model_migration_receipt` | v13 | correctness requirement — §6.5 |

**Foreign keys are inert.** `PRAGMA foreign_keys` is never enabled anywhere, so
every `REFERENCES` clause is documentation. `camera::remove`
(`src/core/camera/repo.cpp:115-149`) deletes only `camera_area` + `camera` and
already **orphans** `camera_model`, `camera_model_class` and `reading`. The reset
deletes explicitly, child-first (§6.3), and is not built on `camera::remove` —
which is now doubly true, since the reset must not delete cameras at all.

### 3.4 Decision: one live workspace, not two namespaced ones

Rejected: a `mode` column on `camera_area`/`camera_model`, keeping both modes'
processing config side by side.

It is **possible** — inactive rows could be excluded from zone validation and
payload construction, leaving the external `zone<N>` contract unchanged. It is
rejected on **cost and blast radius**:

- every area/detection/runtime query gains a mode scope, including the
  zone-uniqueness check `WHERE zone = ? AND camera_id != ?`
  (`repo.cpp:262-272`) and `zones_owned_by_other_cameras` (`:301-317`), which are
  machine-wide today;
- a missed scope is a silent cross-mode leak the compiler cannot catch;
- the Leveler's configuration model does not exist yet (§2.1).

Note this trade-off is now *smaller* than in Revision 2: with camera connections
preserved (§3.2), what a switch actually costs the operator is ROI and model
setup, not the physical inventory.

### 3.5 The processing seam

`FrameProcessor` is already the per-camera seam, with `OrientationProcessor` and
`DetectionProcessor` as siblings selected in `CameraGrid::start_one`
(`camera_grid.cpp:223`). A Leveler processor becomes a third sibling. **No new
abstraction is introduced.** Which processor a camera gets must be a pure
function of `mode + camera config`, unit-testable without a GPU. A
`denso_leveler` static lib under `src/app/leveler/` follows the existing
convention when the algorithm lands.

---

## 4. Persistence

### 4.1 The mode key — no migration, no schema change

Modelled on `BrazingConfig` (`src/core/brazing/config.cpp:34-49`): a new
`src/core/mode/config.{h,cpp}` over the existing `settings` key/value table
(created in v1). Adding a key needs no migration.

| Key | Values | Absent ⇒ |
|---|---|---|
| `mode.target` | `digit_reader` \| `ball_leveler` | `digit_reader` |

`parse_target_mode` follows the `parse_display_mode` contract
(`src/core/settings/display.h:18`): **any unknown or corrupt token resolves to
`digit_reader`** — never to the newer mode.

**Deliberately NOT a field on `settings::Settings`.** `MainWindow::on_reset_defaults`
assigns a default-constructed `Settings` wholesale and calls `settings::save`
(`mainwindow.cpp:299-317`), so any field wired into that aggregate is reset by
"Reset to defaults" — silently changing the appliance's job. `settings::save`
does not delete unknown keys, so an independent key is untouched by that path.

### 4.2 Schema: unchanged at v13

No migration. A committed switch retains every camera row with its id, so there
is nothing whose ownership needs recording per-row; the mode key answers it.
(Revision 1 proposed `camera.owner_mode`; dropped as speculative — §13.)

---

## 5. Prerequisite: stale-generation callback isolation (locked)

**Mandatory, landed and reviewed BEFORE any mode work.** Not an open risk.

`WorkerFailedFn` marshals with `post_to_gui(this, ...)` where `this` is the
**grid**, not the processor (`camera_grid.cpp:257-263`), and the lambda checks
only `if (!health_)`. `clear()` destroys the old `health_`, but `reload()` builds
a new one on the same grid, so an escalation queued by an *old* inference worker
can inhibit a camera in the **new** generation.

**A1 makes this worse, which is why it is now a gate.** With camera rows and ids
preserved across a switch, a stale event carrying `camera_id 3` no longer refers
to a possibly-absent row — it matches a *real, retained* camera 3 in the new
generation, and inhibits it. Before A1 the id might simply have not existed.

The prerequisite slice must:

- introduce a generation token, or make the callback context processor-owned so
  Qt drops it (the mechanism already used for the snapshot path, where the
  context is the `BrazingReporter`, not the grid — `camera_grid.cpp:133-138`);
- prove stale callbacks are ignored, with a regression test that delivers an
  old-generation failure **after** a rebuild and asserts no inhibit in the new
  generation;
- change **no** operating-mode behaviour, and be separately reviewable.

The same shape applies to the queued `status_changed` lambda (`:285-291`); it is
currently safe only because the stream is deleted and Qt drops its events. The
prerequisite should confirm that, not assume it.

---

## 6. `Switch and Reset Target Mode`

### 6.1 Ownership and preconditions

The operation lives in **core** (`src/core/mode/`), taking the existing
`QSqlDatabase`, so `MainWindow` stays a thin orchestrator per the hard rule. It
returns a typed result (success, or failure carrying the SQL error verbatim) and
must not pop UI itself.

Preconditions: GUI thread, from a modal handler — the same context as the
existing full rebuild; operator-confirmed (§7.1); refused while a display
confirm/revert transaction is live (`display_txn_active_`,
`mainwindow.cpp:244-254`).

**Busy state (locked, R1):** the action must show a busy state and be disabled
for the duration of the synchronous teardown, so repeated clicks cannot queue a
second switch behind a multi-second join.

**Concurrency assumption:** the switch runs from a modal handler with no other
writer active, so the counts shown in §7.1 cannot go stale between counting and
committing. This must be stated in code, since it is what makes
count-then-delete safe without re-reading inside the transaction.

### 6.2 Teardown order

The order is the one `CameraGrid::clear()` already implements
(`camera_grid.cpp:57-95`), and the switch **must call that authoritative
primitive** — `CameraGrid::clear()` itself, or one extracted teardown-only entry
point. **Re-implementing the sequence is forbidden**: two lifecycle
implementations would drift, and this one encodes lifetime constraints that are
not locally obvious.

**`CameraView::reload()` must NOT be used as the pre-transaction teardown.** It
calls `grid_->reload()`, whose first action is `clear()` but which then
immediately re-queries `camera::runtime()` — still returning the *old* mode's
completed cameras, because the transaction has not run yet — and restarts every
one of them (`camera_view.cpp:79-86`, `camera_grid.cpp:97-108`). The sequence is
therefore strictly: **teardown-only before the transaction; `reload()` only
after commit or rollback.**

1. `pending_`/`pending_cams_` cleared — they hold raw `CameraTile*`.
2. Per stream `stop()` (joins the **capture** thread), then `delete` (destroys
   `DetectionProcessor`, joining the **inference** thread). This join is what
   guarantees nothing can still reach the `ZoneSink`.
3. Tiles deleted; `tiles_by_cam_` cleared.
4. `reporter_.reset()` — legal only after (2).
5. `brazing_reporter_.reset()` — after the `ZoneReporter`, whose callback targets
   it. Destroys the parented retry `QTimer` and the `QNetworkAccessManager`.
6. `health_.reset()` — last; its callback dereferences `reporter_`.
7. `last_applied_seq_`, `verdict_` and layout state reset.

**`release_streams()` is NOT sufficient.** It joins only capture threads
(`camera_grid.cpp:334-338`), leaving a frame already in `pending_` to be
processed, so `on_zones` → `post_to_gui` can still run afterwards.

**Warm-up must be left running.** `warm_up()` has no cancellation;
`~WarmupState` calls `quit()` + `wait()`, but `quit()` cannot interrupt a
blocking call, so destroying it would block the GUI thread for the remainder of
a TensorRT build. Late `on_model_ready`/`on_warmup_finished` are harmless no-ops
once `pending_cams_` is empty.

### 6.3 The reset transaction

One transaction; child-first deletes; camera rows **updated, never deleted**;
mode key and reporting-disable written **inside** it:

```
BEGIN
  DELETE FROM camera_model_class          -- unconditional; see note below
  DELETE FROM camera_model
  DELETE FROM camera_area
  DELETE FROM reading
  DELETE FROM model_migration_receipt
  UPDATE camera SET setup_complete = 0, areas_need_review = 0      -- all rows
  upsert settings: 'mode.target'     = <new mode>
  upsert settings: 'brazing.enabled' = '0'
COMMIT   -- else ROLLBACK
```

Splitting any of the three writes out of the transaction is the mistake that
must not be made. A crash between the deletes and the key write leaves the
appliance claiming mode A with mode A's processing setup destroyed —
indistinguishable, from the operator's side, from a successful switch that then
lost their work. The same argument applies to `brazing.enabled`: reporting must
not remain armed if the reset committed.

Statements are unfiltered because §3.4 guarantees one live workspace; filtering
would imply a coexistence contract this design does not offer.

**`camera_model_class` is deleted unconditionally, NOT scoped by
`WHERE camera_model_id IN (SELECT id FROM camera_model)`.** The scoped form
would skip rows already orphaned — and this codebase *creates* such orphans:
`camera::remove` deletes only `camera_area` + `camera`, leaving
`camera_model_class` behind (§3.3). Any appliance where a camera was ever
deleted may hold them. The scoped delete would therefore leave rows behind while
§12/§13 assert the table is empty, and the unconditional form additionally
repairs pre-existing damage at no cost.

The settings upserts must be issued on this connection inside this transaction —
**not** via `settings::save` or `brazing::save`. `settings::save` runs its own
transaction (`settings/repo.cpp:76-102`), which must not be nested here;
`brazing::save` performs two unchecked `set()` calls and returns no status
(`brazing/config.cpp:45-49`), so a failure would be invisible to the reset's
typed result. `brazing.enabled` is stored as the string `"1"`/`"0"`
(`brazing/config.cpp:34-49`); write `"0"`, matching the existing representation
rather than inventing one.

**Every statement's result and the commit must be checked**; the transactional
guarantee in §6.7 depends on it, and it is exactly what `brazing::save` fails to
do today.

### 6.4 `camera::runtime()` is guaranteed empty — by construction

`runtime()` filters `WHERE active = 1 AND setup_complete = 1`
(`src/core/camera/repo.cpp:173-189`) and is *the* single decision of what may
stream. Setting `setup_complete = 0` on **every** row therefore makes
`runtime()` empty regardless of `active`, so:

- no retained camera can be started by `CameraGrid::reload()`, which iterates
  `camera::runtime()` (`camera_grid.cpp:102`);
- preserving `active` is safe: it is ANDed with `setup_complete`, so it cannot
  independently re-admit a camera;
- the guarantee lives in SQL, not in UI logic — nothing may re-derive it with a
  local `if (active)`, per the repo contract at `repo.h:37-41`.

`areas_need_review = 0` is reset too. Leaving it at 1 would be *safe* but
misleading: it means "your ROIs need re-verification", and after a switch there
are no ROIs to re-verify — the correct state is "processing setup required",
which `setup_complete = 0` expresses.

### 6.5 Preserved vs destroyed

**Preserved:**

- **Every `camera` row and its `id`** — all connection and capture columns
  (§3.2). This is decision A1.
- `settings`: `width`, `height`, `dark`, `display_mode`.
- `settings`: **`brazing.base_url`** — the server address is infrastructure; the
  operator should not re-type it.
- `net_config` — losing IP config could strand a headless box.
- `model` catalog — describes files on disk.
- Everything on disk: engines, sidecars, `trt_cache`, logs, the DB file.

**Destroyed / reset:**

- every row of `camera_area`, `camera_model`, `camera_model_class`, `reading`,
  `model_migration_receipt`;
- `camera.setup_complete` and `camera.areas_need_review` → `0`;
- **`brazing.enabled` → false** (decision A2).

**Why `brazing.enabled` is disabled (A2, changed from Revision 2).** Reporting
must stop when the operator switches jobs, and must **not** silently resume when
the target mode is later configured — re-enabling is an explicit operator
action. Revision 2 argued preservation was safe because no zones would exist;
that reasoning was correct but insufficient, because it made re-arming
*implicit* — the first zone created in the new mode would have started POSTing
with no one deciding it should. It also protects a future Leveler that reuses
the reporting infrastructure.

**Why `model_migration_receipt` must go (A4).** The receipt is the rollback
contract for a model-generation swap, and its `attachments` JSON stores
`camera_model_id` values, used as `UPDATE camera_model SET model_id=? WHERE id=?`
(`src/core/detection/migrate.cpp:96-97, :119`). Those rows are deleted by this
reset, and there is **no `AUTOINCREMENT` anywhere in the schema**, so SQLite
reuses rowids from 1: a post-switch `rollback-model` would not merely no-op, it
could **repoint an unrelated future attachment**. Model files, sidecars and the
manifest on disk are untouched, so model history is not lost — only the
automated repoint of attachments that no longer exist.

Note this hazard is specific to `camera_model.id`. **`camera.id` is now stable
across a switch** (A1), so nothing that references a camera id can be
misdirected by reuse — a genuine safety improvement from preserving cameras.
See §11-R7 for what remains.

### 6.6 The reporting guarantee, precisely

**Claim:** once the confirmed switch enters the synchronous teardown handler, no
inference result and no retry tick can *initiate* a new transport request.

**Mechanism:** the GUI thread is blocked in teardown throughout, so no queued
event is dispatched; inference callbacks are posted with the `BrazingReporter`
as their Qt context (`camera_grid.cpp:133-138`), and that context is destroyed
before the event loop resumes, so Qt discards them. The retry `QTimer` is
parented to the reporter and dies with it. In-flight replies are destroyed with
the `QNetworkAccessManager`, so `finished` never fires and `done` is never
called.

**Not claimed:** that a request already handed to the OS socket does not reach
the server. It may.

**Also required (R4):** a pending-but-undelivered `BrazingRetryPolicy` snapshot
is discarded on reset. Correct — the old mode's readings must not be posted after
a switch — but currently silent, so it must be **logged: the zone count and zone
numbers only, never payload values.**

After the transaction, `brazing.enabled = false` means the rebuilt grid
constructs no `ZoneReporter` or `BrazingReporter` at all
(`camera_grid.cpp:128-148` gates on `bcfg.enabled && !base_url.empty()`), so
reporting is off both in memory and in configuration.

### 6.7 Failure and rollback

| Failure | Behaviour |
|---|---|
| Any `DELETE`/`UPDATE`/upsert fails | `ROLLBACK`. Mode key, `brazing.enabled`, **and every camera's `setup_complete`/`areas_need_review`** revert together. Rebuild the **old** mode's pipeline. Report the SQL error verbatim. |
| `COMMIT` fails | Explicit `ROLLBACK` — SQLite can leave the tx open on a busy commit; `camera/repo.cpp:142-147` already handles this shape. Same recovery. |
| Process dies mid-transaction | SQLite rolls back on next open. Mode key, reporting flag and camera processing flags stay mutually consistent — what §6.3 buys. |
| Rebuild after a **successful** commit fails | The switch succeeded and the DB is consistent; show the target mode's first-run state (§7.2), not a failure. |
| A capture thread hangs during teardown | **Not recoverable in-handler.** `std::thread::join()` has no timeout and `CameraStream::stop()` reports no error, so control never returns to surface anything. Ordinary teardown is effectively non-throwing; a hang is an unresolved operational failure. A watchdog is separate design work (§11-R1). |

**The in-memory / UI current mode must not be mutated before the commit
succeeds.** On rollback the handler must re-read the mode from the database
rather than keep a target value it assigned optimistically — otherwise SQLite's
atomicity is intact while the running process disagrees with the DB about what
the appliance is doing, which is worse than either failure alone. This is an
orchestration obligation with a test, not something the transaction can enforce.

**No confirm/revert countdown.** The display-settings pattern works there because
reverting *restores* the prior state. Here the processing setup is deleted, so a
countdown that "reverts" would be a lie. Confirmation is up-front (§7.1).

---

## 7. Operator-facing behaviour

### 7.1 Confirmation (A3, approved)

Follows the Areas destructive-save precedent (`areas_page.cpp:734-751`): real
counts, stated consequence, closed question, **default Cancel**. No
type-to-confirm field.

> **Switch to Floating Ball Leveler?**
>
> **3 camera connections will be kept** — sources, credentials, resolution and
> orientation are preserved.
>
> Their Digital Number Reader setup will be deleted: **model bindings**,
> **7 detection areas**, **4 reported zones** (3, 4, 5, 7), **1,284 stored
> readings**, and **2 model-rollback receipts**. Each camera will need
> processing setup again.
>
> **Server reporting will be turned off.** The server address is kept; you must
> re-enable reporting yourself.
>
> **Floating Ball Leveler setup is not available in this release.**
>
> This cannot be undone.
>
> [Cancel] [Switch and Reset]

Counts are read immediately before teardown and must be real. The destination's
unavailability must appear **before** the operator commits. The dialog must not
say or imply that cameras are deleted.

### 7.2 Target-mode first run

After a switch, `camera::all()` is non-empty while `camera::runtime()` is empty.
The existing two-page `CameraView` (`camera_view.cpp:28-86`) cannot express
that, and its page-0 copy — "No cameras yet" — **would be a lie**. A third state
is required:

| Condition | State |
|---|---|
| `all()` empty | existing "No cameras yet" + **+ Add Camera** |
| `all()` non-empty, `runtime()` empty, mode `digit_reader` | **"N camera connections kept — processing setup required"**, listing each retained camera's name and source, with a **Set up cameras** action opening the existing camera dialog |
| `all()` non-empty, `runtime()` empty, mode `ball_leveler` | the same retained-connection list, with **"Floating Ball Leveler setup is not available in this release"** and **no** setup action |
| `runtime()` non-empty | the live grid, unchanged |

Retained cameras re-enter service only through the existing wizard's explicit
terminal action, which is the sole writer of `setup_complete = 1`
(`mark_setup_complete`, `repo.cpp:191-203`, which requires exactly one affected
row). No new completion path is introduced.

**"No stream" means no production `CameraStream`.** The `digit_reader` wizard
legitimately opens a retained source before setup completes: `open_configure()`
triggers `capture_snapshot()` → `grab_snapshot()` on a worker, using the
retained USB index or credential-bearing RTSP URL
(`wizard_controller.cpp:87-...`). That is a one-shot configuration preview — no
`CameraStream`, no processor, no reporter, no grid admission — and it is both
acceptable and necessary, since verifying the connection and ROI geometry is the
point of re-running setup. It is also the payoff of A1: the preview works
immediately because the credentials were never discarded.

For `ball_leveler` no stream, processor or reporter is constructed at all — the
grid is never populated because `runtime()` is empty, no wizard is exposed so
even the snapshot path is unreachable, and reporting is disabled by §6.3.

---

## 8. Compatibility with existing installations

- Absent `mode.target` ⇒ `digit_reader`. An upgraded appliance keeps running
  with no operator action and no schema change.
- No existing table, query or reporting path changes shape. `runtime()`,
  `detection_for`, the zone namespace and the brazing payload are untouched.
- **The full existing `ctest` suite must pass unchanged**, and a `digit_reader`
  appliance that never switches must behave identically.

---

## 9. `status.json`

Add two optional top-level keys, each with a real producer (the codebase forbids
speculative values — `integrity.h:33-35`):

```json
{ "mode": "digit_reader" | "ball_leveler",     // omitted when undeterminable
  "mode_setup_required": true | false,          // omitted when undeterminable
  "status": ..., "blockers": [...], "issues": [...],
  "camera_causes": [...], "held_zones": [...], "inhibited_zones": [...] }
```

**Both keys are OMITTED — never guessed — when the database cannot be read.**
The earliest writer (`main.cpp:197-198` via `report_db_blocker`) runs precisely
when the DB is unopenable, schema-newer or migration-failed. Emitting
`"mode": "digit_reader"` there would report a mode nobody selected, from a
database nobody could read. The `parse_target_mode` default applies to an
absent/corrupt key in a *readable* DB.

**`mode_setup_required` predicate:** true iff **no `camera` row has
`setup_complete = 1`**. Deliberately not `runtime().empty()`, which also requires
`active = 1` — a fully configured appliance whose cameras are temporarily
disabled is not "setup required", and conflating them would make the field lie
in exactly the situation an operator is most likely inspecting it. After a
switch this is true because §6.3 zeroes the column on every row.

**Consolidation required first.** The path is composed independently in three
places (`main.cpp:197-198`, `startup.cpp:134-136`, `camera_grid.cpp:330`). Add
`paths::status_file()` (`src/core/paths/paths.h:21-28`) and route all three
through it.

**No new `ZoneIssue::Kind` or `ZoneCause` bit.** A future Leveler cause takes the
next free bit — `1u << 5` — never `1u << 2`, deliberately vacant from a removed
speculative value (`zone_health.h:19-26`). The bits are a file format: they are
serialized into `camera_causes`.

---

## 10. Locked decisions

| # | Decision |
|---|---|
| **A1** | **Camera identity, connection and capture config are PRESERVED**, including `id` and `active`. Only `setup_complete` and `areas_need_review` reset. Cameras are never deleted by a switch. |
| **A2** | `brazing.base_url` preserved; **`brazing.enabled` set false inside the transaction**. Re-enabling is an explicit operator action. |
| **A3** | Destructive confirmation with real counts, Cancel / Switch and Reset, default Cancel. No type-to-confirm. Must state cameras are kept, processing setup and readings destroyed, reporting disabled, destination possibly unavailable, undo impossible. |
| **A4** | `model_migration_receipt` destroyed — primary-key reuse makes retained receipts unsafe. |
| **R2** | Stale-generation callback isolation is a **mandatory prerequisite slice** (§5), landed and reviewed before mode work. |

---

## 11. Risks

- **R1 — the switch will visibly freeze the UI.** `stop()` joins serially on the
  GUI thread, and GStreamer capture candidates are opened with **no timeout** —
  only `usb-any` and `rtsp-ffmpeg` set `CAP_PROP_OPEN/READ_TIMEOUT_MSEC`
  (`camera_stream.cpp:97-135`); up to three candidates per source may block.
  Unmeasured. **Locked mitigation:** busy state + disabled action (§6.1);
  **measurement on `.15`** is a plan task. Fixing the timeout gap is separate.
- **R3 — engines are never unloaded.** `EngineRegistry` caches per filename and
  never erases (`engine_registry.h:5-7`), so a switch frees no GPU memory.
  **Out of scope for this release**; must be measured once a second model exists.
- **R4 — undelivered snapshots dropped silently.** Correct, but must be logged —
  counts and zone numbers only, never payload values (§6.6).
- **R5 — no "mode changed" signal to the backend.** The numeric-only backend
  cannot distinguish "switched jobs" from "died". Documented limitation.
- **R6 — the Leveler has no Leveler-specific readiness vocabulary.**
  `evaluate_integrity` remains camera/model-centric and may still report generic
  infrastructure or integrity blockers. It cannot express whether Floating Ball
  Leveler configuration is complete or healthy. In this release,
  `mode_setup_required` remains true for `ball_leveler`. A real Leveler fault has
  no `ZoneIssue::Kind`, and none may be added without a real producer. Documented
  limitation.
- **R7 — SQLite primary keys are reused** (no `AUTOINCREMENT`). Now scoped to
  `camera_model.id` and `camera_area.id`, since `camera.id` is stable under A1.
  Any future proposal to retain a structure holding *child* ids across a switch
  must be checked against this — it is why A4 exists.
- **R8 — retained camera ids make stale in-memory events land on real rows.**
  The direct consequence of A1: a stale `ZoneHealth`/`WorkerFailedFn` event
  carrying `camera_id 3` now matches a real retained camera. §5 is the fix and
  is a hard prerequisite; this risk is *the reason* it is one.

---

## 12. Acceptance criteria

1. A `digit_reader` appliance that never switches behaves identically; the full
   existing `ctest` suite passes unchanged; schema stays v13.
2. Absent/corrupt `mode.target` resolves to `digit_reader`.
3. "Reset to defaults" on the Display page does not change the mode.
4. **A switch preserves every `camera` row, its `id`, and all 18 connection /
   capture columns byte-identically** — asserted field by field, including
   `active`.
5. A switch sets `setup_complete = 0` and `areas_need_review = 0` on every
   camera, and destroys every row of `camera_area`, `camera_model`,
   `camera_model_class`, `reading`, `model_migration_receipt`.
6. **`camera::runtime()` is empty immediately after a switch**, including for a
   camera whose `active = 1`.
7. `brazing.enabled` is false after a switch; `brazing.base_url` is unchanged;
   the rebuilt grid constructs no `ZoneReporter`/`BrazingReporter`.
8. Zero orphan rows remain in `camera_area`, `camera_model`,
   `camera_model_class`, `reading` — asserted directly, since foreign keys
   cannot enforce it.
9. A failure at any statement leaves the mode key, `brazing.enabled`, **and
   every camera's processing flags** at their pre-switch values.
10. After teardown begins, no inference result or retry tick initiates a new
    transport request (§6.6); no `BrazingRetryPolicy`, `ZoneAggregator` or
    `ZoneReporter` state survives — in particular the target mode's first
    snapshot is never suppressed by a stale `delivered_`
    (`brazing_retry_policy.cpp:15`).
11. Capture and inference threads are joined before the `ZoneReporter` is
    destroyed; no callback lands in a destroyed object.
12. After a switch the UI shows retained connections and a
    processing-setup-required state — **never "No cameras yet"** — and
    `status.json` reports the new mode with `mode_setup_required: true`.
13. On a DB-stage blocker, `status.json` **omits** `mode`/`mode_setup_required`.
14. Selecting `ball_leveler` constructs no production `CameraStream`, processor
    or reporter, and offers no setup action.
15. **Prerequisite (§5):** an old-generation worker failure delivered after a
    rebuild does not inhibit a camera in the new generation.
16. `camera_model_class` is empty after a switch **even if the database
    contained rows already orphaned** by a previous `camera::remove`.
17. On rollback, the in-memory/UI mode equals the mode stored in the database —
    no optimistic target value survives a failed switch.
18. The pre-transaction teardown does not restart any camera: no `CameraStream`
    is constructed between the confirm and the commit/rollback.

---

## 13. Testing strategy

Unit (Catch2, no GPU/display):

- `parse_target_mode`: round-trip; unknown/empty/corrupt ⇒ `digit_reader`.
- Mode config load/save over an in-memory DB; absent key ⇒ default.
- **Reset transaction** over an in-memory DB seeded with cameras (some
  `active=1`, some `0`), areas, attachments, class rows, readings and receipts:
  - the five tables are empty and no orphans remain;
  - **every camera row still exists with the same `id` and identical connection
    /capture columns** — compared field by field;
  - `setup_complete`/`areas_need_review` are 0 on all rows;
  - `brazing.enabled` is `"0"`, `brazing.base_url` unchanged;
  - `settings` display keys and `net_config` unchanged.
- **`camera::runtime()` empty after reset**, including a camera left `active=1`.
- **Orphan repair**: seed a `camera_model_class` row whose parent `camera_model`
  is absent (the state `camera::remove` leaves behind) and assert the reset
  removes it — the scoped delete would not.
- **Rollback**: inject a failure at each statement in turn (prepared-statement
  seam or a `RAISE` trigger) and assert the mode key, `brazing.enabled`, and
  every camera's `setup_complete`/`areas_need_review` are unchanged; and that
  the mode reported in memory afterwards equals the mode in the database.
- `settings::save` / `on_reset_defaults` does not disturb `mode.target`.
- `mode_setup_required` true with zero completed cameras; **false when a
  completed camera exists but is inactive** — the case distinguishing it from
  `runtime()`.
- Processor selection is a pure function of mode + camera config.

Integration (Qt, offscreen):

- Teardown ordering with a stubbed sink: no `on_zones` after the inference join;
  reporter → brazing → health released in that order.
- **Prerequisite regression (§5):** an old-generation `WorkerFailedFn` delivered
  after a rebuild must not inhibit a camera in the new generation — with a
  retained camera id, which is the case A1 creates.
- The three `CameraView` states render correctly, and the retained-connection
  state never exposes an action that could start a camera before setup.
- **The pre-transaction teardown constructs no `CameraStream`** — guards against
  regressing to `CameraView::reload()`, which would restart the old pipeline.
- `status.json` gains the fields, and omits them on a simulated DB blocker.

Native Jetson — **`.15` only; `192.168.1.81` is reserved for the user's manual
`.deb` testing and must not be accessed**:

- Real switch with live RTSP cameras; **measure** the GUI stall (R1).
- Confirm no POST reaches a listening server after the confirm is accepted.
- Confirm retained cameras stream correctly after re-completing setup — the
  point of A1.

---

## 14. Revision history

**Revision 3b** — editorial erratum found during final validation (documentation
only; no source change, and the specification stays **APPROVED**):

| Change | Why |
|---|---|
| §11-R6 rewritten: "the Leveler has no **Leveler-specific** readiness vocabulary" | Revision 3a's claim that "a Leveler appliance evaluates **Ready** regardless of configuration" was **overbroad**. `evaluate_integrity` is mode-independent, so a `ball_leveler` appliance can and does still report generic infrastructure/integrity verdicts — the Slice-7 manual gate observed `"status":"degraded"` with a real `engines_unmanifested` issue in `ball_leveler`. The genuine limitation is narrower: the verdict has no vocabulary for *Leveler* configuration completeness or health. |

Nothing else changes: A1–A4 and R2 are untouched, §6 transaction behaviour is
untouched, §12 acceptance criteria are untouched, and no implemented scope is
altered.

**Revision 3** — product-owner decisions locked:

| Change | Why |
|---|---|
| **A1 flipped: cameras are preserved, not deleted** | Connection/capture config is shared infrastructure; re-entering RTSP URLs, credentials and orientation on every mode switch is unacceptable. Reset now `UPDATE`s two processing columns instead of deleting rows. |
| `active` preserved; safety rests on `setup_complete` | `runtime()` ANDs both, so zeroing `setup_complete` alone guarantees nothing streams (§6.4). |
| **A2 changed: `brazing.enabled` set false in-transaction** | Reporting must stop on a switch and must not resume implicitly when the new mode is configured. Revision 2's "safe because no zones exist" made re-arming implicit. |
| Third `CameraView` state added | With cameras retained, "No cameras yet" would be false. |
| **R2 promoted from risk to mandatory prerequisite (§5)** | A1 makes stale events land on real retained camera ids (R8). |
| Confirmation text rewritten | Must say connections are kept and reporting is disabled. |
| R7 rescoped to child ids | `camera.id` is now stable, removing one hazard class. |
| Acceptance + tests extended | Field-by-field camera preservation, `runtime()` emptiness with `active=1`, reporting-disable, rollback of camera flags. |

**Revision 3a** — after the second Codex review (four corrections):

| Change | Why |
|---|---|
| `CameraView::reload()` removed as an acceptable pre-transaction teardown | It calls `clear()` then immediately re-queries `runtime()` — still the *old* mode's data — and restarts every camera. Teardown-only before the transaction; `reload()` only after commit/rollback. |
| `camera_model_class` delete made unconditional | The scoped form skipped rows already orphaned by `camera::remove` — which this very spec documents the codebase creating — contradicting the "table is empty" acceptance criterion. Unconditional also repairs pre-existing damage. |
| In-memory mode must not change before commit | SQLite atomicity cannot stop the running process from disagreeing with the DB about the current mode. |
| "No stream" clarified to mean no production `CameraStream` | The wizard's snapshot preview legitimately opens a retained source before completion — and works precisely *because* A1 preserved the credentials. |
| §6.3 rationale corrected | `brazing::save` does not manage a transaction; it makes two **unchecked** `set()` calls returning no status. The conclusion was right, the reason was not. |

**Revision 2** — after the first Codex review: dropped the speculative v14
`camera.owner_mode` migration; moved `model_migration_receipt` from preserved to
destroyed (primary-key reuse); fixed `mode_setup_required` (was
`runtime().empty()`, which misreports a disabled-but-configured appliance);
`status.json` omits mode fields when the DB is unreadable; defined
`ball_leveler`'s stage-1 behaviour instead of leaving it TBD; reframed the
side-by-side rejection as cost rather than impossibility; corrected the
teardown-failure row (`join()` has no timeout); removed a "measured" claim that
had no measurement; required calling the authoritative teardown primitive rather
than restating it; split the POST guarantee into initiate-vs-complete.

**Revision 1** — initial draft from four parallel code investigations.
