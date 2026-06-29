# Resolution Picker — Design Spec

**Date:** 2026-06-29  
**Project:** Denso-DigitalReader (Slint 1.17 + Rust)  
**Status:** Approved

---

## Goal

Add a resolution picker to the Settings modal. The user selects from four preset window sizes, clicks Apply, and the window resizes immediately. The chosen resolution persists to `settings.json` and is restored on next launch.

## Scope

- Replace the "Settings coming soon" placeholder in the Settings modal with a resolution `ComboBox` + Apply/Close buttons.
- Add `resolution-index` property and `apply-resolution(int)` callback to `AppWindow`.
- Add Rust startup logic: read `settings.json`, set window size and ComboBox index.
- Wire `on_apply_resolution` in Rust: resize window + write `settings.json`.
- Add `serde` and `serde_json` dependencies to `Cargo.toml`.

## Out of Scope

- Other settings categories (deferred).
- Custom resolution input (deferred).
- Config file migration/versioning.

---

## Preset Table (single source of truth in Rust)

```rust
const PRESETS: [(u32, u32); 4] = [
    (800, 600),
    (1280, 720),
    (1600, 900),
    (1920, 1080),
];
```

Index 2 (1600×900) is the default if `settings.json` is absent or malformed.

---

## File Layout

```
Denso-DigitalReader/
├── Cargo.toml             ← add serde, serde_json dependencies
├── settings.json          ← written at runtime, next to executable
├── src/
│   └── main.rs            ← startup loading + callback wiring
└── ui/
    └── app-window.slint   ← Settings modal body replaced with real UI
```

---

## UI Design

### `ui/app-window.slint` — Settings modal body

Replace the placeholder `Text { text: "Settings coming soon"; }` and empty spacer `Rectangle {}` with:

```slint
// New properties on AppWindow
in-out property <int> resolution-index: 2;
callback apply-resolution(int);  // passes selected index to Rust

// Inside the modal's VerticalBox, replace placeholder content:
Text {
    text: "Resolution";
    color: #cccccc;
}

ComboBox {
    model: ["800 × 600", "1280 × 720", "1600 × 900", "1920 × 1080"];
    current-index <=> root.resolution-index;
}
```

Close and Apply buttons replace the existing Close-only row:

```slint
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
```

---

## Rust Design

### `src/main.rs`

**Config struct:**
```rust
#[derive(serde::Serialize, serde::Deserialize)]
struct Settings {
    width: u32,
    height: u32,
}
```

**Helper — settings.json path (next to executable):**
```rust
fn settings_path() -> std::path::PathBuf {
    std::env::current_exe()
        .unwrap()
        .parent()
        .unwrap()
        .join("settings.json")
}
```

**Helper — load settings (returns default on any error):**
```rust
fn load_settings() -> Settings {
    std::fs::read_to_string(settings_path())
        .ok()
        .and_then(|s| serde_json::from_str(&s).ok())
        .unwrap_or(Settings { width: 1600, height: 900 })
}
```

**Helper — save settings:**
```rust
fn save_settings(width: u32, height: u32) {
    let s = Settings { width, height };
    if let Ok(json) = serde_json::to_string_pretty(&s) {
        let _ = std::fs::write(settings_path(), json);
    }
}
```

**`main()` startup sequence:**
```rust
slint::include_modules!();

const PRESETS: [(u32, u32); 4] = [
    (800, 600), (1280, 720), (1600, 900), (1920, 1080),
];

fn main() -> Result<(), slint::PlatformError> {
    let settings = load_settings();

    let app = AppWindow::new()?;

    // Apply saved resolution
    app.window().set_size(slint::LogicalSize::new(
        settings.width as f32,
        settings.height as f32,
    ));

    // Set ComboBox to matching preset (default index 2 if not found)
    let index = PRESETS
        .iter()
        .position(|&(w, h)| w == settings.width && h == settings.height)
        .unwrap_or(2) as i32;
    app.set_resolution_index(index);

    // Wire Apply callback
    let app_weak = app.as_weak();
    app.on_apply_resolution(move |index| {
        let (w, h) = PRESETS[index as usize];
        save_settings(w, h);
        if let Some(app) = app_weak.upgrade() {
            app.window().set_size(slint::LogicalSize::new(w as f32, h as f32));
        }
    });

    app.run()
}
```

### `Cargo.toml` additions

```toml
[dependencies]
slint = "1.17.0"
serde = { version = "1", features = ["derive"] }
serde_json = "1"
```

---

## `settings.json` format

```json
{
  "width": 1600,
  "height": 900
}
```

Stored next to the compiled executable. Created/overwritten on every Apply. Missing or malformed → silently defaults to 1600×900.

---

## Behavior Table

| Action | Result |
|--------|--------|
| Open Settings | ComboBox shows current resolution |
| Select different preset | ComboBox updates, no resize yet |
| Click Apply | Window resizes immediately, settings.json written, modal closes |
| Click Close | Modal closes, no change |
| Relaunch app | Window opens at last saved resolution |
| settings.json missing | Window opens at 1600×900 (default) |

---

## Verification

- `cargo build` passes.
- App opens at default 1600×900 on first run.
- Opening Settings shows ComboBox pre-selected at current resolution.
- Selecting 800×600 + Apply → window shrinks, modal closes.
- Relaunch → window opens at 800×600.
- Delete `settings.json`, relaunch → window opens at 1600×900.
