# Color Theme Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a centralized Slint `Theme` global with light/dark switching, migrate hardcoded colors, add a Dark mode toggle in the Settings modal, and persist the theme choice instantly to `settings.json`.

**Architecture:** A new `ui/theme.slint` exports a `global Theme` whose colors are `dark ? <dark> : <light>` ternaries on an `in property <bool> dark`. `app-window.slint` imports it, references tokens, and adds a `Switch` two-way bound to `Theme.dark` that fires `theme-changed(bool)`. Rust extends `Settings` with `dark`, applies it to the Theme global at startup, and saves it instantly on toggle.

**Tech Stack:** Rust, Slint 1.17, slint-build 1.17, serde 1, serde_json 1

## Global Constraints

- Slint + slint-build: exactly `"1.17.0"`; serde `{ version = "1", features = ["derive"] }`; serde_json `"1"`
- Dark values verbatim from `colortheme-ref.txt`; status + neutral colors identical in both themes
- `Theme.dark` default `true`
- `settings.json` is `{ width, height, dark }`; missing `dark` defaults to `true` (`#[serde(default)]`)
- Theme toggle saves instantly (independent of resolution Apply); both callbacks preserve the other's field
- `settings.json` stored next to the executable

---

### Task 1: Create Theme global, migrate colors, add toggle

**Files:**
- Create: `ui/theme.slint`
- Modify: `ui/app-window.slint`

**Interfaces:**
- Produces:
  - `export global Theme` with `in property <bool> dark` (default `true`) and all palette colors as `out property <color>`
  - `callback theme-changed(bool)` on `AppWindow`
  - A `Switch` in the modal, `checked <=> Theme.dark`, firing `theme-changed(Theme.dark)` on toggle
  - From Rust (Task 2): `Theme` reachable via `app.global::<Theme>()`, property accessor `set_dark(bool)`; callback handler `on_theme_changed(impl Fn(bool))`

- [ ] **Step 1: Create `ui/theme.slint`**

Create the file `ui/theme.slint` with exactly this content:

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

    // modal scrim (not in reference; identical both themes)
    out property <color> overlay: #00000099;
}
```

- [ ] **Step 2: Rewrite `ui/app-window.slint`**

Overwrite `ui/app-window.slint` with this content. It adds the `Theme` and `Switch` imports, the `theme-changed` callback, the Dark mode switch, and replaces all six hardcoded hex colors with tokens:

```slint
import { Button, ComboBox, HorizontalBox, Switch, VerticalBox } from "std-widgets.slint";
import { Theme } from "theme.slint";

