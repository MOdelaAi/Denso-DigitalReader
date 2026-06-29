# Tab Bar + Settings Tab — Design Spec

**Date:** 2026-06-29  
**Project:** Denso-DigitalReader (Slint 1.17 + Rust)  
**Status:** Approved

---

## Goal

Add a tab bar at the top of the main window with a single "Settings" placeholder tab. Remove the existing button/status-text placeholder UI.

## Scope

- Add `TabWidget` with one `Tab { title: "Settings" }` containing a placeholder text body.
- Remove `AppWindow`'s `status-text` property and `button-clicked` callback.
- Simplify `main()` to only construct and run the window (no callbacks).

## Out of Scope

- Settings tab content (deferred).
- Additional tabs (deferred).
- Custom tab bar styling.

---

## Component Design

### `AppWindow` (in `src/main.rs`, `slint::slint! {}` macro)

**Before:**
```
export component AppWindow inherits Window {
    in-out property <string> status-text;
    callback button-clicked();
    // ... button + text content
}
```

**After:**
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

### `main()` (Rust)

**Before:** constructs window, clones weak handle, wires `on_button_clicked` callback, calls `run()`.

**After:** constructs window, calls `run()`. No callbacks needed.

```rust
fn main() -> Result<(), slint::PlatformError> {
    let app = AppWindow::new()?;
    app.run()
}
```

---

## Adding More Tabs Later

Each new tab is one additional `Tab { title: "..." }` block inside `TabWidget`. No structural changes required.

---

## Verification

- `cargo build` passes with no errors.
- Window opens with a tab bar showing "Settings".
- Settings tab body shows placeholder text.
- No stale properties or callbacks in generated Rust API.
