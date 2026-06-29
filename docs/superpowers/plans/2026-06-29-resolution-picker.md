# Resolution Picker Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the Settings modal placeholder with a resolution ComboBox and Apply button that resizes the window and persists the choice to `settings.json`.

**Architecture:** Slint owns the UI — a `ComboBox` bound to `resolution-index` and an `apply-resolution(int)` callback. Rust owns I/O — reads `settings.json` on startup to set initial size and index, writes it on Apply. The preset table lives only in Rust.

**Tech Stack:** Rust, Slint 1.17, slint-build 1.17, serde 1, serde_json 1

## Global Constraints

- Slint + slint-build versions: exactly `"1.17.0"`
- serde: `{ version = "1", features = ["derive"] }`, serde_json: `"1"`
- ComboBox model (exact strings): `["800 × 600", "1280 × 720", "1600 × 900", "1920 × 1080"]`
- Default resolution: 1600×900 (PRESETS index 2)
- `settings.json` stored next to the compiled executable
- No UI code in `src/main.rs` beyond `slint::include_modules!()`
- All modal open/close logic stays in pure Slint

---

### Task 1: Update Settings modal UI

**Files:**
- Modify: `ui/app-window.slint`

**Interfaces:**
- Produces:
  - `in-out property <int> resolution-index` on `AppWindow` (default `2`)
  - `callback apply-resolution(int)` on `AppWindow`
  - ComboBox bound to `resolution-index`
  - Apply button fires `apply-resolution(root.resolution-index)` then closes modal
  - Close button closes modal without change

- [ ] **Step 1: Replace `ui/app-window.slint` entirely**

Overwrite `ui/app-window.slint` with:

```slint
import { Button, ComboBox, HorizontalBox, VerticalBox } from "std-widgets.slint";

export component AppWindow inherits Window {
    title: "Denso Digital Reader";
    width: 1600px;
    height: 900px;

    property <bool> settings-open: false;
    in-out property <int> resolution-index: 2;
    callback apply-resolution(int);

    VerticalBox {
        padding: 0px;
        spacing: 0px;

        // Top button bar
        Rectangle {
            height: 40px;
            background: #2c2c2c;

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
            background: #1e1e1e;
        }
    }

    // Modal overlay
    if root.settings-open : Rectangle {
        width: 100%;
        height: 100%;
        background: #00000099;

        Rectangle {
            x: (root.width - self.width) / 2;
            y: (root.height - self.height) / 2;
            background: #3c3c3c;
            border-radius: 8px;

            VerticalBox {
                padding: 20px;
                spacing: 12px;

                Text {
                    text: "Settings";
                    font-size: 18px;
                    color: #ffffff;
                }

                Text {
                    text: "Resolution";
                    color: #cccccc;
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

- [ ] **Step 2: Verify the Slint UI compiles**

Run:
```
cargo build
```

Expected: `Finished dev profile` — no errors. The `apply-resolution` callback will be unimplemented in Rust at this point (Slint generates a no-op stub), but the build must succeed.

- [ ] **Step 3: Commit**

```bash
git add ui/app-window.slint
git commit -m "feat: add resolution ComboBox and Apply button to Settings modal"
```

---

### Task 2: Add serde dependencies + Rust settings logic

**Files:**
- Modify: `Cargo.toml`
- Modify: `src/main.rs`

**Interfaces:**
- Consumes from Task 1:
  - `app.set_resolution_index(i32)` — sets ComboBox selection
  - `app.on_apply_resolution(|i32| { ... })` — callback fired on Apply

- [ ] **Step 1: Add serde dependencies to `Cargo.toml`**

Replace `Cargo.toml` with:

```toml
[package]
name = "Denso-DigitalReader"
version = "0.1.0"
edition = "2024"

[dependencies]
slint = "1.17.0"
serde = { version = "1", features = ["derive"] }
serde_json = "1"

[build-dependencies]
slint-build = "1.17.0"
```

- [ ] **Step 2: Replace `src/main.rs` entirely**

Overwrite `src/main.rs` with:

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
}

fn settings_path() -> std::path::PathBuf {
    std::env::current_exe()
        .unwrap()
        .parent()
        .unwrap()
        .join("settings.json")
}

fn load_settings() -> Settings {
    std::fs::read_to_string(settings_path())
        .ok()
        .and_then(|s| serde_json::from_str(&s).ok())
        .unwrap_or(Settings { width: 1600, height: 900 })
}

fn save_settings(width: u32, height: u32) {
    let s = Settings { width, height };
    if let Ok(json) = serde_json::to_string_pretty(&s) {
        let _ = std::fs::write(settings_path(), json);
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

- [ ] **Step 3: Build**

Run:
```
cargo build
```

Expected: `Finished dev profile` — no errors or warnings (serde_json may download on first run).

- [ ] **Step 4: Run and verify — first launch (no settings.json)**

Delete any existing `settings.json` next to the executable, then run:
```
cargo run
```

Expected:
- Window opens at 1600×900
- Click Settings → ComboBox shows "1600 × 900" selected (index 2)
- Click Close → modal dismisses, no change

- [ ] **Step 5: Verify Apply resizes and persists**

With the app running:
1. Click Settings
2. Select "800 × 600" from the ComboBox
3. Click Apply

Expected:
- Window immediately shrinks to 800×600
- Modal closes
- `settings.json` appears next to the executable with content:
```json
{
  "width": 800,
  "height": 600
}
```

- [ ] **Step 6: Verify persistence across restart**

Close the app. Run again:
```
cargo run
```

Expected:
- Window opens at 800×600 (not 1600×900)
- Click Settings → ComboBox shows "800 × 600" selected (index 0)

- [ ] **Step 7: Commit**

```bash
git add Cargo.toml src/main.rs
git commit -m "feat: persist and restore resolution via settings.json"
```
