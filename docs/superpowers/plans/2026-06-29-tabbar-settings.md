# Tab Bar + Settings Modal Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Restructure the project to use separate `.slint` files and add a top button bar where clicking "Settings" opens a modal dialog overlay.

**Architecture:** All UI lives in `ui/app-window.slint`. A `build.rs` compiles it at build time. `AppWindow` holds a `settings-open` bool property that drives a conditional overlay. The Rust entry point is construct-and-run only.

**Tech Stack:** Rust, Slint 1.17, slint-build 1.17

## Global Constraints

- Slint version: 1.17.0 (must match `slint-build` version exactly)
- No UI code in `src/main.rs`
- No Rust callbacks needed — all modal open/close logic is pure Slint
- Window title: `"Denso Digital Reader"`, size: 600×400 px

---

### Task 1: Add build infrastructure

**Files:**
- Modify: `Cargo.toml`
- Create: `build.rs`

**Interfaces:**
- Produces: `build.rs` that compiles `ui/app-window.slint`; makes `AppWindow` available via `slint::include_modules!()`

- [ ] **Step 1: Add `slint-build` to `Cargo.toml`**

Replace the entire `Cargo.toml` with:

```toml
[package]
name = "Denso-DigitalReader"
version = "0.1.0"
edition = "2024"

[dependencies]
slint = "1.17.0"

[build-dependencies]
slint-build = "1.17.0"
```

- [ ] **Step 2: Create `build.rs`**

Create `build.rs` at the project root (same level as `Cargo.toml`):

```rust
fn main() {
    slint_build::compile("ui/app-window.slint").unwrap();
}
```

- [ ] **Step 3: Verify build infrastructure is picked up (expected failure)**

Run:
```
cargo build
```

Expected: error about `ui/app-window.slint` not found — confirms `build.rs` runs. Correct failure before Task 2.

- [ ] **Step 4: Commit**

```bash
git add Cargo.toml build.rs
git commit -m "build: add slint-build dependency and build.rs"
```

---

### Task 2: Create UI and update main.rs

**Files:**
- Create: `ui/app-window.slint`
- Modify: `src/main.rs`

**Interfaces:**
- Consumes: `build.rs` from Task 1
- Produces: `AppWindow` with dark top bar, Settings button, and modal overlay driven by `settings-open` bool property

- [ ] **Step 1: Create `ui/app-window.slint`**

Create the `ui/` directory and `ui/app-window.slint`:

```slint
import { Button, HorizontalBox, VerticalBox } from "std-widgets.slint";

export component AppWindow inherits Window {
    title: "Denso Digital Reader";
    width: 600px;
    height: 400px;

    property <bool> settings-open: false;

    VerticalBox {
        padding: 0px;
        spacing: 0px;

        // Top button bar
        Rectangle {
            height: 40px;
            background: #2c2c2c;

            HorizontalBox {
                padding: 4px;
                alignment: start;

                Button {
                    text: "Settings";
                    clicked => { root.settings-open = true; }
                }
            }
        }

        // Main content area (empty for now)
        Rectangle {
            background: #1e1e1e;
        }
    }

    // Modal overlay — shown when settings-open is true
    if root.settings-open : Rectangle {
        width: 100%;
        height: 100%;
        background: #00000099;

        // Centered dialog box
        Rectangle {
            width: 360px;
            height: 240px;
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
                    text: "Settings coming soon";
                    color: #aaaaaa;
                    vertical-alignment: center;
                }

                Rectangle { }

                HorizontalBox {
                    alignment: end;
                    Button {
                        text: "Close";
                        clicked => { root.settings-open = false; }
                    }
                }
            }
        }
    }
}
```

- [ ] **Step 2: Replace `src/main.rs` entirely**

Overwrite `src/main.rs` with:

```rust
slint::include_modules!();

fn main() -> Result<(), slint::PlatformError> {
    let app = AppWindow::new()?;
    app.run()
}
```

- [ ] **Step 3: Build**

Run:
```
cargo build
```

Expected: compiles with no errors.

- [ ] **Step 4: Run and visually verify**

Run:
```
cargo run
```

Expected:
- Window opens titled "Denso Digital Reader" at 600×400 px
- Dark top bar with a "Settings" button on the left
- Dark main content area below
- Clicking "Settings" → modal overlay appears with centered dark dialog, "Settings" title, placeholder text, and "Close" button
- Clicking "Close" → modal dismisses, main window visible again

- [ ] **Step 5: Commit**

```bash
git add ui/app-window.slint src/main.rs
git commit -m "feat: add top button bar with Settings modal dialog"
```
