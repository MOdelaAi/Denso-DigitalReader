# Implementation note — one Save action, Backend indicator, Refresh Cameras

Continues `wip/live-brazing-settings` (dbbe204). Three focused UI changes; no
shell redesign, no new page, no new reporting pipeline.

## What the buttons meant before

| Control | Scope | Persisted? | Applied? |
|---|---|---|---|
| Footer **Apply** (gold) | display mode + window size only | on Keep, after a 15 s confirm/revert | yes, immediately |
| Footer **Close** | — | — | — |
| Server page **Save** (gold) | brazing only | yes | yes (live reload) |
| Dark-mode toggle | theme | immediately, on toggle | immediately |
| NetworkPanel card **Apply** / **Connect** | one adapter | its own domain | its own domain |
| Mode page **Switch** | mode transaction | its own confirm dialog | — |

Two gold primaries with disjoint meanings, plus a toggle that committed itself
before the operator agreed to anything. That is the ambiguity.

## A. One primary action

Footer becomes `Reset to defaults` … `Cancel` `Save changes`. The Server page's
own Save button is **deleted**; its inline status label stays, because a
validation message belongs next to the field that failed.

`Save changes` (`saveChangesButton`) runs one ordered pass:

**Phase 1 — validate and persist. No runtime effect yet.**

1. **Validate** the Server page through `brazing::normalize_base_url` (unchanged
   rule). On failure: switch the nav to Server, show the reason under the field,
   keep the dialog open, persist nothing, emit nothing.
2. **Persist** brazing rows via `brazing::save_rows`.
3. **Persist** the theme rows through a `theme_committer_` functor installed by
   MainWindow (a functor, not a signal, because a signal cannot report failure),
   which calls `settings::save_rows` on a **copy** of the settings struct — so a
   later rollback cannot leave the in-memory struct claiming what the database
   does not hold.

`save_changes()` **owns one transaction across all of phase 1**: BEGIN → 1 → 2 →
3 → COMMIT. Any failure rolls the whole form back, keeps the dialog open with the
reason, and emits nothing — so a Save reported as failed can never leave the
Server page persisted and the display page not. Both modules gained a
`save_rows()` (rows only, no transaction control) precisely because SQLite has no
nested transactions; their existing `save()` is that wrapped in its own checked
transaction and is unchanged for every other caller. `settings::save` also gained
the driver-capability guard `brazing::save` already had: a transaction-capable
driver that cannot BEGIN now returns false having written nothing.

**Phase 2 — apply, only once every write landed.**

4. **Emit** `brazing_config_changed`.
5. **Emit** `theme_changed` — MainWindow now only *applies* it and updates
   `state_` (the write already happened in phase 1, so persistence no longer
   trails the apply, and `state_` moves only after a successful commit).
6. **Display**: emit `apply_display_requested` — MainWindow's existing deferred
   confirm/revert transaction applies, then persists on Keep.
7. `accept()` — closing on success matches what Apply already did.

`Cancel` (`cancelButton`) is wired to `QDialog::reject`, which is **overridden**:
Esc, the header close glyph and the window manager's close box all end there too,
so every dismissal restores the entry theme. Wiring only the button would have
left an Esc-dismissed dialog with its preview applied and nothing persisted.
Nothing is written and no commit signal fires on any of those paths.

**Theme is previewed, not committed.** The toggle now emits a new
`theme_preview_requested(bool)`, which MainWindow applies **without** persisting;
`theme_changed` (persist + apply) fires only from Save. Cancel emits the preview
signal again with the theme the dialog opened on. Live preview survives, and
Cancel genuinely applies nothing.

**Dirty state.** `dirty_` is DERIVED — `current_form() != baseline_` over every
editable value — not latched. Every seeding path is suppressed and re-captures
the baseline instead. `Save changes` is disabled while `!dirty_`. See section D
for why a sticky flag could not work.

**Deliberately left alone.** The display confirm/revert inverts persist/apply on
purpose — an unusable display mode must be revertible, and a mode persisted
before the operator can see it would brick the panel. `Reset to defaults`
(flat, tertiary), the Mode page's `Switch` and NetworkPanel's per-card
`Apply`/`Connect` are page/card-scoped actions, not competing dialog primaries;
none are gold.

