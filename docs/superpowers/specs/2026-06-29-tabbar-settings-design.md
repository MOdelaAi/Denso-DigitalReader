# Tab Bar + Settings Tab — Design Spec

**Date:** 2026-06-29  
**Project:** Denso-DigitalReader (Slint 1.17 + Rust)  
**Status:** Approved

---

## Goal

Add a tab bar at the top of the main window with a single "Settings" placeholder tab, using the standard Slint project structure (separate `.slint` files compiled via `build.rs`).

## Scope

- Add `build.rs` to compile `.slint` files.
- Create `ui/app-window.slint` with `TabWidget` + one `Tab { title: "Settings" }` placeholder.
- Remove all UI code from `src/main.rs`; replace with the generated `include!` macro.
- Remove the old `status-text` property, `button-clicked` callback, and all Rust callback wiring.

## Out of Scope

- Settings tab content (deferred).
- Additional tabs (deferred).
- Custom tab bar styling.

---

## File Layout

```
Denso-DigitalReader/
├── build.rs               ← new: compiles ui/*.slint
├── Cargo.toml             ← add slint-build dev-dependency
├── src/
│   └── main.rs            ← thin entry point only
└── ui/
    └── app-window.slint   ← new: AppWindow component
```

---

## Component Design

### `ui/app-window.slint`

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

### `build.rs`

```rust
fn main() {
    slint_build::compile("ui/app-window.slint").unwrap();
}
```

### `src/main.rs`

```rust
slint::include_modules!();

fn main() -> Result<(), slint::PlatformError> {
    let app = AppWindow::new()?;
    app.run()
}
```

### `Cargo.toml` changes

Add `slint-build` as a build dependency:

```toml
[build-dependencies]
slint-build = "1.17.0"
```

---

## Adding More Tabs Later

Each new tab is one additional `Tab { title: "..." }` block inside `TabWidget` in `app-window.slint`. Complex tab content can be split into separate `.slint` files and imported.

---

## Verification

- `cargo build` passes with no errors.
- Window opens with a tab bar showing "Settings".
- Settings tab body shows placeholder text.
- No UI code remains in `src/main.rs`.