export component AppWindow inherits Window {
    title: "Denso Digital Reader";
    preferred-width: 1600px;
    preferred-height: 900px;

    property <bool> settings-open: false;
    in-out property <int> resolution-index: 2;
    callback apply-resolution(int);
    callback theme-changed(bool);

    VerticalBox {
        padding: 0px;
        spacing: 0px;

        // Top button bar
        Rectangle {
            height: 40px;
            background: Theme.panel-2;

            HorizontalBox {
                padding: 4px;
                alignment: end;

                Button {
                    text: "Settings";
                    clicked => { root.settings-open = true; }
                }
            }
        }

        // Main content area
        Rectangle {
            background: Theme.panel;
        }
    }

    // Modal overlay
    if root.settings-open : Rectangle {
        width: 100%;
        height: 100%;
        background: Theme.overlay;

        Rectangle {
            x: (root.width - self.width) / 2;
            y: (root.height - self.height) / 2;
            background: Theme.panel-3;
            border-radius: 8px;

            VerticalBox {
                padding: 20px;
                spacing: 12px;

                Text {
                    text: "Settings";
                    font-size: 18px;
                    color: Theme.txt;
                }

                Switch {
                    text: "Dark mode";
                    checked <=> Theme.dark;
                    toggled => { root.theme-changed(Theme.dark); }
                }

                Text {
                    text: "Resolution";
                    color: Theme.txt-mid;
                }

                ComboBox {
                    model: ["800 × 600", "1280 × 720", "1600 × 900", "1920 × 1080"];
                    current-index <=> root.resolution-index;
                }

                HorizontalBox {
                    alignment: end;
                    spacing: 8px;

                    Button {
                        text: "Close";
                        clicked => { root.settings-open = false; }
                    }

                    Button {
                        text: "Apply";
                        clicked => {
                            root.apply-resolution(root.resolution-index);
                            root.settings-open = false;
                        }
                    }
                }
            }
        }
    }
}
```

- [ ] **Step 3: Build to verify Slint compiles**

Run:
```
cargo build
```

Expected: `Finished dev profile` — no errors. The new `theme-changed` callback is an unimplemented no-op stub in Rust at this point (Slint generates the stub), which compiles fine. There may be a warning about an unused `Theme` global from Rust's side — that is resolved in Task 2.

- [ ] **Step 4: Commit**

```bash
git add ui/theme.slint ui/app-window.slint
git commit -m "feat: add Theme global, migrate colors, add Dark mode toggle"
```

---

### Task 2: Persist theme — Settings.dark, startup apply, instant save

**Files:**
- Modify: `src/main.rs`

**Interfaces:**
- Consumes from Task 1:
  - `app.global::<Theme>().set_dark(bool)` — apply theme to the global
  - `app.on_theme_changed(impl Fn(bool) + 'static)` — fires on Switch toggle
- Note: `Theme` is brought into Rust scope by `slint::include_modules!()` (already present at top of `src/main.rs`)

- [ ] **Step 1: Replace `src/main.rs` entirely**

Overwrite `src/main.rs` with this content. Changes vs. current: `Settings` gains `dark` with a serde default, `save_settings` takes `&Settings`, both callbacks load-modify-save to preserve the other field, and the theme is applied at startup.

```rust
slint::include_modules!();

const PRESETS: [(u32, u32); 4] = [
    (800, 600),
    (1280, 720),
    (1600, 900),
    (1920, 1080),
];

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

fn settings_path() -> Option<std::path::PathBuf> {
    std::env::current_exe()
        .ok()?
        .parent()
        .map(|p| p.join("settings.json"))
}

fn load_settings() -> Settings {
    settings_path()
        .and_then(|path| std::fs::read_to_string(path).ok())
        .and_then(|s| serde_json::from_str(&s).ok())
        .unwrap_or(Settings { width: 1600, height: 900, dark: true })
}

fn save_settings(settings: &Settings) {
    if let Some(path) = settings_path() {
        if let Ok(json) = serde_json::to_string_pretty(settings) {
            let _ = std::fs::write(path, json);
        }
    }
}

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

    app.global::<Theme>().set_dark(settings.dark);

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

    app.on_theme_changed(move |dark| {
        let mut s = load_settings();
        s.dark = dark;
        save_settings(&s);
    });

    app.run()
}
```

- [ ] **Step 2: Build**

Run:
```
cargo build
```

Expected: `Finished dev profile` — no errors, no warnings.

- [ ] **Step 3: Run and verify theme toggle + persistence**

Delete any existing `settings.json` next to the executable, then run:
```
cargo run
```

Expected:
- App launches in **dark** theme (top bar dark grey, content near-black, white title text)
- Open Settings → toggle **Dark mode** off → entire UI flips to light palette instantly (light grey panels, dark text)
- `target/debug/settings.json` now contains `"dark": false` (along with width/height) — written immediately, without clicking Apply

- [ ] **Step 4: Verify persistence + independence across restart**

Close the app. Run again:
```
cargo run
```

Expected:
- App opens in **light** theme (the saved value)
- Open Settings, change resolution to "800 × 600", click Apply → window resizes; `settings.json` still has `"dark": false` (resolution Apply preserved the theme)

- [ ] **Step 5: Commit**

```bash
git add src/main.rs
git commit -m "feat: persist theme via settings.json and apply at startup"
```