## B. Backend indicator

`BrazingStatus{Off, On, Error}` (`src/app/brazing/brazing_status.h`).

**CameraGrid stays the single authority** — the indicator reads the same object
that owns the sender, never a second config read:

* `apply_brazing_config()` sets `On` when it has a live sender, `Off` otherwise
  (so an invalid or empty URL can never read `On`, and a mode switch's
  `brazing.enabled = 0` lands here on the next apply);
* `BrazingReporter` gains `delivery_succeeded` / `delivery_failed`, emitted from
  the existing `QPointer`-guarded result callback. The grid maps them to
  `On` / `Error`. No polling, no health request — the backend still has only
  `POST /api/brazing/update`.

`CameraGrid::brazing_status_changed` → `CameraView` (pure forward) →
`MainWindow::on_brazing_status_changed`. MainWindow also calls
`refresh_brazing_indicator()` after construction and after a mode switch, because
`CameraView`'s constructor reloads before the window can connect, and a
ball_leveler grid with no camera never enters `build_zone_reporting()`.

The widget is a flat `QPushButton` (`backendStatus`) reading
`Backend: OFF | ON | ERROR`, coloured by a `backendState` property against the
theme's existing `status_ok` / `status_bad` / `status_neutral` palette entries
(new `%(statusOk)` / `%(statusBad)` / `%(statusNeutral)` QSS substitutions), so
both themes stay readable and nothing is hard-coded. Deliberately **not**
"Connected": there is no health endpoint, and `On` means the reporting stack is
running, not that the server answered. Clicking opens Settings on the Server
page; it never toggles reporting, so production reporting cannot be disabled by
a stray click. The tooltip names the state, the canonical base URL (which cannot
contain credentials — the normalizer rejects userinfo) and what a click does.

## C. Refresh Cameras

`refreshCamerasButton` in the top bar, tooltip "Reconnect and reload all
configured cameras". `MainWindow::refresh_cameras()` is a public slot (so a test
drives the same entry point the button does):

* refuses while a refresh, a mode switch or a display transaction is running;
* disables itself, shows `Refreshing cameras…`, then defers one tick so the busy
  state actually paints before the synchronous rebuild;
* calls **`CameraView::reload()`** — the existing seam. No startup logic is
  duplicated: `CameraGrid::reload()` bumps `generation_` (so every callback the
  old workers captured is rejected), stops and joins every stream, deletes the
  tiles, and rebuilds from the same persisted rows;
* reports the outcome in a non-modal `refreshStatus` label — `N of M cameras
  started` when some camera built no runtime, cleared on a clean refresh. **Both
  numbers come from the grid** (`admitted_count()` vs `stream_count()`), never
  re-derived from database rows out in the window: the grid alone knows the mode's
  admission filter and the deliberate four-tile cap, and counting rows would
  report "4 of 5 cameras started" on a five-camera appliance behaving exactly as
  designed. Modal boxes are wrong on a factory panel and untestable; per-camera
  connection failures continue to surface on the tile itself.

Nothing is written to the database, so cameras, models, areas/zones, decimal
formats, Ball calibration, `mode.target`, `brazing.enabled` and
`brazing.base_url` are all untouched by construction. The rebuilt grid re-reads
the same brazing config through the same `apply_brazing_config()`, so there is
exactly one reporter afterwards.

**Pending Backend retries now survive a refresh.** `CameraGrid::clear()` no
longer destroys the `BrazingReporter`; retiring it moved to `teardown()`, the one
caller that must not rebuild. So a mode switch still drops the old mode's
un-acked snapshot (spec §6.6, unchanged and tested), while a camera rebuild —
the wizard closing, or Refresh Cameras — keeps the sender, its pending snapshot
and its backoff, and never churns the top-bar indicator. `clear()` remains the
single camera-teardown sequence; the two callers differ only where they genuinely
differ. Nothing can submit in the gap: every worker is joined before the sender
would be reachable again, and `reload()`'s early-exit paths call
`apply_brazing_config()` so a grid that builds no pipeline keeps no sender.

