# UI Refresh — Rounder, Friendlier, Clearer — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give the whole Qt Widgets UI a rounder, friendlier, clearer look — larger consistent corner radii, roomier padding, clear focus/hover/disabled feedback, and a stronger selected-nav cue — without changing colors or layouts.

**Architecture:** The entire change lives in the single centralized stylesheet builder `style_sheet(const Palette&)` in `src/app/ui/theme.cpp`. That one function emits the app-wide Qt stylesheet, so editing its rule blocks restyles every screen at once. No other files change. All color tokens used already exist in the palette and are already wired into the `.replace(...)` chain, so no new tokens or `.replace()` calls are needed.

**Tech Stack:** C++17, Qt6 Widgets (Qt Style Sheets — a CSS-like subset), CMake + Ninja, MSYS2 UCRT64 toolchain.

## Global Constraints

- Do **not** change the color palette (dark grey + gold) — both dark and light variants must keep their current colors. Only radii, padding, and interaction-state borders change.
- Do **not** restructure any layout, page, or widget tree. Stylesheet-only edits.
- Do **not** add a `heading="true"` token — headings are already handled per-widget via `QFont`; a parallel mechanism is out of scope (see spec).
- Only `src/app/ui/theme.cpp` is touched.
- Qt Style Sheets silently ignore malformed rules — a successful compile does **not** prove correctness. Every task must be verified by **running the app and looking**, in **both** dark and light themes.
- Verification is visual (the stylesheet emits a string; there are no unit tests for it). The existing `ctest` suite must still pass (no behavior change), but it does not cover the theme.

**Theme toggle for verification:** launch the app, open **Settings** (top bar) → **Appearance** panel → switch between dark and light to eyeball both. Close and reopen the Settings modal and open the **Camera** modal to check the wizard.

---

### Task 1: Rounder corners + roomier padding

Bumps every corner radius to one consistent, moderately-round scale and opens up the padding. This is the core "make it more round" deliverable — a reviewer can accept/reject it purely on the rounded look.

**Files:**
- Modify: `src/app/ui/theme.cpp` (inside the `R"(...)"` raw string in `style_sheet`)

**Interfaces:**
- Consumes: existing palette tokens `%(panel)`, `%(panel3)`, `%(txt)`, `%(gold)` (already replaced in the `.replace(...)` chain — no change to that chain).
- Produces: no code interface; the visual result other tasks build on.

- [ ] **Step 1: Bump the button radius and padding**

In `style_sheet`, find the `QPushButton` base rule:

```cpp
        QPushButton {
            background: %(panel3); color: %(txt);
            border: none; border-radius: 8px; padding: 6px 14px;
        }
```

Replace it with:

```cpp
        QPushButton {
            background: %(panel3); color: %(txt);
            border: none; border-radius: 12px; padding: 8px 18px;
        }
```

- [ ] **Step 2: Bump the text-input radii and padding**

Find the three input rules:

```cpp
        QLineEdit {
            background: %(panel); color: %(txt);
            border: 1px solid %(panel3); border-radius: 6px; padding: 4px 6px;
            selection-background-color: %(gold);
        }
        QComboBox {
            background: %(panel); color: %(txt);
            border: 1px solid %(panel3); border-radius: 6px; padding: 4px 6px;
        }
        QComboBox QAbstractItemView {
            background: %(panel2); color: %(txt);
            selection-background-color: %(panel3);
        }
        QAbstractSpinBox {
            background: %(panel); color: %(txt);
            border: 1px solid %(panel3); border-radius: 6px; padding: 4px 6px;
            selection-background-color: %(gold);
        }
```

Replace with (radius `6px`→`10px`, padding `4px 6px`→`6px 10px`; the dropdown popup view is left as-is):

```cpp
        QLineEdit {
            background: %(panel); color: %(txt);
            border: 1px solid %(panel3); border-radius: 10px; padding: 6px 10px;
            selection-background-color: %(gold);
        }
        QComboBox {
            background: %(panel); color: %(txt);
            border: 1px solid %(panel3); border-radius: 10px; padding: 6px 10px;
        }
        QComboBox QAbstractItemView {
            background: %(panel2); color: %(txt);
            selection-background-color: %(panel3);
        }
        QAbstractSpinBox {
            background: %(panel); color: %(txt);
            border: 1px solid %(panel3); border-radius: 10px; padding: 6px 10px;
            selection-background-color: %(gold);
        }
```

- [ ] **Step 3: Round the spin-box step buttons to match the frame**

Find the spin-button rule:

```cpp
        QAbstractSpinBox::up-button, QAbstractSpinBox::down-button {
            background: %(panel3); border: none; width: 16px;
        }
```

Replace with (round the outer right corners so the buttons sit flush inside the now-rounder frame):

```cpp
        QAbstractSpinBox::up-button, QAbstractSpinBox::down-button {
            background: %(panel3); border: none; width: 16px;
        }
        QAbstractSpinBox::up-button { border-top-right-radius: 10px; }
        QAbstractSpinBox::down-button { border-bottom-right-radius: 10px; }
```

- [ ] **Step 4: Bump the nav item, card, and dialog-panel radii + nav padding**

Find:

```cpp
        #navList::item {
            color: %(txtDim); padding: 8px 10px; border-radius: 6px; margin: 2px 0px;
        }
```

Replace with (radius `6px`→`10px`, padding `8px 10px`→`9px 12px`):

```cpp
        #navList::item {
            color: %(txtDim); padding: 9px 12px; border-radius: 10px; margin: 2px 0px;
        }
```

Find:

```cpp
        #card { background: %(panel3); border-radius: 10px; }
        #dialogPanel {
            background: %(panel2);
            border: 1px solid %(panel3); border-radius: 12px;
        }
```

