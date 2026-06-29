# Color Theme — Design Spec

**Date:** 2026-06-29
**Project:** Denso-DigitalReader (Slint 1.17 + Rust)
**Status:** Approved

---

## Goal

Introduce a centralized color theme as a Slint `global` singleton (`ui/theme.slint`), ported from `colortheme-ref.txt`. Support runtime light/dark switching via a `dark` boolean. Add a "Dark mode" toggle to the Settings modal that re-themes the app live and persists the choice instantly to `settings.json`. Migrate the hardcoded colors in `app-window.slint` to theme tokens.

## Scope

- Create `ui/theme.slint` — `export global Theme` with the full palette, each color a `dark ? <dark> : <light>` ternary on an `in property <bool> dark`.
- Migrate all hardcoded hex colors in `ui/app-window.slint` to `Theme.*` tokens.
- Add a "Dark mode" `Switch` to the Settings modal, two-way bound to `Theme.dark`, that fires a `theme-changed(bool)` callback on toggle.
- Extend Rust `Settings` struct with `dark: bool` (serde default `true` for backward compatibility).
- Rust startup: apply saved `dark` to the Theme global.
- Rust: `on_theme_changed` saves the theme choice instantly (independent of the resolution Apply button).

## Out of Scope

- Theming widgets beyond the colors currently in `app-window.slint`.
- A separate Tailwind→Slint build step (the palette is hand-ported once).
- `boxShadow` / `zIndex` tokens from the reference (no Slint token equivalent; z-order is declaration order, shadows are per-element properties).

---

## File Layout

```
Denso-DigitalReader/
├── settings.json          ← runtime, now { width, height, dark }
├── src/
│   └── main.rs            ← Settings.dark, startup apply, on_theme_changed
└── ui/
    ├── theme.slint        ← NEW: global Theme singleton
    └── app-window.slint   ← import Theme, migrate colors, add Dark mode switch
```

`build.rs` already compiles `ui/app-window.slint`; importing `theme.slint` from it pulls the new file into the same compilation unit (no build.rs change needed).

---

## `ui/theme.slint` — Full Palette

Dark values are verbatim from `colortheme-ref.txt`. Light values are derived: backgrounds invert (dark → light grey), text inverts (light → dark), gold darkens for contrast on white, and status/neutral colors are kept identical across themes for consistent meaning.

```slint
export global Theme {
    in property <bool> dark: true;

    // backgrounds / panels
    out property <color> bg-grad-1: dark ? #232323 : #e8e8e8;
    out property <color> panel:     dark ? #202020 : #f5f5f5;
    out property <color> panel-1:   dark ? #202020 : #f5f5f5;
    out property <color> panel-2:   dark ? #343434 : #e2e2e2;
    out property <color> panel-3:   dark ? #373737 : #dcdcdc;

    // accent (gold)
    out property <color> gold:     dark ? #ffc646 : #e69822;
    out property <color> gold-300: dark ? #ffeb3b : #d4b400;
    out property <color> gold-400: dark ? #face48 : #d9a520;
    out property <color> gold-500: dark ? #f8cc47 : #d7a31f;

    // text
    out property <color> txt:           dark ? #ffffff : #1a1a1a;
    out property <color> txt-dim:       dark ? #e9e9e9 : #3a3a3a;
    out property <color> txt-border:    dark ? #e69822 : #b8761a;
    out property <color> txt-faint:     dark ? #9ca3af : #6b7280;
    out property <color> txt-secondary: dark ? #e9e9e9 : #3a3a3a;
    out property <color> txt-mid:       dark ? #cccccc : #555555;

    // status (identical in both themes)
    out property <color> status-ok:            #22c55e;
    out property <color> status-ok-dark:       #16a34a;
    out property <color> status-ok-light:      #4ade80;
    out property <color> status-bad:           #ef4444;
    out property <color> status-bad-dark:      #b91c1c;
    out property <color> status-bad-light:     #f87171;
    out property <color> status-warning:       #f59e0b;
    out property <color> status-warning-dark:  #d97706;
    out property <color> status-warning-light: #fbbf24;
    out property <color> status-info:          #8b5cf6;
    out property <color> status-info-dark:     #7c3aed;
    out property <color> status-info-light:    #a78bfa;
    out property <color> status-neutral:       #6b7280;
    out property <color> status-neutral-dark:  #4b5563;
    out property <color> status-neutral-light: #9ca3af;

    // neutrals (identical in both themes)
    out property <color> neutral-dark:   #4b5563;
    out property <color> neutral-medium: #6b7280;

    // modal scrim (not in reference; kept identical both themes)
    out property <color> overlay: #00000099;
}
```

---

## `ui/app-window.slint` — Color Migration

Add the import and replace each hardcoded hex:

```slint
import { Theme } from "theme.slint";
```

