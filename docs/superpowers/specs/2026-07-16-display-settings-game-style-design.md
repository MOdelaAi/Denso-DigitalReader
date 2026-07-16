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
4. **Fullscreen escape** — the top bar (with the Settings button) is hidden in
   fullscreen; only F11/Esc get you out.

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
- **No in-fullscreen touch affordance** (floating gear button / edge tab). The
  user chose **F11/Esc only**. See Known limitations.
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
  replacing the clamp-that-lies. A pure helper `fitting_presets(avail_w,
  avail_h)` (Qt-free, unit-tested) returns the indices of `PRESETS` that fit; the
  dialog is populated from that against the screen the window is currently on.
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

When Apply changes display mode and/or window size:

1. Snapshot the **last-known-good** `DisplayState` (mode, normal geometry,
   window flags, the screen the window is on) **before** applying.
2. Apply the new state (§6), let window-system events settle, then show a
   **frameless, always-on-top modal** overlay: *"Keep these display settings?
   Reverting in 15s…"* with a live countdown and **Keep** / **Revert** buttons.
   The overlay must render **above** the (possibly fullscreen) window on Windows
   and X11.
3. **Keep** → persist the new state (§5), close the overlay.
4. **Revert** or **timeout** → restore the snapshot fully (flags + geometry +
   state + screen), persist nothing.
5. **Nothing is persisted until Keep.** This recovers from an unusable render or
   a mode the operator can't read.

The revert-target selection and the "did display actually change?" decision are
pure, testable logic over `DisplayState`; the countdown itself is a `QTimer`.

### 5. Persistence (no SQL migration)

Settings live in the **key/value `settings` table** (`src/core/settings/repo.cpp`),
not columns — so this needs **no schema migration** and `SCHEMA_VERSION` stays 11.

- `settings::Settings` replaces `bool fullscreen` with
  `DisplayMode mode` (`enum class DisplayMode { Windowed, Borderless, Fullscreen }`).
  `width`/`height` remain the **windowed** size; `dark` unchanged.
- `save()` writes a `"display_mode"` key (`"windowed"|"borderless"|"fullscreen"`).
- `load()` reads `"display_mode"`; **legacy fallback**: if that key is absent but
  the old `"fullscreen"` key exists, map `fullscreen=1 → Fullscreen`, else
  `Windowed`. An **unknown/corrupt** `display_mode` value falls back to
  **Windowed** (never strand startup in an unreachable mode).
- `import_legacy` (JSON) gains the same mapping: `fullscreen` bool → `mode`,
  tolerating absence.

### 6. Mode application order (MainWindow)

A single `apply_display_mode(DisplayMode, size)` that:

- **Windowed**: if currently frameless/fullscreen, `showNormal()`, clear
  `FramelessWindowHint` (hide→setFlag→show as needed), then
  `resize_within_screen(size)`.
- **Borderless**: `showNormal()`, set `FramelessWindowHint`, `setGeometry(screen
  full rect)`, show.
- **Fullscreen**: `showFullScreen()`.

Use `showFullScreen()`/`showNormal()` + geometry rather than hand-forcing
positions where the platform constrains it. Startup (`apply_startup`) applies the
persisted mode the same way, **without** the confirm/revert overlay (persisted
state is already trusted).

### 7. Platform-capability guard (defensive)

Query `QGuiApplication::platformName()`. On a WM-less stack (`eglfs`, `linuxfb`)
where windowed/borderless are fictional, **force Fullscreen and hide the Display
mode + Window size controls**. On `xcb` (Jetson), `windows`, `wayland`, all three
modes are offered. Wayland caveat: clients can't dictate top-level position, so
Borderless there falls back to `showMaximized()`-like behavior; acceptable.

## Components / units

| File | Change |
|---|---|
| `src/core/settings/settings.h` | Add `enum class DisplayMode`; replace `bool fullscreen` with `DisplayMode mode`; add pure `fitting_presets(avail_w, avail_h)` + `display_mode` string (de)serialize helpers. |
| `src/core/settings/repo.cpp` | Load/save `display_mode` key + legacy `fullscreen` fallback + safe unknown→Windowed; update `import_legacy`. |
| `src/app/ui/settings/settings_dialog.{h,cpp}` | Display-mode combobox; rename Resolution→Window size + subtext; enable size only in Windowed; stage selections; emit single `apply_display_requested`; drop `apply_resolution_requested`/`toggle_fullscreen_requested`. |
| `src/app/ui/mainwindow.{h,cpp}` | `apply_display_mode()`; `DisplayState` snapshot/restore; confirm/revert transaction; startup applies persisted mode; platform guard. F11/Esc unchanged. |
| `src/app/ui/common/` (new) | A small reusable confirm/revert overlay widget (frameless, top-most, countdown). |
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
- `DisplayMode` ⇄ string round-trip; unknown → Windowed.
- Legacy `fullscreen=1/0` (no `display_mode` key) → Fullscreen/Windowed.
- `import_legacy` JSON mapping incl. missing `fullscreen`.
- `fitting_presets(avail)` filters correctly (all fit, some fit, none fit →
  at least the smallest, or empty handled).
- `preset_index` still round-trips windowed size.
- "display changed?" + revert-target selection over `DisplayState`.

**Manual / on-device (no display over SSH → AnyDesk on Jetson):**
- Windows + Jetson X11: each mode transition; Apply→Keep persists; Apply→wait
  15s auto-reverts; Apply→Revert restores; Window size greyed outside Windowed;
  size list matches the panel; restart restores the kept mode.

## Known limitations (accepted)

- **Keyboardless fullscreen trap:** with F11/Esc-only escape and a hidden top
  bar, an operator on a keyboardless touchscreen who **keeps** Fullscreen has no
  touch path back to Settings until a keyboard is attached. The confirm/revert
  countdown protects only the *transition* (a broken/unreadable render
  auto-reverts), not a deliberately-kept working fullscreen. A future in-screen
  gear button can lift this without changing the rest of the design.
- Borderless is visually ~identical to Fullscreen on a single monitor.
- "Window size" is logical (device-independent) pixels, not guaranteed physical
  panel pixels under display scaling.

## Open questions

None outstanding — the three contested choices (three modes; theme stays live;
F11/Esc-only escape) were decided by the user.
