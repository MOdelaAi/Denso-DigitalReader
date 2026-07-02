# UI Refresh — Rounder, Friendlier, Clearer

**Date:** 2026-07-02
**Status:** Approved (design)

## Goal

An app-wide visual refresh of the Qt Widgets UI that makes it feel **rounder,
friendlier, and clearer**, without changing the color theme (dark grey + gold)
or restructuring any screen layouts. The device is operated by **mouse +
keyboard**, so control sizes stay roughly as-is — this is polish, not a
touch-target overhaul.

Driven almost entirely through the single centralized stylesheet builder
(`src/app/ui/theme.cpp`), plus a few inline styles brought into the same scale.

## Non-goals

- No color-theme change (keep the dark grey + gold palette, both variants).
- No layout/flow restructuring (wizard step order, page structure, nav order
  all stay). Guidance improvements are purely visual cues, not re-flows.
- No enlarged touch targets — this is a mouse/keyboard device.
- No new widgets or screens.

## Design

### 1. Consistent corner-radius scale

Replace the ad-hoc radii with one consistent, moderately-rounded scale applied
across the whole widget tree:

| Element | Current | New |
|---|---|---|
| `QPushButton` (all variants) | 8px | **12px** |
| `QLineEdit` / `QComboBox` / `QAbstractSpinBox` | 6px | **10px** |
| `#navList::item` | 6px | **10px** |
| `#card` | 10px | **16px** |
| `#dialogPanel` | 12px | **18px** |
| Spin up/down buttons | square | rounded outer corners to match the frame |

### 2. Roomier padding

| Element | Current | New |
|---|---|---|
| `QPushButton` | `6px 14px` | **`8px 18px`** |
| `QLineEdit` / `QComboBox` / `QAbstractSpinBox` | `4px 6px` | **`6px 10px`** |
| `#navList::item` | `8px 10px` | **`9px 12px`** |

### 3. Interactive feedback (friendliness)

- **Focus ring:** `QLineEdit`, `QComboBox`, `QAbstractSpinBox` get a
  gold `1px` border on `:focus` so the active field is unmistakable (there is
  no focus affordance today).
- **Input hover:** a slightly lighter border on `:hover` for the same inputs.
- **Disabled buttons:** a dimmed `:disabled` style for `QPushButton` (today a
  disabled button is visually indistinguishable from an enabled one).

### 4. Clarity / hierarchy (light, theme-only)

- **Selected nav item:** in addition to the gold text, add a thin gold
  left-edge accent bar and bold weight, so the current page reads at a glance.
  (Achieved with a `border-left` on `#navList::item:selected`.)
- **Heading role:** add an optional `QLabel[heading="true"]` token (larger,
  bolder). It only affects labels that opt in by setting the property, so it is
  additive and safe; wiring specific headings to it is left to the
  implementation plan where it clearly helps (e.g. page titles).

## Files touched

- `src/app/ui/theme.cpp` — the bulk: radius scale, padding, focus/hover/disabled
  states, nav accent, heading role.
- `src/app/ui/theme.h` — only if a new token/helper is needed (likely not).
- Inline-style spots brought into the same radius/padding scale so nothing looks
  out of step:
  - `src/app/ui/settings/netcard.cpp` (5 inline styles)
  - `src/app/ui/camera/dialog/wizard_stepper.cpp` (2)
  - `src/app/ui/camera/dialog/add_page.cpp` (1)
  - `src/app/ui/camera/dialog/configure_page.cpp` (1)

## Testing / verification

- The theme is not unit-tested (it produces a stylesheet string); correctness is
  visual. Verify by **building and running the app** and viewing the main
  window, settings modal (each panel), and the camera wizard (each step),
  confirming corners, spacing, focus rings, disabled buttons, and the nav accent
  render as intended in **both dark and light** themes.
- Existing `ctest` suite must still pass (no core/behavior change expected).

## Risks / notes

- Very large radii on small inputs look bad; the chosen scale is deliberately
  moderate. Radii live in one file, so post-review tuning is a one-line change.
- Light theme uses the same radii/padding; both variants must be eyeballed since
  contrast differs.
- `border-left` accent on a selected nav item shifts its text ~3px; compensate
  with matching left padding so text doesn't jump on selection.
