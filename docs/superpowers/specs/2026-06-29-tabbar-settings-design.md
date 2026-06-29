# Tab Bar + Settings Modal — Design Spec

**Date:** 2026-06-29  
**Project:** Denso-DigitalReader (Slint 1.17 + Rust)  
**Status:** Approved

---

## Goal

Add a button bar at the top of the main window. Clicking "Settings" opens a modal dialog overlay. Close button dismisses it. Uses standard Slint project structure (separate `.slint` files compiled via `build.rs`).

## Scope

- Add `build.rs` to compile `.slint` files.
- Create `ui/app-window.slint` with:
  - A top `HorizontalBox` button bar containing a "Settings" button.
  - A `bool` property `settings-open` on `AppWindow`, default `false`.
  - A full-screen semi-transparent overlay `Rectangle` visible when `settings-open` is true.
  - A centered dialog box inside the overlay with title, placeholder text, and a Close button.
- Remove all UI code from `src/main.rs`; replace with `slint::include_modules!()`.
- Remove the old `status-text` property, `button-clicked` callback, and all Rust callback wiring.

## Out of Scope

- Settings content (deferred).
- Additional top-bar buttons (deferred).
- Custom styling beyond basic layout.

---

## File Layout

```
Denso-DigitalReader/
├── build.rs               ← new: compiles ui/*.slint
├── Cargo.toml             ← add slint-build dev-dependency
├── src/
│   └── main.rs            ← thin entry point only
└── ui/
    └── app-window.slint   ← new: AppWindow component with modal
```

---

## Component Design

### `ui/app-window.slint`

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
        background: #00000099;  // semi-transparent black

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

                Rectangle { } // spacer

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

### `Cargo.toml` additions

```toml
[build-dependencies]
slint-build = "1.17.0"
```

---

## Behavior

| Action | Result |
|--------|--------|
| Click "Settings" button | Modal overlay appears, dialog centered |
| Click "Close" inside dialog | Modal dismisses, main window visible |
| Clicking outside dialog (on overlay) | No action (overlay is not dismissible) |

---

## Adding More Buttons/Modals Later

Add another `Button` to the top `HorizontalBox` and another `property <bool>` + `if` overlay block. Each modal is fully independent.

---

## Verification

- `cargo build` passes with no errors.
- Window opens with a dark top bar containing a "Settings" button.
- Clicking "Settings" shows the modal overlay with dialog.
- Clicking "Close" dismisses the modal.
- No UI code remains in `src/main.rs`.