## D. Settings ↔ authoritative Backend configuration (follow-up fix)

**Symptom.** Backend ON → switch mode → the database correctly holds
`brazing.enabled = 0` and the top bar correctly reads `Backend: OFF`, but
Settings → Server still shows the Enable-Backend checkbox ticked.

**Root cause, confirmed empirically** with a throwaway two-case probe (since
deleted):

| scenario | result |
|---|---|
| dialog left **open** across `perform_switch` | checkbox stays ticked — **FAILED** |
| dialog closed, then reopened across `perform_switch` | checkbox correctly unticked — passed |

So it was never a caching bug. The Server page is seeded only at construction and
in `showEvent`, and the **Switch button lives on this same dialog's Mode page** —
the switch therefore runs with the dialog still visible, `showEvent` never
re-fires, and nothing pushed the new configuration in.

**Fix.** `SettingsDialog::refresh_backend_state()` — a public, *passive* re-sync
that re-reads `brazing::load(db_)`, the same authority `apply_brazing_config()`
and Save already use. No new state holder: the checkbox and the URL field stay an
editor view of the stored configuration. `MainWindow::perform_switch()` calls it
alongside `refresh_brazing_indicator()`, for a committed **and** a rolled-back
switch — re-reading truth is right either way, and guessing the outcome here
would be a second authority. The three post-switch window updates now each have
their **own** exception guard, so a throw in one cannot skip the sync.

The sync writes nothing, emits no `brazing_config_changed` (a redisplay is not an
operator action, and the runtime already followed the switch through its own
path), and cannot arm Save — the widgets move under `suppress_signals_`.

**Dirty became a comparison, not a flag.** A sticky boolean could not express
"this page was re-synced, so its edit no longer exists" without also discarding
an unsaved edit on another page. `FormState` snapshots every editable value;
`baseline_` is the "nothing to save" reference, captured at construction, on
show, on a successful Save, on reject, and by the three authoritative seeding
setters; `recompute_dirty()` compares. `refresh_backend_state()` rebases **only**
the two Server fields — so the operator's overwritten tick stops arming Save,
while a pending display/theme edit stays armed. A field returned to its stored
value by hand now also disarms Save, which the old flag could not do.

`brazing.base_url` is preserved by the switch, so the re-seeded page still shows
the address and the operator never has to retype it to re-enable.

## E. Settings closes itself after a committed mode switch

The Switch button is on the Settings dialog's own Mode page, so after a switch the
operator was left on a form describing the appliance as it was before a
destructive reset.

`SettingsDialog::close_after_mode_switch()` — a no-op when the dialog is not
visible, otherwise delegating to the overridden `reject()`. A committed switch is
a **discard**, not a Save: `reject()` already restores the unsaved theme preview
to the persisted theme, re-captures the dirty baseline, writes nothing and emits
no commit signal. Delegating rather than reimplementing keeps one discard
authority; the method's NAME carries the intent that a bare `reject()` at the call
site would not. No discard confirmation is asked — the operator has just confirmed
a destructive switch.

`MainWindow::perform_switch()` calls it gated on **`outcome == Outcome::Committed`
alone**, after every resynchronisation step and the camera reload, and **before**
the failure message box. Both details were review findings:

* *the gate.* A commit that could not finish updating the window is still a
  commit — both modes' setup is gone, so the form is equally invalid. And Settings
  is application-modal while `show_non_modal()` explicitly is not, so leaving it up
  would put a critical "restart the application" warning behind a dialog the
  operator must dismiss first.
* *the ordering.* Dismissing first means that warning lands on the main window
  already usable. It still says exactly what went wrong, so a degraded switch
  cannot read as an ordinary success.

Cancel, a same-mode refusal, a rolled-back transaction and an unresolved outcome
all leave the dialog exactly where it was. The committed-with-a-failure state
cannot be reached from a test — the only injection seam (`set_switch_observer`) is
called through a `noexcept` `fire()` that swallows throws by design — so the gate
shape and the close-before-warning ordering are pinned by a source guard, the same
mutation-must-die idiom `test_zone_runtime_wiring.cpp` uses.