| Element (current line) | Current hex | New token |
|---|---|---|
| Top bar `Rectangle.background` | `#2c2c2c` | `Theme.panel-2` |
| Main content `Rectangle.background` | `#1e1e1e` | `Theme.panel` |
| Modal scrim `Rectangle.background` | `#00000099` | `Theme.overlay` |
| Dialog box `Rectangle.background` | `#3c3c3c` | `Theme.panel-3` |
| "Settings" title `Text.color` | `#ffffff` | `Theme.txt` |
| "Resolution" label `Text.color` | `#cccccc` | `Theme.txt-mid` |

### Dark mode toggle (new control in modal)

Add a `Switch` (import from `std-widgets.slint`) inside the modal's `VerticalBox`, between the title and the Resolution label:

```slint
Switch {
    text: "Dark mode";
    checked <=> Theme.dark;
    toggled => { root.theme-changed(Theme.dark); }
}
```

New callback on `AppWindow`:

```slint
callback theme-changed(bool);
```

Behavior: the Switch is two-way bound to `Theme.dark`, so toggling re-themes the app instantly. `toggled` then fires `theme-changed(Theme.dark)` so Rust can persist the new value.

---

## Rust Changes (`src/main.rs`)

### Settings struct (add `dark`)

```rust
#[derive(serde::Serialize, serde::Deserialize)]
struct Settings {
    width: u32,
    height: u32,
    #[serde(default = "default_dark")]
    dark: bool,
}

fn default_dark() -> bool {
    true
}
```

`#[serde(default)]` means an existing `settings.json` without a `dark` field still loads (defaults to `true` = dark).

### load_settings default

```rust
fn load_settings() -> Settings {
    settings_path()
        .and_then(|path| std::fs::read_to_string(path).ok())
        .and_then(|s| serde_json::from_str(&s).ok())
        .unwrap_or(Settings { width: 1600, height: 900, dark: true })
}
```

### save_settings (write whole struct)

Change `save_settings` to take a `&Settings` so any field can be persisted:

```rust
fn save_settings(settings: &Settings) {
    if let Some(path) = settings_path() {
        if let Ok(json) = serde_json::to_string_pretty(settings) {
            let _ = std::fs::write(path, json);
        }
    }
}
```

### main() — startup + callbacks

Both callbacks load the current settings, update only their field, and save — so resolution and theme persist independently without shared mutable state.

```rust
fn main() -> Result<(), slint::PlatformError> {
    let settings = load_settings();

    let app = AppWindow::new()?;

    app.window().set_size(slint::LogicalSize::new(
        settings.width as f32,
        settings.height as f32,
    ));

    let index = PRESETS
        .iter()
        .position(|&(w, h)| w == settings.width && h == settings.height)
        .unwrap_or(2) as i32;
    app.set_resolution_index(index);

    // Apply saved theme to the Theme global
    app.global::<Theme>().set_dark(settings.dark);

    // Resolution Apply — preserves dark
    let app_weak = app.as_weak();
    app.on_apply_resolution(move |index| {
        let (w, h) = PRESETS[index as usize];
        let mut s = load_settings();
        s.width = w;
        s.height = h;
        save_settings(&s);
        if let Some(app) = app_weak.upgrade() {
            app.window().set_size(slint::LogicalSize::new(w as f32, h as f32));
        }
    });

    // Theme toggle — instant save, preserves width/height
    app.on_theme_changed(move |dark| {
        let mut s = load_settings();
        s.dark = dark;
        save_settings(&s);
    });

    app.run()
}
```

`Theme` is accessible from Rust via `app.global::<Theme>()` because it is an `export global`. The generated accessor for the `dark` property is `set_dark`/`get_dark`.

---

## `settings.json` format (extended)

```json
{
  "width": 1600,
  "height": 900,
  "dark": true
}
```

Backward compatible: a file missing `dark` loads with `dark = true`.

---

## Behavior Table

| Action | Result |
|---|---|
| Launch app | Colors applied from `Theme.dark` = saved value (default dark) |
| Open Settings, toggle Dark mode off | App switches to light palette instantly; `settings.json` `dark` set to `false` immediately |
| Close modal without further action | Theme change already saved (instant) |
| Relaunch | App opens in last-used theme |
| Change resolution + Apply | Resolution saved; `dark` field preserved |
| Old `settings.json` (no `dark`) | Loads as dark |

---

## Verification

- `cargo build` passes.
- App launches in dark theme (default).
- Toggling Dark mode flips the entire UI (top bar, content, dialog, text) between dark and light live.
- `settings.json` gains `"dark": false` immediately after toggling off (no Apply needed).
- Relaunch opens in the last-used theme.
- Changing resolution + Apply does not reset the theme; `dark` survives.
- Deleting `settings.json` → app opens dark at 1600×900.
