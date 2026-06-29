# Tab Bar + Settings Placeholder Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Restructure the project to use separate `.slint` files (best practice) and add a tab bar with a single "Settings" placeholder tab.

**Architecture:** Move all UI markup from the inline `slint::slint! {}` macro into `ui/app-window.slint`. A new `build.rs` compiles that file at build time and exposes `AppWindow` to Rust via `slint::include_modules!()`. The Rust entry point shrinks to construct-and-run only.

**Tech Stack:** Rust, Slint 1.17, slint-build 1.17

## Global Constraints

- Slint version: 1.17.0 (must match `slint-build` version exactly)
- No UI code in `src/main.rs`
- No callbacks or properties on `AppWindow` until a tab needs them
- Window title: `"Denso Digital Reader"`, size: 600×400 px

---

### Task 1: Add build infrastructure

**Files:**
- Modify: `Cargo.toml`
- Create: `build.rs`

**Interfaces:**
- Produces: `build.rs` that compiles `ui/app-window.slint` at build time; makes `AppWindow` struct available via `slint::include_modules!()`

- [ ] **Step 1: Add `slint-build` to `Cargo.toml`**

Open `Cargo.toml` and add a `[build-dependencies]` section so it reads:

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

- [ ] **Step 3: Verify build infrastructure compiles (will fail on missing .slint — that's expected)**

Run:
```
cargo build
```

Expected: error about `ui/app-window.slint` not found — confirms `build.rs` is picked up and running. This is the correct failure mode before Task 2.

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
- Consumes: `build.rs` from Task 1 (compiles `ui/app-window.slint`)
- Produces: `AppWindow` component with a top tab bar; "Settings" tab shows placeholder text

- [ ] **Step 1: Create `ui/app-window.slint`**

Create the directory `ui/` and the file `ui/app-window.slint`:

```slint
import { TabWidget, Tab, VerticalBox } from "std-widgets.slint";

export component AppWindow inherits Window {
    title: "Denso Digital Reader";
    width: 600px;
    height: 400px;

    TabWidget {
        Tab {
            title: "Settings";
            VerticalBox {
                alignment: center;
                Text {
                    text: "Settings coming soon";
                    horizontal-alignment: center;
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

`slint::include_modules!()` expands to the generated Rust code produced by `build.rs` — it makes `AppWindow` available as a plain Rust struct.

- [ ] **Step 3: Build and verify**

Run:
```
cargo build
```

Expected: compiles with no errors or warnings.

- [ ] **Step 4: Run and visually verify**

Run:
```
cargo run
```

Expected:
- Window opens titled "Denso Digital Reader" at 600×400 px
- Tab bar appears at the top with a "Settings" tab selected
- Tab body shows "Settings coming soon" centered

- [ ] **Step 5: Commit**

```bash
git add ui/app-window.slint src/main.rs
git commit -m "feat: add TabWidget with Settings placeholder tab using .slint file"
```
