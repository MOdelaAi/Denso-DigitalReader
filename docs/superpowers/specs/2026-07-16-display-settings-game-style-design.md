# Game-style Display settings (mode + window size + confirm/revert)

Date: 2026-07-16
Status: Approved (design) — pending spec review
Authors: Claude + Codex (debate workflow)

## Context

The Settings → Display panel today (`src/app/ui/settings/settings_dialog.cpp`,
`src/app/ui/mainwindow.cpp`) has:

- A **Resolution** combobox of 4 hardcoded pixel presets (800×600 … 1920×1080)
  that applies **only on the "Apply" button** → `MainWindow::on_apply_resolution`
  → `resize_within_screen()` (clamps to work area + re-centers) + `settings::save`.
- A **Fullscreen** checkbox that applies **instantly on toggle** →
  `set_fullscreen()` → `showFullScreen()`/`showNormal()` + immediate save.

Problems this creates:

1. **Inconsistent apply model** — fullscreen mutates live, resolution waits for
   Apply, theme mutates live; "Close" reverts none of them, so there is no real
   cancel. "Apply" effectively means only "apply resolution".
2. **Clamp-that-lies** — `resize_within_screen` shrinks a preset to fit the work
   area, so the "1920 × 1080" label is not literally what you get.
3. **Resolution ↔ fullscreen** — changing resolution while fullscreen does
   nothing visible; `showNormal()` restores pre-fullscreen geometry, not the
   chosen preset.
4. **Stale claim** — a comment at `mainwindow.cpp:69-71` says the top bar is
   "hidden while fullscreen"; it is **not** (no code hides it — `showFullScreen()`
   only strips OS decorations, and `topBar` is an app widget). The Settings
   button therefore stays touch-reachable in every mode. The comment is corrected
   as part of this work.

The user wants this reworked to feel like a **desktop game's video settings**.

Primary target: a fixed industrial **touchscreen on the Jetson Orin Nano**
(running a full **X11 GNOME** desktop, so windowing works — see
`d:\workspace\devices.md`). Windows is the dev/desktop build.

## Goals

- A game-style Display panel: a **Display mode** selector, a **Window size**
  selector, changes **staged behind Apply**, and a **confirm/revert countdown**.
- Honest controls: no lying labels; "Window size" that does not pretend to be a
  monitor resolution change.
- A single, consistent apply model for display settings (mode + size batched).
- Safe persistence with backward compatibility and a safe fallback for corrupt
  values.

## Non-goals

- **No real GPU / OS display-mode switch** (`ChangeDisplaySettings` / `xrandr`).
  "Window size" resizes the app window only; the monitor's native mode is never
  touched. This is deliberate — fragile in general and actively unwanted on a
  fixed industrial panel.
- **No added in-fullscreen touch affordance** (floating gear button / edge tab).
  The user chose **F11/Esc only** — and this is safe because the **top bar (with
  the Settings button) stays visible in every mode**, including fullscreen, so a
  touchscreen operator always has a path back to Windowed via Settings. F11/Esc
  are convenience shortcuts on top of that.
- No multi-monitor selector, refresh-rate, or VSync controls.

## Design

### 1. Display mode (three modes, game-style)

A `Display mode` dropdown replaces the Fullscreen checkbox, with three values:

| Mode | Behavior |
|---|---|
| **Windowed** | Normal framed window at the chosen **Window size**. `showNormal()`, no frameless hint, `resize_within_screen(size)`. |
| **Borderless** | Frameless window covering the **full screen rect** (`screen()->geometry()`), native res, no mode switch — the "borderless windowed" games offer. Precisely: `Qt::FramelessWindowHint` + geometry = the current screen's full rect, normal (non-fullscreen) window state. |
| **Fullscreen** | `showFullScreen()` — fills the screen at native res; **not** an OS mode switch. |

Toggling `Qt::FramelessWindowHint` on a live top-level requires a hide/show
cycle and re-assertion of geometry/state (Qt caveat); mode application must do
this in a defined order (see §6) rather than flipping the flag in place.

### 2. Window size (renamed from "Resolution")

- Label becomes **"Window size"** with subtext *"Does not change the monitor
  resolution."*
- **Enabled only in Windowed mode**; disabled (greyed) in Borderless/Fullscreen,
  where the window fills the screen and a size is meaningless.
