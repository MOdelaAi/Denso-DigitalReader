# Game-style Display Settings Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Rework Settings → Display into a desktop-game-style panel: a Windowed/Borderless/Fullscreen mode selector, a "Window size" picker, display changes batched behind Apply with a 15s confirm/revert countdown, theme stays live.

**Architecture:** Pure, Qt-free decision logic (mode enum, transition planner, preset filtering, semantic state) lives in `src/core/settings/` and is TDD-unit-tested. The `SettingsDialog` stages selections and emits one batched signal; `MainWindow` executes the plan against real `QWidget` calls, snapshots semantic state, and runs a confirm/revert transaction via a small display-specific `QDialog`. Persistence is a new `display_mode` key with a transactional dual-write of the legacy `fullscreen` key.

**Tech Stack:** C++17, Qt6 Widgets, CMake + Ninja, Catch2 v3, SQLite (QSQLITE). Dev toolchain MSYS2 UCRT64.

## Global Constraints

- Build/test on MSYS2 UCRT64: `export PATH=/c/msys64/ucrt64/bin:$PATH` then `cmake --build build` and `ctest --test-dir build`.
- `denso_core` links `Qt6::Core`/`Sql` ONLY — no Widgets. All new pure logic (`display.{h,cpp}`, `fitting_presets`) stays Qt-free (plain `<cstdint>`/`<string>`/`<vector>`), no Qt types.
- Persistence is the key/value `settings` table — **no SQL schema migration**, `SCHEMA_VERSION` stays 11.
- "Window size" resizes the app window only — NEVER a GPU/OS display-mode switch.
- Theme (dark mode) stays live/instant — it is NOT part of the confirm/revert transaction.
- Fullscreen escape stays F11/Esc-only; the top bar (Settings button) is visible in every mode (do not hide it).
- CMake lists sources explicitly: add `settings/display.cpp` to `src/core/CMakeLists.txt` and `test_display.cpp` to `tests/CMakeLists.txt`.
- Follow existing style: `namespace denso::settings` / `denso::ui`, `#pragma once`, includes read from each target's own root (e.g. `settings/display.h`).

---

## File Structure

| File | Responsibility |
|---|---|
| `src/core/settings/display.h` / `.cpp` (new) | Pure display domain: `DisplayMode` enum, string ⇄ enum, `DisplayState`, `display_changed`, `PlatformCaps`, `TransitionPlan`, `plan_transition`. |
| `src/core/settings/settings.h` / `.cpp` | `Settings::mode` (replaces `bool fullscreen`); `fitting_presets` / `largest_fitting_preset` next to `PRESETS`/`preset_index`. |
| `src/core/settings/repo.cpp` | Load prefers `display_mode` (legacy `fullscreen` fallback, unknown→Windowed); transactional dual-write; `import_legacy` maps to `mode`. |
| `src/app/ui/settings/settings_dialog.h` / `.cpp` | Display-mode combo + "Window size" combo (item-data ids, filtered, Windowed-only); staged; emits `apply_display_requested(int mode, int width, int height)`. |
| `src/app/ui/settings/display_confirm_dialog.h` / `.cpp` (new) | Top-level `QDialog` (StaysOnTop, app-modal) with 15s countdown + Keep/Revert. |
| `src/app/ui/mainwindow.h` / `.cpp` | `apply_display_mode()` (canonical sequence), semantic snapshot/restore, confirm transaction + interference routing, startup, platform caps, F11/Esc → mode, fix stale comment. |
| `tests/test_display.cpp` (new) | Unit tests for the pure display logic. |
| `tests/test_settings_repo.cpp` | Extend: precedence, dual-write, legacy, old-build downgrade, import. |

---

## Task 1: DisplayMode enum + string conversion (pure)

**Files:**
- Create: `src/core/settings/display.h`, `src/core/settings/display.cpp`
- Modify: `src/core/CMakeLists.txt` (add `settings/display.cpp`), `tests/CMakeLists.txt` (add `test_display.cpp`)
- Test: `tests/test_display.cpp` (new)

**Interfaces:**
- Produces: `enum class denso::settings::DisplayMode { Windowed, Borderless, Fullscreen }`; `const char* to_string(DisplayMode)`; `DisplayMode parse_display_mode(const std::string&)`.

- [ ] **Step 1: Write the failing test** — create `tests/test_display.cpp`:

```cpp
#include "settings/display.h"

#include <catch2/catch_test_macros.hpp>

using namespace denso::settings;

TEST_CASE("DisplayMode round-trips through string") {
    CHECK(std::string(to_string(DisplayMode::Windowed)) == "windowed");
    CHECK(std::string(to_string(DisplayMode::Borderless)) == "borderless");
    CHECK(std::string(to_string(DisplayMode::Fullscreen)) == "fullscreen");
    CHECK(parse_display_mode("windowed") == DisplayMode::Windowed);
    CHECK(parse_display_mode("borderless") == DisplayMode::Borderless);
    CHECK(parse_display_mode("fullscreen") == DisplayMode::Fullscreen);
}

TEST_CASE("parse_display_mode falls back to Windowed on unknown/corrupt") {
    CHECK(parse_display_mode("") == DisplayMode::Windowed);
    CHECK(parse_display_mode("garbage") == DisplayMode::Windowed);
    CHECK(parse_display_mode("FULLSCREEN") == DisplayMode::Windowed);  // case-sensitive
}
```

- [ ] **Step 2: Create the header** — `src/core/settings/display.h`:

```cpp
// Pure display-settings domain: the window display mode plus the transition
// logic the UI/window drive it with. Qt-free (std types only) so it stays in
// denso_core and is unit-tested off-device.
#pragma once

#include <cstdint>
#include <string>

namespace denso::settings {

enum class DisplayMode { Windowed, Borderless, Fullscreen };

/// Stable persisted token for a mode ("windowed"|"borderless"|"fullscreen").
const char* to_string(DisplayMode mode);

/// Parse a persisted token; any unknown/corrupt value is Windowed (never strand
/// startup in an unreachable mode).
DisplayMode parse_display_mode(const std::string& s);

} // namespace denso::settings
```

- [ ] **Step 3: Create the source** — `src/core/settings/display.cpp`:

