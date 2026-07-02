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
  (Achieved with a `border-left` on `#navList::item:selected`, with matching
  left padding on the base item so the text doesn't shift on selection.)

> **Dropped (YAGNI):** an earlier draft proposed a `QLabel[heading="true"]`
> stylesheet token. Review found headings are already distinguished per-widget
> via `QFont` (e.g. the Settings title is +6pt bold, the empty-state title +4pt
> bold, section titles bold with letter-spacing). A parallel stylesheet
> mechanism would be redundant and risk font conflicts, so it is intentionally
> not added.

## Files touched

- `src/app/ui/theme.cpp` — the **entire** refresh: radius scale, padding,
  focus/hover/disabled states, nav accent, heading role. Everything in this
  design is expressible through the one central `style_sheet()` builder.
- `src/app/ui/theme.h` — not expected to change (no new token/helper needed).

**Reviewed and intentionally NOT touched:** the inline `setStyleSheet` calls in
`netcard.cpp`, `wizard_stepper.cpp`, `add_page.cpp`, `configure_page.cpp` were
all found to set **text color / font-weight only** (status lines, error labels,
wizard step emphasis) — they carry no corner radii or padding, so the rounding
and spacing goals do not reach them. They do hardcode hex colors that drift
slightly from the palette tokens, but re-tokenizing colors is a separate concern
outside this "keep colors, make rounder" refresh and is deliberately left alone.

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