- The preset list is **filtered to sizes that fit the current screen's available
  logical geometry**, so no label ever exceeds what the window can actually be —
  replacing the clamp-that-lies. A pure helper `fitting_presets(avail_w, avail_h,
  frame_w, frame_h)` (Qt-free, unit-tested) returns the `PRESETS` indices whose
  **framed** size (client + window-frame overhead) fits `availableGeometry`. It
  returns **empty when none fit** (the dialog then shows the current size as a
  disabled single entry — never an oversized lie).
- **Combo item-data holds the real `PRESETS` index** (`setItemData`), because
  after filtering the visible combo row no longer equals the preset index. All
  size lookups go through the item-data id, not the row.
- The filtered list is **rebuilt whenever Settings opens** and when the target
  screen / available geometry changes, against the screen the window is currently
  on (not `primaryScreen()`).
- **Persisted size no longer offered** (screen shrank, or came from another
  machine): stage the **largest fitting preset**, and do **not** persist it until
  a confirmed Apply.
- Note: Qt geometry is in device-independent pixels; under display scaling a
  preset need not equal physical panel pixels. Acceptable — "Window size" is a
  logical window size, not a panel-pixel promise.

### 3. Staged apply (display batched; theme stays live)

- **Display mode + Window size are staged**: editing the dropdowns changes only
  the dialog's pending selection, not the window. They apply together when the
  user presses **Apply**.
- The dialog emits a single `apply_display_requested(DisplayMode, int size_index)`
  replacing the separate `apply_resolution_requested` / `toggle_fullscreen_requested`.