```cpp
#include "settings/display.h"

namespace denso::settings {

const char* to_string(DisplayMode mode) {
    switch (mode) {
        case DisplayMode::Borderless: return "borderless";
        case DisplayMode::Fullscreen: return "fullscreen";
        case DisplayMode::Windowed:   break;
    }
    return "windowed";
}

DisplayMode parse_display_mode(const std::string& s) {
    if (s == "borderless") return DisplayMode::Borderless;
    if (s == "fullscreen") return DisplayMode::Fullscreen;
    return DisplayMode::Windowed;  // "windowed" + every unknown value
}

} // namespace denso::settings
```

- [ ] **Step 4: Register sources** — in `src/core/CMakeLists.txt` add `settings/display.cpp` next to `settings/settings.cpp` (line ~18); in `tests/CMakeLists.txt` add `test_display.cpp` next to `test_settings.cpp` (line ~19).

- [ ] **Step 5: Configure + build + run the test**

Run: `export PATH=/c/msys64/ucrt64/bin:$PATH && cmake -S . -B build -G Ninja && cmake --build build --target denso_tests && ctest --test-dir build -R "DisplayMode" --output-on-failure`
Expected: 2 test cases PASS.

- [ ] **Step 6: Commit**

```bash
git add src/core/settings/display.h src/core/settings/display.cpp src/core/CMakeLists.txt tests/CMakeLists.txt tests/test_display.cpp
git commit -m "feat(settings): DisplayMode enum + persisted-token conversion"
```

---

## Task 2: DisplayState + display_changed (pure)

**Files:**
- Modify: `src/core/settings/display.h`, `src/core/settings/display.cpp`
- Test: `tests/test_display.cpp`

**Interfaces:**
- Consumes: `DisplayMode` (Task 1).
- Produces: `struct DisplayState { DisplayMode mode; uint32_t width; uint32_t height; std::string screen_name; }`; `bool display_changed(const DisplayState& from, const DisplayState& to)`.

- [ ] **Step 1: Write the failing test** — append to `tests/test_display.cpp`:

```cpp
TEST_CASE("display_changed: mode difference always counts") {
    DisplayState a{DisplayMode::Windowed, 1600, 900, "S1"};
    DisplayState b{DisplayMode::Fullscreen, 1600, 900, "S1"};
    CHECK(display_changed(a, b));
}

TEST_CASE("display_changed: size counts only in Windowed") {
    DisplayState w1{DisplayMode::Windowed, 1600, 900, "S1"};
    DisplayState w2{DisplayMode::Windowed, 1280, 720, "S1"};
    CHECK(display_changed(w1, w2));

    DisplayState f1{DisplayMode::Fullscreen, 1600, 900, "S1"};
    DisplayState f2{DisplayMode::Fullscreen, 1280, 720, "S1"};
    CHECK_FALSE(display_changed(f1, f2));  // size irrelevant in Fullscreen
}

TEST_CASE("display_changed: same state (incl. screen move) is no change") {
    DisplayState a{DisplayMode::Windowed, 1600, 900, "S1"};
    DisplayState same{DisplayMode::Windowed, 1600, 900, "S1"};
    DisplayState moved{DisplayMode::Windowed, 1600, 900, "S2"};
    CHECK_FALSE(display_changed(a, same));
    CHECK_FALSE(display_changed(a, moved));  // screen move alone doesn't confirm
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `export PATH=/c/msys64/ucrt64/bin:$PATH && cmake --build build --target denso_tests 2>&1 | tail -5`
Expected: compile FAIL — `DisplayState`/`display_changed` not declared.

- [ ] **Step 3: Add to the header** — in `display.h`, after `parse_display_mode`:

```cpp
/// Semantic display state — what the user chose, independent of raw Qt window
/// flags. `width`/`height` are the Windowed client size.
struct DisplayState {
    DisplayMode mode = DisplayMode::Windowed;
    uint32_t width = 1600;
    uint32_t height = 900;
    std::string screen_name;

    bool operator==(const DisplayState&) const = default;
};

/// A change worth a confirm/revert transaction: mode differs, or (both Windowed
/// and the window size differs). Screen moves and size changes in
/// Borderless/Fullscreen do not count.
bool display_changed(const DisplayState& from, const DisplayState& to);
```

- [ ] **Step 4: Add to the source** — in `display.cpp`:

```cpp
bool display_changed(const DisplayState& from, const DisplayState& to) {
    if (from.mode != to.mode) return true;
    if (from.mode == DisplayMode::Windowed &&
        (from.width != to.width || from.height != to.height)) {
        return true;
    }
    return false;
}
```

- [ ] **Step 5: Run test to verify it passes**

Run: `ctest --test-dir build -R "display_changed" --output-on-failure`
Expected: 3 test cases PASS.

- [ ] **Step 6: Commit**

```bash
git add src/core/settings/display.h src/core/settings/display.cpp tests/test_display.cpp
git commit -m "feat(settings): semantic DisplayState + display_changed"
```

---

## Task 3: plan_transition + PlatformCaps (pure)

**Files:**
- Modify: `src/core/settings/display.h`, `src/core/settings/display.cpp`
- Test: `tests/test_display.cpp`

**Interfaces:**
- Consumes: `DisplayMode`, `DisplayState`, `display_changed`.
- Produces:
  - `struct PlatformCaps { bool windowing = true; }`
  - `struct TransitionPlan { DisplayMode mode; bool frameless; bool fullscreen; enum class Geom { ResizeWithinScreen, FullScreenRect, NativeFullscreen } geom; uint32_t width; uint32_t height; bool needs_confirm; }`
  - `TransitionPlan plan_transition(const DisplayState& current, DisplayMode requested, uint32_t req_w, uint32_t req_h, PlatformCaps caps)`

- [ ] **Step 1: Write the failing test** — append to `tests/test_display.cpp`:

```cpp
TEST_CASE("plan_transition: canonical flags per mode (absolute, not incremental)") {
    DisplayState fromBorderless{DisplayMode::Borderless, 1600, 900, "S1"};
    // Borderless -> Fullscreen must clear frameless (no stale flag).
    auto fs = plan_transition(fromBorderless, DisplayMode::Fullscreen, 1600, 900, {true});
    CHECK(fs.mode == DisplayMode::Fullscreen);
    CHECK(fs.fullscreen);
    CHECK_FALSE(fs.frameless);
    CHECK(fs.geom == TransitionPlan::Geom::NativeFullscreen);

    auto win = plan_transition(fromBorderless, DisplayMode::Windowed, 1280, 720, {true});
    CHECK(win.mode == DisplayMode::Windowed);
    CHECK_FALSE(win.fullscreen);
    CHECK_FALSE(win.frameless);
    CHECK(win.geom == TransitionPlan::Geom::ResizeWithinScreen);
    CHECK(win.width == 1280);
    CHECK(win.height == 720);

    auto bl = plan_transition({DisplayMode::Windowed, 1600, 900, "S1"},
                              DisplayMode::Borderless, 1600, 900, {true});
    CHECK(bl.frameless);
    CHECK_FALSE(bl.fullscreen);
    CHECK(bl.geom == TransitionPlan::Geom::FullScreenRect);
}