Replace with (`10px`→`16px`, `12px`→`18px`):

```cpp
        #card { background: %(panel3); border-radius: 16px; }
        #dialogPanel {
            background: %(panel2);
            border: 1px solid %(panel3); border-radius: 18px;
        }
```

- [ ] **Step 5: Build**

Run: `cmake --build build`
Expected: builds with no errors. (If `build/` is not configured yet: `cmake -S . -B build -G Ninja` first.)

- [ ] **Step 6: Run and verify visually (dark + light)**

Run the app: `./build/src/app/denso` (path varies by generator).
Check, in **both** dark and light (toggle via Settings → Appearance):
- Buttons (top bar, dialog footers), inputs, combos, and spin boxes have visibly rounder corners and a touch more breathing room.
- Cards and the Settings/Camera modal panels have softer, larger corners.
- Nav items (Settings left nav) are rounder.
- Spin-box up/down buttons still line up flush inside the rounded input frame (no square corner poking out).
Capture a screenshot of the main window + Settings modal for the review.

- [ ] **Step 7: Commit**

```bash
git add src/app/ui/theme.cpp
git commit -m "feat(ui): rounder corners + roomier padding across the app"
```

---

### Task 2: Interaction feedback + selected-nav accent

Adds the "friendly + clear" cues: a gold focus ring and hover border on inputs, a dimmed disabled-button state, and a gold left-accent bar on the selected nav item. A reviewer can accept/reject this independently of the rounding.

**Files:**
- Modify: `src/app/ui/theme.cpp` (same `style_sheet` raw string)

**Interfaces:**
- Consumes: palette tokens `%(gold)`, `%(txtFaint)`, `%(panel2)`, `%(panel3)` (all already in the `.replace(...)` chain).
- Produces: no code interface.

- [ ] **Step 1: Add focus + hover borders to the inputs**

Immediately **after** the `QAbstractSpinBox { ... }` rule (as edited in Task 1), add these rules:

```cpp
        QLineEdit:hover, QComboBox:hover, QAbstractSpinBox:hover {
            border: 1px solid %(txtFaint);
        }
        QLineEdit:focus, QComboBox:focus, QAbstractSpinBox:focus {
            border: 1px solid %(gold);
        }
```

- [ ] **Step 2: Add a dimmed disabled state for buttons**

Immediately **after** the existing `QPushButton[flatText="true"]:hover { ... }` rule, add:

```cpp
        QPushButton:disabled {
            background: %(panel2); color: %(txtFaint);
        }
        QPushButton[gold="true"]:disabled {
            background: %(panel3); color: %(txtFaint);
        }
        QPushButton[flatText="true"]:disabled {
            background: transparent; color: %(txtFaint);
        }
```

- [ ] **Step 3: Add the gold left-accent bar to the selected nav item**

Find (post-Task-1) the nav item base + selected rules:

```cpp
        #navList::item {
            color: %(txtDim); padding: 9px 12px; border-radius: 10px; margin: 2px 0px;
        }
        #navList::item:selected { background: %(panel3); color: %(gold); }
        #navList::item:hover { background: %(panel3); }
```

Replace with (reserve a transparent 3px left border on every item so the text does **not** shift when the selected item gains its gold bar; the selected item also goes bold):

```cpp
        #navList::item {
            color: %(txtDim); padding: 9px 12px; border-radius: 10px; margin: 2px 0px;
            border-left: 3px solid transparent;
        }
        #navList::item:selected {
            background: %(panel3); color: %(gold);
            border-left: 3px solid %(gold); font-weight: 600;
        }
        #navList::item:hover { background: %(panel3); }
```

- [ ] **Step 4: Build**

Run: `cmake --build build`
Expected: builds with no errors.

- [ ] **Step 5: Run and verify visually (dark + light)**

Run the app and confirm, in **both** themes:
- Clicking into a text field / combo / spin box shows a **gold** border; hovering an unfocused one shows a subtly lighter border.
- A disabled button (e.g. the wizard **Next**/**Finish** before its step is valid, or **Back** on the first step) looks clearly greyed-out vs. an enabled one.
- The selected Settings nav item shows a **gold left bar + bold text**, and the other items' text stays aligned (no horizontal jump when selection changes).
Capture a screenshot showing a focused input and the selected nav item for the review.

- [ ] **Step 6: Commit**

```bash
git add src/app/ui/theme.cpp
git commit -m "feat(ui): focus/hover/disabled feedback + selected-nav accent"
```

---

## Self-Review

**Spec coverage:**
- §1 Corner-radius scale → Task 1 (steps 1–4): buttons 12, inputs 10, nav 10, card 16, dialog 18, spin buttons rounded. ✓
- §2 Roomier padding → Task 1 (steps 1, 2, 4): button `8px 18px`, inputs `6px 10px`, nav `9px 12px`. ✓
- §3 Interactive feedback → Task 2 (steps 1–2): input focus ring, input hover, disabled buttons. ✓
- §4 Selected-nav accent → Task 2 (step 3). Heading role deliberately dropped per spec. ✓
- "Keep colors, both themes, ctest passes, theme.cpp only" → Global Constraints + every task's dark/light verification. ✓

**Placeholder scan:** no TBD/TODO; every code step shows exact old→new stylesheet text; every command has expected output. ✓

**Type/name consistency:** all tokens (`%(gold)`, `%(txtFaint)`, `%(panel2)`, `%(panel3)`, `%(panel)`, `%(txt)`, `%(txtDim)`) already exist in the current `.replace(...)` chain in `theme.cpp` — no new token is referenced, so nothing to wire. Task 2 edits the exact nav rule text that Task 1 leaves behind (kept consistent). ✓