- **Theme stays instant** (user's choice): the dark-mode toggle keeps emitting
  `theme_changed` live and persists immediately, exactly as today. Theme is not
  part of the confirm/revert transaction.

### 4. Confirm / revert transaction

Applies only when the change is a **semantic display change** (see below), never
for theme-only.

1. Snapshot the **last-known-good semantic `DisplayState`** *before* applying:
   `{ mode, windowed_size (normal client size), screen_name }`. **Not** raw Qt
   window flags — those are stale/transient and `setWindowFlags()` recreates+hides
   the native window. **Scope note:** the target is a **single-panel** kiosk, so
   revert restores **mode + windowed size on the current screen**; `screen_name`
   is captured for a future multi-monitor pass but is **not** acted on today
   (no cross-screen restoration). Do not claim screen restoration in code comments.
2. Apply the new state (§6), let window-system events settle (a queued tick),
   then show the confirm overlay: *"Keep these display settings? Reverting in
   15s…"* with a live countdown + **Keep** / **Revert**.
3. **Keep** → persist the new state (§5, transactional), close the overlay.
4. **Revert** or **timeout** → **re-apply the snapshot by calling the same
   canonical mode applicator** (§6) — do not replay raw flags. Persist nothing.
5. **Nothing is persisted until Keep.**

**Semantic "display changed?"** = mode differs, OR (mode is Windowed on both
sides AND windowed_size differs). Re-picking the same values, or changing the
staged size while staying Fullscreen/Borderless, is **not** a display change and
skips the overlay — but a staged windowed size still records what a *future*
Windowed switch would use (saved only on a confirmed Apply).

**Interference while a transaction is pending** must be explicitly routed:
- **Esc** / **Revert button** → revert the transaction.
- **F11**, a second **Apply**, **Reset defaults** → rejected while a transaction
  is pending (each early-returns on the `display_txn_active_` guard).
- **Reset defaults** is itself the *recovery* action: it always lands on the safe
  Windowed default, so it applies **directly, without a confirm/revert countdown**
  — a countdown would be backwards, since a timeout would restore the very
  (possibly unusable) state the operator reset away from. Theme (dark) is reset
  live alongside it.
- **Window close / app shutdown** → persist nothing; startup keeps the last kept
  state (fail safe).

**The overlay** is a **child-owned top-level `QDialog`** with
`Qt::WindowStaysOnTopHint`, application-modal, explicitly `raise()`+
`activateWindow()`ed after the applied window settles — *not* a generic reusable
widget (no second consumer yet). **Failure criterion** (fail-safe → immediate
revert): the dialog fails to create/show, or a short queued post-show check finds
it not visible / not the active window.

The "display changed?" test, the semantic snapshot, and the revert plan are pure,
testable logic over `DisplayState`; the countdown is a `QTimer`.

### 5. Persistence (no SQL migration)

Settings live in the **key/value `settings` table** (`src/core/settings/repo.cpp`),
not columns — so this needs **no schema migration** and `SCHEMA_VERSION` stays 11.

- `settings::Settings` replaces `bool fullscreen` with
  `DisplayMode mode` (`enum class DisplayMode { Windowed, Borderless, Fullscreen }`).
  `width`/`height` remain the **windowed** size; `dark` unchanged.
- `save()` **dual-writes** so an older binary opening the same DB degrades
  safely (it only knows the `fullscreen` key):
  - Windowed → `display_mode=windowed`, `fullscreen=0`
  - Borderless → `display_mode=borderless`, `fullscreen=0` (old build → Windowed)
  - Fullscreen → `display_mode=fullscreen`, `fullscreen=1`
  Writes must be **transactional** (all keys in one commit) so a partial failure
  can't leave `display_mode` and `fullscreen` disagreeing.
- `load()` **prefers `display_mode`**; if absent, **legacy fallback** to the old
  `fullscreen` key (`1 → Fullscreen`, else `Windowed`). An **unknown/corrupt**
  `display_mode` falls back to **Windowed** (never strand startup in an
  unreachable mode).
- `import_legacy` (JSON) gains the same mapping: `fullscreen` bool → `mode`,
  tolerating absence.

### 6. Mode application (MainWindow) — canonical states, deterministic sequence

Each mode is a **canonical absolute state**, never derived incrementally from the
previous mode (so no stale `FramelessWindowHint` survives e.g. Borderless →
Fullscreen):

| Mode | Fullscreen state | Frameless flag | Geometry |
|---|---|---|---|
| Windowed | no | **off** | `resize_within_screen(size)`, centered |
| Borderless | no | **on** | current screen's **full** rect |
| Fullscreen | **yes** | **off** | (screen native, via `showFullScreen`) |

Because `setWindowFlags()`/frameless changes recreate and hide the native window,
`apply_display_mode(mode, size)` uses **one deterministic sequence**:

1. Capture target `QScreen*` + desired geometry.
2. `hide()`.
3. Set the **canonical** flags for the target mode while hidden
   (`FramelessWindowHint` on for Borderless, off otherwise).
4. `setWindowState(Qt::WindowNoState)` to clear any prior fullscreen/maximized.
5. `show()` on the intended screen.
6. Re-assert geometry after the native window is (re)created: Windowed →
   `resize_within_screen`; Borderless → `setGeometry(full screen rect)`;
   Fullscreen → `showFullScreen()`.

> xcb note (Jetson): some WMs honor geometry only after mapping, so Borderless
> may need `show()` then a **queued** `setGeometry(fullRect)`. This ordering is
> verified on-device (X11 GNOME), not assumed.

**Pure transition planner (testable):** a Qt-free
`plan_transition(current_semantic_state, requested_mode, requested_size,
platform_caps) → { target flags-state (as an enum triple), geometry action,
needs_confirm }`. `MainWindow` executes the plan against real `QWidget` calls;
the *decision* logic (what to do) is unit-tested, the *execution* (QWidget) is
manual/platform-tested.

Startup (`apply_startup`) runs the planner+applicator for the persisted mode
**without** the confirm/revert overlay (persisted state is already trusted).

Also: **correct the stale comment** at `mainwindow.cpp:69-71` — the top bar is
not hidden in fullscreen; F11/Esc are convenience shortcuts, not the only exit.

### 7. Platform-capability guard (defensive)

Query `QGuiApplication::platformName()`. On a WM-less stack (`eglfs`, `linuxfb`)
where windowed/borderless are fictional, **force Fullscreen and hide the Display
mode + Window size controls**. On `xcb` (Jetson), `windows`, `wayland`, all three
modes are offered. Wayland caveat: clients can't dictate top-level position, so
Borderless there falls back to `showMaximized()`-like behavior; acceptable.

## Components / units

| File | Change |
|---|---|
| `src/core/settings/settings.h` | Add `enum class DisplayMode`; replace `bool fullscreen` with `DisplayMode mode`; pure helpers: `fitting_presets(avail_w, avail_h, frame_w, frame_h)`, `display_mode` ⇄ string, `plan_transition(...)`, semantic `DisplayState` + "changed?"/revert logic. |
| `src/core/settings/repo.cpp` | Load `display_mode` (prefer) + legacy `fullscreen` fallback + unknown→Windowed; **transactional dual-write** (`display_mode` + `fullscreen`); update `import_legacy`. |
| `src/app/ui/settings/settings_dialog.{h,cpp}` | Display-mode combobox; rename Resolution→Window size + subtext; size combo carries `PRESETS` id in item-data, enabled only in Windowed, filtered+rebuilt on open/screen-change; stage selections; emit single `apply_display_requested`; drop `apply_resolution_requested`/`toggle_fullscreen_requested`. |
| `src/app/ui/mainwindow.{h,cpp}` | `apply_display_mode()` (canonical sequence); semantic snapshot/restore via the applicator; confirm/revert transaction + interference routing; startup applies persisted mode; platform guard; **fix the stale `topBar`-hidden comment**. |
| `src/app/ui/settings/display_confirm_dialog.{h,cpp}` (new) | Display-specific child-owned top-level `QDialog` (`WindowStaysOnTopHint`, app-modal, countdown, Keep/Revert). Not in `ui/common` — no second consumer. |
| `tests/` | Unit tests (below). |

## Data flow

```
Dialog (stage mode+size) --Apply--> apply_display_requested(mode, idx)
  -> MainWindow: snapshot last-good DisplayState
  -> apply_display_mode(mode, PRESETS[idx])
  -> settle events -> ConfirmRevert overlay (15s)
       Keep    -> settings::save(state{mode,width,height,dark})
       Revert/timeout -> restore snapshot, save nothing
Theme toggle --theme_changed(dark)--> apply immediately + save   (unchanged)
Startup --> load() (display_mode | legacy fullscreen | Windowed) -> apply_display_mode (no overlay)
```

## Error handling / edge cases

- Corrupt/unknown `display_mode` → Windowed.
- Screen changes / smaller screen than the persisted windowed size →
  `resize_within_screen` + `fitting_presets` keep it on-screen.
- Apply with **no display change** (only re-picked the same values) → skip the
  confirm/revert overlay entirely.
- Overlay must stay above a fullscreen window; if it can't be shown, treat as
  immediate revert (fail safe).
- Frameless toggle that disturbs focus/placement → re-assert geometry after show.

## Testing

**Pure / unit (Catch2, cross-platform):**
- `DisplayMode` ⇄ string round-trip; unknown/corrupt → Windowed.
- Persistence precedence + dual-write: `display_mode` present wins over
  `fullscreen`; new `save()` writes both keys consistently for all 3 modes;
  legacy `fullscreen=1/0` alone → Fullscreen/Windowed; **old-build downgrade**
  (borderless persisted → an old reader sees `fullscreen=0` → Windowed).
- `import_legacy` JSON mapping incl. missing `fullscreen`.
- `fitting_presets(avail, frame)` — all fit / some fit / **none fit → empty**;
  framed size (client+frame) is what's compared.
- `preset_index` still round-trips windowed size.
- `plan_transition(...)` — canonical target per requested mode from every source
  mode (incl. Borderless→Fullscreen leaves frameless **off**); `needs_confirm`
  only on semantic display change; platform-caps force Fullscreen on eglfs.
- Persisted size no-longer-offered → largest fitting preset, not persisted.

**Manual / on-device (no display over SSH → AnyDesk on Jetson):**
- Windows + Jetson X11: each mode transition; Apply→Keep persists; Apply→wait
  15s auto-reverts; Apply→Revert restores; Window size greyed outside Windowed;
  size list matches the panel; restart restores the kept mode.

## Known limitations (accepted)

- **No keyboardless trap** (corrected from the first draft): the top bar with the
  Settings button stays visible in fullscreen, so a touchscreen operator always
  reaches Settings → Windowed without a keyboard. F11/Esc are extras. *(If a
  future change hides the top bar in fullscreen for a cleaner game feel, an
  in-screen gear button must be added at the same time — out of scope here.)*
- Borderless is visually ~identical to Fullscreen on a single monitor (the user
  wants all three modes regardless).
- "Window size" is logical (device-independent) pixels, not guaranteed physical
  panel pixels under display scaling.
- Wayland: Borderless can't force top-level position, so it falls back to
  maximized-within-work-area behavior there.

## Open questions

None outstanding — the three contested choices were decided by the user: three
modes (Windowed/Borderless/Fullscreen); theme stays live; and F11/Esc kept as
the only *keyboard* shortcuts for leaving fullscreen (extras on top of the
always-visible top bar, which stays reachable in every mode — there is no
keyboardless trap).