TEST_CASE("plan_transition: no windowing capability forces Fullscreen") {
    DisplayState cur{DisplayMode::Windowed, 1600, 900, "S1"};
    auto p = plan_transition(cur, DisplayMode::Windowed, 1600, 900, {false});
    CHECK(p.mode == DisplayMode::Fullscreen);
    CHECK(p.fullscreen);
}

TEST_CASE("plan_transition: needs_confirm follows display_changed") {
    DisplayState cur{DisplayMode::Windowed, 1600, 900, "S1"};
    CHECK(plan_transition(cur, DisplayMode::Fullscreen, 1600, 900, {true}).needs_confirm);
    CHECK_FALSE(plan_transition(cur, DisplayMode::Windowed, 1600, 900, {true}).needs_confirm);
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build --target denso_tests 2>&1 | tail -5`
Expected: compile FAIL — `plan_transition`/`PlatformCaps`/`TransitionPlan` undeclared.

- [ ] **Step 3: Add to the header** — in `display.h`:

```cpp
/// What the current platform can actually do. On a WM-less stack (eglfs/linuxfb)
/// windowed/borderless are fictional, so only Fullscreen is real.
struct PlatformCaps {
    bool windowing = true;
};

/// The canonical target of a mode transition — absolute, so it never inherits a
/// stale flag from the previous mode. The window layer executes this.
struct TransitionPlan {
    DisplayMode mode = DisplayMode::Windowed;
    bool frameless = false;
    bool fullscreen = false;
    enum class Geom { ResizeWithinScreen, FullScreenRect, NativeFullscreen } geom =
        Geom::ResizeWithinScreen;
    uint32_t width = 1600;
    uint32_t height = 900;
    bool needs_confirm = false;
};

/// Plan a transition from `current` to `requested` at the requested windowed
/// size, honoring platform capability. `req_w`/`req_h` matter only for Windowed.
TransitionPlan plan_transition(const DisplayState& current, DisplayMode requested,
                               uint32_t req_w, uint32_t req_h, PlatformCaps caps);
```

- [ ] **Step 4: Add to the source** — in `display.cpp`:

```cpp
TransitionPlan plan_transition(const DisplayState& current, DisplayMode requested,
                               uint32_t req_w, uint32_t req_h, PlatformCaps caps) {
    const DisplayMode eff = caps.windowing ? requested : DisplayMode::Fullscreen;

    TransitionPlan p;
    p.mode = eff;
    p.width = req_w;
    p.height = req_h;
    switch (eff) {
        case DisplayMode::Windowed:
            p.frameless = false; p.fullscreen = false;
            p.geom = TransitionPlan::Geom::ResizeWithinScreen;
            break;
        case DisplayMode::Borderless:
            p.frameless = true; p.fullscreen = false;
            p.geom = TransitionPlan::Geom::FullScreenRect;
            break;
        case DisplayMode::Fullscreen:
            p.frameless = false; p.fullscreen = true;
            p.geom = TransitionPlan::Geom::NativeFullscreen;
            break;
    }
    const DisplayState target{eff, req_w, req_h, current.screen_name};
    p.needs_confirm = display_changed(current, target);
    return p;
}
```

- [ ] **Step 5: Run test to verify it passes**

Run: `ctest --test-dir build -R "plan_transition" --output-on-failure`
Expected: 3 test cases PASS.

- [ ] **Step 6: Commit**

```bash
git add src/core/settings/display.h src/core/settings/display.cpp tests/test_display.cpp
git commit -m "feat(settings): canonical plan_transition + PlatformCaps"
```

---

## Task 4: fitting_presets + largest_fitting_preset (pure)

**Files:**
- Modify: `src/core/settings/settings.h`, `src/core/settings/settings.cpp`
- Test: `tests/test_settings.cpp`

**Interfaces:**
- Produces: `std::vector<int> fitting_presets(uint32_t avail_w, uint32_t avail_h, uint32_t frame_w, uint32_t frame_h)`; `int largest_fitting_preset(uint32_t avail_w, uint32_t avail_h, uint32_t frame_w, uint32_t frame_h)` (returns -1 if none fit).

- [ ] **Step 1: Write the failing test** — append to `tests/test_settings.cpp`:

```cpp
TEST_CASE("fitting_presets returns indices whose FRAMED size fits") {
    // Frame overhead 20x40. 1920x1080 needs 1940x1120.
    auto all = denso::settings::fitting_presets(3000, 2000, 20, 40);
    CHECK(all.size() == denso::settings::PRESETS.size());  // all fit on a big screen

    // 1600x900 (index 2) framed = 1620x940; 1920x1080 framed = 1940x1120.
    auto some = denso::settings::fitting_presets(1620, 940, 20, 40);
    CHECK(some == std::vector<int>{0, 1, 2});  // 1920x1080 excluded
}

TEST_CASE("fitting_presets is empty when nothing fits, never a lie") {
    auto none = denso::settings::fitting_presets(500, 400, 20, 40);
    CHECK(none.empty());  // even 800x600 doesn't fit
}

TEST_CASE("largest_fitting_preset picks the biggest fitting, else -1") {
    CHECK(denso::settings::largest_fitting_preset(1620, 940, 20, 40) == 2);
    CHECK(denso::settings::largest_fitting_preset(3000, 2000, 20, 40) == 3);
    CHECK(denso::settings::largest_fitting_preset(500, 400, 20, 40) == -1);
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build --target denso_tests 2>&1 | tail -5`
Expected: compile FAIL — `fitting_presets`/`largest_fitting_preset` undeclared.

- [ ] **Step 3: Declare in the header** — in `settings.h`, after `preset_index`:

```cpp
#include <vector>
```
(ensure `<vector>` is included near the top), and after the `preset_index` declaration:

```cpp
/// Indices into PRESETS whose FRAMED size (client + window-frame overhead) fits
/// the available logical geometry. Empty when none fit — never returns an
/// oversized preset. PRESETS is ascending, so the result is ascending too.
std::vector<int> fitting_presets(uint32_t avail_w, uint32_t avail_h,
                                 uint32_t frame_w, uint32_t frame_h);

/// The largest fitting preset index, or -1 if none fit.
int largest_fitting_preset(uint32_t avail_w, uint32_t avail_h,
                           uint32_t frame_w, uint32_t frame_h);
```

- [ ] **Step 4: Define in the source** — in `settings.cpp`:

```cpp
std::vector<int> fitting_presets(uint32_t avail_w, uint32_t avail_h,
                                 uint32_t frame_w, uint32_t frame_h) {
    std::vector<int> out;
    for (int i = 0; i < static_cast<int>(PRESETS.size()); ++i) {
        const auto [w, h] = PRESETS[static_cast<size_t>(i)];
        if (w + frame_w <= avail_w && h + frame_h <= avail_h) out.push_back(i);
    }
    return out;
}

int largest_fitting_preset(uint32_t avail_w, uint32_t avail_h,
                           uint32_t frame_w, uint32_t frame_h) {
    const std::vector<int> fit = fitting_presets(avail_w, avail_h, frame_w, frame_h);
    return fit.empty() ? -1 : fit.back();  // PRESETS ascending -> back() is largest
}
```

- [ ] **Step 5: Run test to verify it passes**

Run: `ctest --test-dir build -R "fitting_presets|largest_fitting" --output-on-failure`
Expected: 3 test cases PASS.

- [ ] **Step 6: Commit**

```bash
git add src/core/settings/settings.h src/core/settings/settings.cpp tests/test_settings.cpp
git commit -m "feat(settings): fitting_presets/largest_fitting_preset (honest size list)"
```

---

## Task 5: Persistence swap — Settings::mode + repo dual-write (behavior-preserving)

**Files:**
- Modify: `src/core/settings/settings.h`, `src/core/settings/repo.cpp`, `src/app/ui/mainwindow.cpp`, `src/app/ui/settings/settings_dialog.cpp`
- Test: `tests/test_settings_repo.cpp`

**Interfaces:**
- Consumes: `DisplayMode`, `to_string`, `parse_display_mode` (Task 1).
- Produces: `Settings::mode` (replaces `Settings::fullscreen`). `load()` prefers `display_mode`, else legacy `fullscreen`, else Windowed. `save()` transactionally dual-writes `display_mode` + `fullscreen`.

This task swaps the persistence model while keeping the app's *visible* behavior identical (mode is bridged to the old bool at existing call sites). The real UI/window rework is Tasks 6–8.

- [ ] **Step 1: Write the failing tests** — append to `tests/test_settings_repo.cpp` (follow the file's existing in-memory DB setup pattern — reuse its helper for opening a temp DB + running migrations):

```cpp
TEST_CASE("save dual-writes display_mode and legacy fullscreen") {
    auto db = make_temp_db();  // existing helper in this file
    denso::settings::Settings s;
    s.mode = denso::settings::DisplayMode::Fullscreen;
    denso::settings::save(db, s);

    QSqlQuery q(db);
    q.exec("SELECT value FROM settings WHERE key='display_mode'"); q.next();
    CHECK(q.value(0).toString() == "fullscreen");
    q.exec("SELECT value FROM settings WHERE key='fullscreen'"); q.next();
    CHECK(q.value(0).toString() == "1");  // old build reads Fullscreen

    s.mode = denso::settings::DisplayMode::Borderless;
    denso::settings::save(db, s);
    q.exec("SELECT value FROM settings WHERE key='display_mode'"); q.next();
    CHECK(q.value(0).toString() == "borderless");
    q.exec("SELECT value FROM settings WHERE key='fullscreen'"); q.next();
    CHECK(q.value(0).toString() == "0");  // old build downgrades Borderless->Windowed
}

TEST_CASE("load prefers display_mode over legacy fullscreen") {
    auto db = make_temp_db();
    QSqlQuery q(db);
    q.exec("INSERT INTO settings(key,value) VALUES('display_mode','borderless')");
    q.exec("INSERT INTO settings(key,value) VALUES('fullscreen','1')");
    CHECK(denso::settings::load(db).mode == denso::settings::DisplayMode::Borderless);
}

TEST_CASE("load falls back to legacy fullscreen when display_mode absent") {
    auto db = make_temp_db();
    QSqlQuery q(db);
    q.exec("INSERT INTO settings(key,value) VALUES('fullscreen','1')");
    CHECK(denso::settings::load(db).mode == denso::settings::DisplayMode::Fullscreen);
}

TEST_CASE("load falls back to Windowed on corrupt display_mode") {
    auto db = make_temp_db();
    QSqlQuery q(db);
    q.exec("INSERT INTO settings(key,value) VALUES('display_mode','garbage')");
    CHECK(denso::settings::load(db).mode == denso::settings::DisplayMode::Windowed);
}
```

> If `test_settings_repo.cpp` has no `make_temp_db()` helper, reuse whatever setup the existing tests in that file use verbatim; do not invent a new pattern.

- [ ] **Step 2: Run tests to verify they fail**

Run: `cmake --build build --target denso_tests 2>&1 | tail -5`
Expected: compile FAIL — `Settings::mode` / `DisplayMode` unknown.

- [ ] **Step 3: Change the Settings struct** — in `settings.h`, add `#include "settings/display.h"` at the top, and replace the `fullscreen` field:

```cpp
struct Settings {
    uint32_t width = 1600;
    uint32_t height = 900;
    bool dark = true;
    DisplayMode mode = DisplayMode::Windowed;
};
```

- [ ] **Step 4: Update repo load/save/import** — in `repo.cpp`:

Replace the `fullscreen` read in `load()` with mode resolution (after the `dark` block):

```cpp
    if (const auto v = get(db, QStringLiteral("display_mode"))) {
        out.mode = parse_display_mode(v->toStdString());
    } else if (const auto f = get(db, QStringLiteral("fullscreen"))) {
        out.mode = (*f == QStringLiteral("1")) ? DisplayMode::Fullscreen
                                               : DisplayMode::Windowed;
    }
```

Replace the `fullscreen` write in `save()` with a transactional dual-write (QSqlDatabase's transaction()/commit() are non-const, so copy the handle):

```cpp
    QSqlDatabase wdb = db;  // handle copy shares the connection; commit() is non-const
    wdb.transaction();
    set(db, QStringLiteral("width"), QString::number(settings.width));
    set(db, QStringLiteral("height"), QString::number(settings.height));
    set(db, QStringLiteral("dark"), settings.dark ? QStringLiteral("1") : QStringLiteral("0"));
    set(db, QStringLiteral("display_mode"),
        QString::fromLatin1(to_string(settings.mode)));
    set(db, QStringLiteral("fullscreen"),
        settings.mode == DisplayMode::Fullscreen ? QStringLiteral("1")
                                                 : QStringLiteral("0"));
    wdb.commit();
```

Add `#include "settings/display.h"` to `repo.cpp` if not already transitively present. In `import_legacy`, where it currently sets `s.fullscreen = fv.toBool();`, replace with:

```cpp
        s.mode = fv.toBool() ? DisplayMode::Fullscreen : DisplayMode::Windowed;
```

(and delete the now-unused `s.fullscreen` default comment references).

- [ ] **Step 5: Bridge the existing call sites (keep behavior identical)** — in `mainwindow.cpp`, replace every `state_->fullscreen` / `s.fullscreen` / `d.fullscreen` read with `(… .mode == settings::DisplayMode::Fullscreen)`, and in `set_fullscreen(bool on)` replace `state_->fullscreen = on;` with:

```cpp
    state_->mode = on ? settings::DisplayMode::Fullscreen : settings::DisplayMode::Windowed;
```

Add `#include "settings/display.h"` to `mainwindow.cpp` if needed. (These sites are fully rewritten in Task 8; this step only keeps the build green and behavior unchanged.) `settings_dialog.cpp`'s `set_fullscreen`/`set_resolution_index` are untouched here (dialog rework is Task 6).

- [ ] **Step 6: Build + run the full suite**

Run: `cmake --build build && ctest --test-dir build --output-on-failure 2>&1 | tail -6`
Expected: all tests PASS (previous count + 4 new repo cases).

- [ ] **Step 7: Commit**

```bash
git add src/core/settings/settings.h src/core/settings/repo.cpp src/app/ui/mainwindow.cpp tests/test_settings_repo.cpp
git commit -m "feat(settings): persist display_mode with transactional legacy dual-write"
```

---

## Task 6: SettingsDialog — mode selector + Window size picker (staged)

**Files:**
- Modify: `src/app/ui/settings/settings_dialog.h`, `src/app/ui/settings/settings_dialog.cpp`
- Manual test (Qt widget — no unit test): build + smoke.

**Interfaces:**
- Consumes: `DisplayMode`, `fitting_presets`, `PRESETS` (Tasks 1/4).
- Produces: signal `void apply_display_requested(int mode, int width, int height)` (mode is `static_cast<int>(DisplayMode)`); `void set_display_mode(DisplayMode)`, `void set_window_size(uint32_t w, uint32_t h)`. Removes `apply_resolution_requested`, `toggle_fullscreen_requested`, `set_fullscreen`, `set_resolution_index`, `resolution_index`.

- [ ] **Step 1: Update the header** — in `settings_dialog.h`: add `#include "settings/display.h"`; replace the display-related signals/setters/members:

```cpp
    // replaces set_resolution_index/set_fullscreen:
    void set_display_mode(settings::DisplayMode mode);   // no signal emitted
    void set_window_size(uint32_t width, uint32_t height); // no signal emitted
```

```cpp
signals:
    void apply_display_requested(int mode, int width, int height);  // batched
    void theme_changed(bool dark);
    void reset_defaults_requested();
```

Replace members `resolution_`/`fullscreen_switch_`:

```cpp
    QComboBox* display_mode_ = nullptr;
    QComboBox* window_size_ = nullptr;
    QLabel* window_size_hint_ = nullptr;
```

Add private helpers:

```cpp
    void rebuild_window_sizes();   // filter PRESETS to this dialog's screen
    void sync_size_enabled();      // enable window_size_ only in Windowed
    settings::DisplayMode staged_mode() const;
```

- [ ] **Step 2: Rewrite `build_display()`** — in `settings_dialog.cpp` replace the body:

```cpp
QWidget* SettingsDialog::build_display() {
    auto* page = new QWidget;
    auto* v = new QVBoxLayout(page);
    v->setSpacing(12);
    v->addWidget(common::eyebrow(QStringLiteral("DISPLAY")));

    auto* mode_box = new QVBoxLayout;
    mode_box->setSpacing(6);
    mode_box->addWidget(common::dim_label(QStringLiteral("Display mode")));
    display_mode_ = new QComboBox;
    display_mode_->addItem(QStringLiteral("Windowed"),
                           static_cast<int>(settings::DisplayMode::Windowed));
    display_mode_->addItem(QStringLiteral("Borderless"),
                           static_cast<int>(settings::DisplayMode::Borderless));
    display_mode_->addItem(QStringLiteral("Fullscreen"),
                           static_cast<int>(settings::DisplayMode::Fullscreen));
    mode_box->addWidget(display_mode_);
    v->addLayout(mode_box);

    auto* size_box = new QVBoxLayout;
    size_box->setSpacing(6);
    size_box->addWidget(common::dim_label(QStringLiteral("Window size")));
    window_size_ = new QComboBox;
    size_box->addWidget(window_size_);
    window_size_hint_ = common::dim_label(
        QStringLiteral("Does not change the monitor resolution."));
    window_size_hint_->setProperty("faint", true);
    size_box->addWidget(window_size_hint_);
    v->addLayout(size_box);

    connect(display_mode_, &QComboBox::currentIndexChanged, this,
            [this](int) { sync_size_enabled(); });

    v->addStretch(1);
    return page;
}
```

- [ ] **Step 3: Add the helpers + setters** — in `settings_dialog.cpp`:

```cpp
void SettingsDialog::rebuild_window_sizes() {
    const QScreen* scr = screen();
    const QRect avail = scr ? scr->availableGeometry() : QRect(0, 0, 1920, 1080);
    // Rough frame overhead; final clamp happens in MainWindow::resize_within_screen.
    const std::vector<int> fit = settings::fitting_presets(
        static_cast<uint32_t>(avail.width()), static_cast<uint32_t>(avail.height()), 40, 80);

    const int prev_id = window_size_->count()
        ? window_size_->currentData().toInt() : -1;
    window_size_->clear();
    for (int idx : fit) {
        const auto [w, h] = settings::PRESETS[static_cast<size_t>(idx)];
        window_size_->addItem(QStringLiteral("%1 × %2").arg(w).arg(h), idx);
    }
    if (window_size_->count() == 0) {  // nothing fits — show current as disabled
        window_size_->addItem(QStringLiteral("(screen too small)"), -1);
    }
    const int restore = window_size_->findData(prev_id);
    if (restore >= 0) window_size_->setCurrentIndex(restore);
}

void SettingsDialog::sync_size_enabled() {
    window_size_->setEnabled(staged_mode() == settings::DisplayMode::Windowed);
}

settings::DisplayMode SettingsDialog::staged_mode() const {
    return static_cast<settings::DisplayMode>(display_mode_->currentData().toInt());
}

void SettingsDialog::set_display_mode(settings::DisplayMode mode) {
    suppress_signals_ = true;
    const int i = display_mode_->findData(static_cast<int>(mode));
    if (i >= 0) display_mode_->setCurrentIndex(i);
    sync_size_enabled();
    suppress_signals_ = false;
}

void SettingsDialog::set_window_size(uint32_t width, uint32_t height) {
    suppress_signals_ = true;
    rebuild_window_sizes();
    const int idx = settings::preset_index(width, height);
    const int at = window_size_->findData(idx);
    if (at >= 0) window_size_->setCurrentIndex(at);
    else if (window_size_->count() > 0) window_size_->setCurrentIndex(window_size_->count() - 1);
    suppress_signals_ = false;
}
```

- [ ] **Step 4: Wire Apply to the batched signal** — in the constructor's Apply button lambda, replace `emit apply_resolution_requested(resolution_index());` with:

```cpp
        int w = 1600, h = 900;
        const int id = window_size_->currentData().toInt();
        if (id >= 0) {
            const auto [pw, ph] = settings::PRESETS[static_cast<size_t>(id)];
            w = static_cast<int>(pw);
            h = static_cast<int>(ph);
        }
        emit apply_display_requested(display_mode_->currentData().toInt(), w, h);
        accept();
```

Also rebuild the size list when the dialog is shown: in `showEvent`, after `nav_->setCurrentRow(0);` add `rebuild_window_sizes();`. Remove the old `set_resolution_index`/`set_fullscreen`/`resolution_index` definitions.

- [ ] **Step 5: Build + smoke**

Run: `export PATH=/c/msys64/ucrt64/bin:$PATH && cmake --build build 2>&1 | tail -5`
Expected: builds clean (MainWindow still references removed signals → will fail; that's fixed in Task 8, so this task's build gate is the dialog TU compiling: `cmake --build build --target denso 2>&1 | grep settings_dialog` shows no errors for `settings_dialog.cpp.obj`). If the link fails only on MainWindow signal wiring, proceed to Task 8 before the full build gate.

- [ ] **Step 6: Commit**

```bash
git add src/app/ui/settings/settings_dialog.h src/app/ui/settings/settings_dialog.cpp
git commit -m "feat(ui): game-style Display panel — mode selector + filtered Window size"
```

---

## Task 7: Display confirm/revert dialog (countdown)

**Files:**
- Create: `src/app/ui/settings/display_confirm_dialog.h`, `src/app/ui/settings/display_confirm_dialog.cpp`
- Modify: `src/app/CMakeLists.txt` (add the new source if sources are listed explicitly)
- Manual test (Qt widget).

**Interfaces:**
- Produces: `class denso::ui::DisplayConfirmDialog : public QDialog` with `explicit DisplayConfirmDialog(int seconds, QWidget* parent)`; returns `QDialog::Accepted` on Keep, `QDialog::Rejected` on Revert/timeout. Frameless, `WindowStaysOnTopHint`, application-modal.

- [ ] **Step 1: Create the header** — `display_confirm_dialog.h`:

```cpp
// A game-style "Keep these display settings?" confirmation with an auto-revert
// countdown. Modal, always-on-top so it shows above a fullscreen window. Accept
// = Keep; Reject (button or timeout) = revert. Display-specific by design.
#pragma once

#include <QDialog>

class QLabel;
class QTimer;

namespace denso::ui {

class DisplayConfirmDialog : public QDialog {
    Q_OBJECT
public:
    explicit DisplayConfirmDialog(int seconds, QWidget* parent = nullptr);

private:
    void tick();
    QLabel* message_ = nullptr;
    QTimer* timer_ = nullptr;
    int remaining_ = 0;
};

} // namespace denso::ui
```

- [ ] **Step 2: Create the source** — `display_confirm_dialog.cpp`:

```cpp
#include "ui/settings/display_confirm_dialog.h"

#include <QDialogButtonBox>
#include <QLabel>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

namespace denso::ui {

DisplayConfirmDialog::DisplayConfirmDialog(int seconds, QWidget* parent)
    : QDialog(parent), remaining_(seconds) {
    setWindowTitle(QStringLiteral("Confirm display settings"));
    setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
    setModal(true);

    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(24, 24, 24, 24);
    v->setSpacing(16);
    message_ = new QLabel;
    message_->setAlignment(Qt::AlignCenter);
    v->addWidget(message_);

    auto* buttons = new QDialogButtonBox;
    auto* keep = buttons->addButton(QStringLiteral("Keep"), QDialogButtonBox::AcceptRole);
    keep->setProperty("gold", true);
    buttons->addButton(QStringLiteral("Revert"), QDialogButtonBox::RejectRole);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    v->addWidget(buttons);

    timer_ = new QTimer(this);
    timer_->setInterval(1000);
    connect(timer_, &QTimer::timeout, this, &DisplayConfirmDialog::tick);
    timer_->start();
    tick();  // paint the initial count
}

void DisplayConfirmDialog::tick() {
    if (remaining_ <= 0) {
        timer_->stop();
        reject();  // timeout -> revert
        return;
    }
    message_->setText(
        QStringLiteral("Keep these display settings?\nReverting in %1s…").arg(remaining_));
    --remaining_;
}

} // namespace denso::ui
```

- [ ] **Step 3: Register the source** — if `src/app/CMakeLists.txt` lists `denso` sources explicitly, add `ui/settings/display_confirm_dialog.cpp`. (If it globs, skip.) Verify with: `grep -n "settings_dialog.cpp" src/app/CMakeLists.txt` — mirror that entry.

- [ ] **Step 4: Build the TU**

Run: `export PATH=/c/msys64/ucrt64/bin:$PATH && cmake -S . -B build -G Ninja && cmake --build build --target denso 2>&1 | grep -i "display_confirm" || echo "compiles (no errors)"`
Expected: the new TU compiles (final link is completed in Task 8).

- [ ] **Step 5: Commit**

```bash
git add src/app/ui/settings/display_confirm_dialog.h src/app/ui/settings/display_confirm_dialog.cpp src/app/CMakeLists.txt
git commit -m "feat(ui): display confirm/revert countdown dialog"
```

---

## Task 8: MainWindow — apply display via planner + confirm transaction

**Files:**
- Modify: `src/app/ui/mainwindow.h`, `src/app/ui/mainwindow.cpp`
- Manual test (Qt widget) — build clean + full suite + on-device checklist.

**Interfaces:**
- Consumes: `apply_display_requested(int,int,int)` (Task 6), `DisplayConfirmDialog` (Task 7), `plan_transition`/`PlatformCaps`/`DisplayState`/`TransitionPlan` (Tasks 2/3), `settings::save/load`.
- Produces: `void apply_display_mode(const settings::TransitionPlan&)`, `settings::DisplayState current_display_state() const`, `settings::PlatformCaps platform_caps() const`, `void on_apply_display(int mode, int width, int height)`.

- [ ] **Step 1: Update the header** — in `mainwindow.h`, add includes for `settings/display.h`; declare:

```cpp
    void on_apply_display(int mode, int width, int height);
    void apply_display_mode(const settings::TransitionPlan& plan);
    settings::DisplayState current_display_state() const;
    settings::PlatformCaps platform_caps() const;
```
Remove `on_apply_resolution`, `on_toggle_fullscreen`, `set_fullscreen` declarations (their behavior folds into the above).

- [ ] **Step 2: Replace the signal wiring** — in the constructor, replace the `apply_resolution_requested`/`toggle_fullscreen_requested` connects with:

```cpp
    connect(settings_, &SettingsDialog::apply_display_requested, this,
            &MainWindow::on_apply_display);
```

- [ ] **Step 3: Implement platform caps + current state** — in `mainwindow.cpp`:

```cpp
#include "ui/settings/display_confirm_dialog.h"
#include <QGuiApplication>
#include <QScreen>

settings::PlatformCaps MainWindow::platform_caps() const {
    const QString plat = QGuiApplication::platformName();
    // eglfs/linuxfb have no window manager -> only Fullscreen is real.
    const bool windowing = plat != QStringLiteral("eglfs") &&
                           plat != QStringLiteral("linuxfb");
    return settings::PlatformCaps{windowing};
}

settings::DisplayState MainWindow::current_display_state() const {
    const QScreen* scr = screen();
    return settings::DisplayState{
        state_->mode, state_->width, state_->height,
        scr ? scr->name().toStdString() : std::string{}};
}
```

- [ ] **Step 4: Implement `apply_display_mode` (canonical sequence)** — in `mainwindow.cpp`:

```cpp
void MainWindow::apply_display_mode(const settings::TransitionPlan& plan) {
    // Canonical, deterministic: hide -> set absolute flags -> clear state -> show
    // -> re-assert geometry. Prevents a stale frameless hint surviving a switch.
    const bool frameless_now = windowFlags().testFlag(Qt::FramelessWindowHint);
    if (frameless_now != plan.frameless || isFullScreen()) {
        hide();
        setWindowFlag(Qt::FramelessWindowHint, plan.frameless);
        setWindowState(windowState() & ~Qt::WindowFullScreen);
        show();
    }
    switch (plan.geom) {
        case settings::TransitionPlan::Geom::ResizeWithinScreen:
            showNormal();
            resize_within_screen(static_cast<int>(plan.width),
                                 static_cast<int>(plan.height));
            break;
        case settings::TransitionPlan::Geom::FullScreenRect: {
            showNormal();
            const QScreen* scr = screen();
            if (scr) setGeometry(scr->geometry());
            break;
        }
        case settings::TransitionPlan::Geom::NativeFullscreen:
            showFullScreen();
            break;
    }
}
```

- [ ] **Step 5: Implement the confirm transaction** — in `mainwindow.cpp`:

```cpp
void MainWindow::on_apply_display(int mode, int width, int height) {
    const auto requested = static_cast<settings::DisplayMode>(mode);
    const settings::DisplayState before = current_display_state();
    const settings::TransitionPlan plan = settings::plan_transition(
        before, requested, static_cast<uint32_t>(width),
        static_cast<uint32_t>(height), platform_caps());

    apply_display_mode(plan);

    if (!plan.needs_confirm) {  // e.g. same mode/size, or platform forced no-op
        commit_display(plan);
        return;
    }
    // Let the window-system settle, then confirm above the (maybe fullscreen) window.
    DisplayConfirmDialog dlg(15, this);
    dlg.raise();
    dlg.activateWindow();
    const int result = dlg.exec();
    if (result == QDialog::Accepted) {
        commit_display(plan);
    } else {
        // Revert by re-applying the previous semantic state (no raw-flag replay).
        const settings::TransitionPlan revert = settings::plan_transition(
            current_display_state(), before.mode, before.width, before.height,
            platform_caps());
        apply_display_mode(revert);
    }
}

void MainWindow::commit_display(const settings::TransitionPlan& plan) {
    state_->mode = plan.mode;
    if (plan.mode == settings::DisplayMode::Windowed) {
        state_->width = plan.width;
        state_->height = plan.height;
    }
    settings::save(db_, *state_);
    settings_->set_display_mode(state_->mode);
    settings_->set_window_size(state_->width, state_->height);
}
```

Declare `void commit_display(const settings::TransitionPlan&);` in the header.

- [ ] **Step 6: Rewrite startup + reset + F11/Esc + open_settings + fix the comment** — in `mainwindow.cpp`:

`apply_startup()`: replace the resolution/fullscreen seeding + apply with:

```cpp
    settings_->set_display_mode(s.mode);
    settings_->set_window_size(s.width, s.height);
    settings_->set_theme_dark(s.dark);
    const settings::TransitionPlan boot = settings::plan_transition(
        settings::DisplayState{settings::DisplayMode::Windowed, s.width, s.height, {}},
        s.mode, s.width, s.height, platform_caps());
    apply_display_mode(boot);  // no confirm dialog on trusted persisted state
    apply_theme(s.dark);
```

`open_settings()`: replace the two `set_resolution_index`/`set_fullscreen` lines with:

```cpp
    settings_->set_display_mode(state_->mode);
    settings_->set_window_size(state_->width, state_->height);
```

F11/Esc shortcuts (fix the stale comment at ~line 69-71 and route through modes):

```cpp
    // F11 toggles Fullscreen<->Windowed, Esc leaves Fullscreen. These are
    // convenience shortcuts; the top bar (with Settings) stays visible in every
    // mode, so a touchscreen operator can also switch mode from Settings.
    auto* fs = new QShortcut(QKeySequence(Qt::Key_F11), this);
    connect(fs, &QShortcut::activated, this, [this] {
        const auto next = isFullScreen() ? settings::DisplayMode::Windowed
                                         : settings::DisplayMode::Fullscreen;
        on_apply_display(static_cast<int>(next), static_cast<int>(state_->width),
                         static_cast<int>(state_->height));
    });
    auto* esc = new QShortcut(QKeySequence(Qt::Key_Escape), this);
    connect(esc, &QShortcut::activated, this, [this] {
        if (isFullScreen())
            on_apply_display(static_cast<int>(settings::DisplayMode::Windowed),
                             static_cast<int>(state_->width),
                             static_cast<int>(state_->height));
    });
```

`on_reset_defaults()`: replace its fullscreen/resize block with a routed apply:

```cpp
    const settings::Settings d;  // defaults (Windowed, 1600x900, dark)
    state_->mode = d.mode;
    state_->width = d.width;
    state_->height = d.height;
    state_->dark = d.dark;
    settings::save(db_, *state_);
    const settings::TransitionPlan p = settings::plan_transition(
        current_display_state(), d.mode, d.width, d.height, platform_caps());
    apply_display_mode(p);
    settings_->set_display_mode(d.mode);
    settings_->set_window_size(d.width, d.height);
    settings_->set_theme_dark(d.dark);
    apply_theme(d.dark);
```

Delete the now-dead `on_apply_resolution`, `on_toggle_fullscreen`, and `set_fullscreen` definitions.

- [ ] **Step 7: Build clean + full suite**

Run: `export PATH=/c/msys64/ucrt64/bin:$PATH && cmake --build build 2>&1 | tail -5 && ctest --test-dir build --output-on-failure 2>&1 | tail -4`
Expected: build clean; all tests PASS.

- [ ] **Step 8: Windows smoke (manual)** — run `./build/src/app/denso.exe`, open Settings → Display:
  - Windowed + pick a size → Apply → confirm dialog → Keep persists; reopen shows the size.
  - Fullscreen → Apply → confirm dialog → wait 15s → auto-reverts to Windowed.
  - Borderless → Apply → Keep; frameless full-screen window; top bar still visible.
  - Window size greyed in Borderless/Fullscreen.
  - F11 toggles; Esc exits fullscreen. Restart the app → last kept mode restored.

- [ ] **Step 9: Commit**

```bash
git add src/app/ui/mainwindow.h src/app/ui/mainwindow.cpp
git commit -m "feat(ui): game-style display apply — planner + confirm/revert transaction"
```

---

## Task 9: On-device verification (Jetson) + cleanup

**Files:** none (verification) — plus any fixes surfaced.

- [ ] **Step 1: Push the branch + build on the Jetson** (X11 GNOME — windowing works):

```bash
git push -u origin feature/display-settings-game-style
ssh modela@192.168.1.15 'cd ~/project/Denso-DigitalReader && git fetch origin feature/display-settings-game-style && git checkout feature/display-settings-game-style && cmake --build build -- -j4 && ctest --test-dir build 2>&1 | tail -3'
```
Expected: build clean; tests PASS (Linux count).

- [ ] **Step 2: GUI smoke via AnyDesk** (see `d:\workspace\devices.md`) — repeat Task 8 Step 8 on the Jetson panel: each mode transition, confirm/revert countdown, Window-size filtered to the panel, top bar reachable in fullscreen, restart restores mode.

- [ ] **Step 3: Codex review of the full branch diff** — request a review pass; apply any must-fix findings (new commits).

- [ ] **Step 4: Finalize** — invoke `superpowers:finishing-a-development-branch` to decide merge/PR.

---

## Self-Review

- **Spec coverage:** modes (T3/T6/T8), Window size rename+filter (T4/T6), staged Apply (T6/T8), confirm/revert (T7/T8), F11/Esc + top-bar comment fix (T8), persistence dual-write + fallback (T5), platform guard (T3/T8), pure test coverage (T1–T5), on-device (T9). All spec sections map to a task.
- **Placeholder scan:** none — every code step has full code.
- **Type consistency:** `DisplayMode`, `DisplayState`, `TransitionPlan`, `PlatformCaps`, `plan_transition`, `fitting_presets`/`largest_fitting_preset`, `apply_display_requested(int,int,int)`, `apply_display_mode`, `commit_display` are used consistently across tasks.
- **Note for the executor:** Tasks 6–8 form a compile set — the full `denso` link only succeeds after Task 8; use per-TU build checks within Tasks 6–7 as noted.
