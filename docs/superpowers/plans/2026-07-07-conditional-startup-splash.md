# Conditional Startup Splash (cold-only) + UI-First Warm Restart — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Show the blocking `StartupScreen` splash only on a *cold* start (no cached TensorRT engine → the minutes-long build); on a *warm* restart (engine cached) keep the fast UI-first load.

**Architecture:** A new pure `startup_mode` unit decides cold vs warm by probing the filesystem (models have `*.onnx`, cache has no `*.engine`). `launch()` becomes a thin selector over two isolated helpers in `startup.cpp`: `launch_cold_with_splash` (restores the pre-UI-first splash flow, building `MainWindow` with `warmup=nullptr` after warm-up finishes) and `launch_warm_ui_first` (the current UI-first flow). The deleted `StartupScreen` is restored. All UI-first machinery (`WarmupState`, `warmup_gate`, the `EngineRegistry` mutex/`on_ready`, the "Preparing" tile) stays and serves the warm path.

**Tech Stack:** C++17, Qt6 (Core/Widgets), OpenCV, ONNX Runtime/TensorRT, Catch2 v3, CMake + Ninja, MSYS2 UCRT64.

## Global Constraints

- Toolchain: MSYS2 UCRT64. Build: `export PATH=/c/msys64/ucrt64/bin:$PATH; cmake -S . -B build -G Ninja; cmake --build build; ctest --test-dir build`. (PowerShell: `$env:PATH="C:\msys64\ucrt64\bin;$env:PATH"; cmake --build build; if ($?) { ctest --test-dir build }`.)
- **Hard rule — no engine build on a capture thread.** The minutes-long, non-interruptible TensorRT build must run only on the warm-up worker. Cold flow: warm-up finishes before `MainWindow` (and thus any capture thread) is built. Warm flow: a detection camera's stream is created only after its models are ready, so `EngineRegistry::get()` is a cache-hit.
- `denso_core` must not link `Qt6::Widgets`; this feature is app-layer only (`src/app/…`).
- The test target (`denso_tests`) links `denso_core` + Catch2 + OpenCV + `Qt6::Gui` only (**no Widgets**). So pure/Core-only units are unit-tested; `QWidget`-derived units (`StartupScreen`, `MainWindow`, `startup`) are **build-gated + on-device smoke**, not unit-tested. The pure `startup_mode` probe (Task 1) is the unit-tested piece.
- The cache path passed to the probe must be the exact one `EngineRegistry` uses: `applicationDirPath()/models` and `applicationDirPath()/models/trt_cache`.
- Reuse the existing TensorRT cache config in `ort_engine.cpp` — do not add another cache. Do not remove any UI-first unit; the warm path depends on all of them.

---

## File Structure

- Create `src/app/ui/startup_mode.{h,cpp}` — pure `cold_start_needs_splash()` filesystem probe.
- Create `tests/test_startup_mode.cpp` — Catch2 unit tests (temp dirs).
- Restore `src/app/ui/startup_screen.{h,cpp}` — the splash widget (deleted by the UI-first merge).
- Modify `src/app/ui/startup.cpp` — `launch()` selector + `launch_cold_with_splash` + `launch_warm_ui_first`.
- Modify `src/app/CMakeLists.txt` — re-add `ui/startup_screen.cpp`; add `ui/startup_mode.cpp`.
- Modify `tests/CMakeLists.txt` — add `test_startup_mode.cpp` + `startup_mode.cpp`.
- Modify `CLAUDE.md`, `docs/ARCHITECTURE.md` — document the conditional (cold-only) splash.

---

## Task 1: `startup_mode` — cold/warm probe (pure, unit-tested)

**Files:**
- Create: `src/app/ui/startup_mode.h`
- Create: `src/app/ui/startup_mode.cpp`
- Test: `tests/test_startup_mode.cpp`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: nothing (std::filesystem only).
- Produces: `bool denso::ui::cold_start_needs_splash(const std::string& models_dir, const std::string& cache_dir);` — true iff `models_dir` has ≥1 `*.onnx` (case-insensitive) AND `cache_dir` has no `*.engine`.

- [ ] **Step 1: Write the header**

Create `src/app/ui/startup_mode.h`:

```cpp
// Picks the launch UX: a cold start (there are detection models to warm but no
// prebuilt TensorRT engine cached yet → the minutes-long build) shows the
// blocking StartupScreen splash; a warm restart (engine already cached) uses the
// fast UI-first path. Pure std::filesystem — unit-tested. See ui/startup.cpp.
#pragma once

#include <string>

namespace denso::ui {

/// True when launch() should show the blocking splash: models_dir has ≥1 *.onnx
/// AND cache_dir has no *.engine (a lone *.timing file does not count as warm).
bool cold_start_needs_splash(const std::string& models_dir,
                             const std::string& cache_dir);

} // namespace denso::ui
```

- [ ] **Step 2: Write the failing tests**

Create `tests/test_startup_mode.cpp`:

```cpp
#include "ui/startup_mode.h"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;
using denso::ui::cold_start_needs_splash;

namespace {
// Unique temp workspace per test (models/ + models/trt_cache/); RAII cleanup.
struct TempDirs {
    fs::path root;
    fs::path models;
    fs::path cache;
    explicit TempDirs(const std::string& tag) {
        root = fs::temp_directory_path() / ("denso_startup_mode_" + tag);
        models = root / "models";
        cache = root / "models" / "trt_cache";
        fs::remove_all(root);
        fs::create_directories(models);
    }
    ~TempDirs() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }
    void touch(const fs::path& dir, const std::string& name) const {
        fs::create_directories(dir);
        std::ofstream(dir / name) << "x";
    }
};
}  // namespace

TEST_CASE("no models → not cold (nothing to warm)") {
    TempDirs t("no_models");
    REQUIRE_FALSE(cold_start_needs_splash(t.models.string(), t.cache.string()));
}

TEST_CASE("models present, no engine cache → cold (splash)") {
    TempDirs t("no_cache");
    t.touch(t.models, "denso.onnx");
    REQUIRE(cold_start_needs_splash(t.models.string(), t.cache.string()));
}

TEST_CASE("models present, engine cached → warm (no splash)") {
    TempDirs t("warm");
    t.touch(t.models, "denso.onnx");
    t.touch(t.cache,
            "TensorrtExecutionProvider_TRTKernel_graph_x_fp16_sm89.engine");
    REQUIRE_FALSE(cold_start_needs_splash(t.models.string(), t.cache.string()));
}

TEST_CASE("models present, only timing cache (no engine) → cold") {
    TempDirs t("timing_only");
    t.touch(t.models, "denso.onnx");
    t.touch(t.cache, "TensorrtExecutionProvider_cache_sm89.timing");
    REQUIRE(cold_start_needs_splash(t.models.string(), t.cache.string()));
}

TEST_CASE("onnx extension match is case-insensitive") {
    TempDirs t("case");
    t.touch(t.models, "DENSO.ONNX");
    REQUIRE(cold_start_needs_splash(t.models.string(), t.cache.string()));
}
```

- [ ] **Step 3: Register the test + source in CMake**

In `tests/CMakeLists.txt`, after the `warmup_gate.cpp` block (near the end of the `add_executable(denso_tests …)` list, before the closing `)`), add:

```cmake
    test_startup_mode.cpp
    # startup_mode is GUI-target code but pure std::filesystem (no Qt/OpenCV), so
    # compile it in.
    ${CMAKE_SOURCE_DIR}/src/app/ui/startup_mode.cpp
```

- [ ] **Step 4: Run tests to verify they fail**

Run: `export PATH=/c/msys64/ucrt64/bin:$PATH; cmake -S . -B build -G Ninja; cmake --build build 2>&1 | tail -20`
Expected: configure/compile failure — `startup_mode.cpp` does not exist yet.

- [ ] **Step 5: Write the implementation**

Create `src/app/ui/startup_mode.cpp`:

```cpp
#include "ui/startup_mode.h"

#include <algorithm>
#include <cctype>
#include <filesystem>

namespace denso::ui {
namespace {
namespace fs = std::filesystem;

std::string lower_ext(const fs::path& p) {
    std::string ext = p.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return ext;
}

// True if `dir` exists and holds ≥1 regular file whose lower-cased extension is
// `want_ext` (e.g. ".onnx", ".engine").
bool dir_has_ext(const std::string& dir, const std::string& want_ext) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) {
        return false;
    }
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (entry.is_regular_file(ec) && lower_ext(entry.path()) == want_ext) {
            return true;
        }
    }
    return false;
}
}  // namespace

bool cold_start_needs_splash(const std::string& models_dir,
                             const std::string& cache_dir) {
    const bool has_models = dir_has_ext(models_dir, ".onnx");
    const bool has_engine = dir_has_ext(cache_dir, ".engine");
    return has_models && !has_engine;
}

}  // namespace denso::ui
```

- [ ] **Step 6: Run tests to verify they pass**

Run: `cmake --build build 2>&1 | tail -5; ctest --test-dir build --output-on-failure 2>&1 | tail -15`
Expected: all 5 `startup_mode` cases PASS; full suite green.

- [ ] **Step 7: Commit**

```bash
git add src/app/ui/startup_mode.h src/app/ui/startup_mode.cpp tests/test_startup_mode.cpp tests/CMakeLists.txt
git commit -m "feat(startup): pure cold_start_needs_splash probe (models present, no cached engine)"
```

---

## Task 2: Restore `StartupScreen` + branch `launch()` cold/warm

**Files:**
- Restore: `src/app/ui/startup_screen.h`, `src/app/ui/startup_screen.cpp`
- Modify: `src/app/ui/startup.cpp`
- Modify: `src/app/CMakeLists.txt`

**Interfaces:**
- Consumes: `cold_start_needs_splash` (Task 1); `StartupScreen(bool dark)` + `StartupScreen::set_status(const QString&)` (restored); `MainWindow(db, state, engines, WarmupState*)`, `WarmupState`, `WarmupWorker` (existing).
- Produces: unchanged `int launch(QApplication&, QSqlDatabase, std::shared_ptr<settings::Settings>)`.

- [ ] **Step 1: Restore the deleted splash widget from git**

The UI-first merge deleted `startup_screen.{h,cpp}` in commit `ea2891c`. Restore both files byte-for-byte from its parent:

```bash
git checkout ea2891c^ -- src/app/ui/startup_screen.h src/app/ui/startup_screen.cpp
```

Verify they came back (expect the class declaration):

```bash
grep -n "class StartupScreen" src/app/ui/startup_screen.h
```
Expected: one match (`class StartupScreen : public QWidget {`).

- [ ] **Step 2: Re-register `startup_screen.cpp` + add `startup_mode.cpp` in the app CMake**

In `src/app/CMakeLists.txt`, find the block:

```cmake
    ui/warmup_worker.cpp
    ui/warmup_state.cpp
    ui/startup.cpp
```

Replace it with:

```cmake
    ui/startup_screen.cpp
    ui/warmup_worker.cpp
    ui/warmup_state.cpp
    ui/startup_mode.cpp
    ui/startup.cpp
```

- [ ] **Step 3: Rewrite `startup.cpp` as a selector + two helpers**

Replace the entire body of `src/app/ui/startup.cpp` with:

```cpp
#include "ui/startup.h"

#include "ui/camera/shared/detection/engine_registry.h"
#include "ui/mainwindow.h"
#include "ui/startup_mode.h"
#include "ui/startup_screen.h"
#include "ui/warmup_state.h"
#include "ui/warmup_worker.h"

#include <QApplication>
#include <QCoreApplication>
#include <QThread>

#include <memory>
#include <string>
#include <utility>

namespace denso::ui {

namespace {

// Cold start: block behind the animated splash while every model warms on the
// worker, then build + show the window. Warm-up finishes before any capture
// thread exists, so the minutes-long TensorRT build never lands on one.
int launch_cold_with_splash(QApplication& app, QSqlDatabase db,
                            std::shared_ptr<settings::Settings> state,
                            std::shared_ptr<EngineRegistry> engines) {
    auto splash = std::make_unique<StartupScreen>(state->dark);
    splash->show();
    splash->raise();
    splash->activateWindow();  // claim the foreground at launch

    auto* thread = new QThread;
    auto* worker = new WarmupWorker(engines);
    worker->moveToThread(thread);

    // Built on the main thread once warm-up finishes; must outlive app.exec(),
    // so it lives in this scope and is populated by the finished handler.
    std::unique_ptr<MainWindow> window;

    QObject::connect(thread, &QThread::started, worker, &WarmupWorker::run);
    QObject::connect(worker, &WarmupWorker::progress, splash.get(),
                     &StartupScreen::set_status);
    QObject::connect(worker, &WarmupWorker::finished, &app,
                     [&window, &splash, thread, worker, db, state, engines]() {
                         thread->quit();
                         thread->wait();  // warm-up done before we build the grid
                         delete worker;
                         delete thread;

                         // warmup=nullptr: every model is warm now, so CameraGrid
                         // starts all cameras immediately (cache-hit get()).
                         window = std::make_unique<MainWindow>(db, state, engines,
                                                               nullptr);
                         window->apply_startup();
                         window->show();
                         // Created after the event loop is already running (via
                         // this queued handler), so pull it to the front and take
                         // the foreground the splash was holding.
                         window->raise();
                         window->activateWindow();
                         splash->close();
                         splash.reset();
                     });

    thread->start();
    return app.exec();
}

// Warm restart: show the window immediately and warm models in the background;
// each detection camera starts as its model(s) come ready (WarmupState + the
// per-camera gate). No splash.
int launch_warm_ui_first(QApplication& app, QSqlDatabase db,
                         std::shared_ptr<settings::Settings> state,
                         std::shared_ptr<EngineRegistry> engines) {
    // WarmupState owns the worker thread and outlives app.exec() (this scope).
    WarmupState warmup(engines);

    // Build the window first (it subscribes CameraGrid to warmup signals in its
    // ctor), THEN start warming — so any model_ready/finished the worker queues
    // is delivered after the grid has connected (is_ready() covers a race).
    MainWindow window(db, state, engines, &warmup);
    window.apply_startup();
    window.show();
    window.raise();
    window.activateWindow();  // claim the foreground at launch

    warmup.start();
    return app.exec();
}

}  // namespace

int launch(QApplication& app, QSqlDatabase db,
           std::shared_ptr<settings::Settings> state) {
    const std::string dir = QCoreApplication::applicationDirPath().toStdString();
    const std::string models_dir = dir + "/models";
    const std::string cache_dir = dir + "/models/trt_cache";
    auto engines = std::make_shared<EngineRegistry>(models_dir, cache_dir);

    // Splash only when there's a minutes-long build to wait on (cold); otherwise
    // the fast UI-first load.
    if (cold_start_needs_splash(models_dir, cache_dir)) {
        return launch_cold_with_splash(app, std::move(db), std::move(state),
                                       std::move(engines));
    }
    return launch_warm_ui_first(app, std::move(db), std::move(state),
                                std::move(engines));
}

} // namespace denso::ui
```

- [ ] **Step 4: Update the `startup.h` file comment**

In `src/app/ui/startup.h`, replace the file-header comment (currently describing the always-UI-first flow) with:

```cpp
// Startup orchestration: pick the launch UX. A cold start (no cached TensorRT
// engine → the minutes-long build) shows the blocking StartupScreen splash and
// warms behind it, then builds MainWindow. A warm restart builds + shows
// MainWindow immediately and warms in the background (WarmupState). The pure
// ui/startup_mode probe decides. Keeps main.cpp a thin orchestrator.
```

- [ ] **Step 5: Build + run the suite**

Run: `export PATH=/c/msys64/ucrt64/bin:$PATH; cmake -S . -B build -G Ninja; cmake --build build 2>&1 | tail -20; ctest --test-dir build --output-on-failure 2>&1 | tail -10`
Expected: clean build (StartupScreen restored + registered, both flows compile, `MainWindow` 4-arg ctor satisfied in both); suite green (startup is not unit-tested; this compiles it).

- [ ] **Step 6: Commit**

```bash
git add src/app/ui/startup_screen.h src/app/ui/startup_screen.cpp src/app/ui/startup.cpp src/app/ui/startup.h src/app/CMakeLists.txt
git commit -m "feat(startup): splash on cold start only; keep UI-first for warm restart"
```

---

## Task 3: Documentation

**Files:**
- Modify: `CLAUDE.md`
- Modify: `docs/ARCHITECTURE.md`

- [ ] **Step 1: Update `CLAUDE.md`**

Find this text in the TensorRT hard-rule bullet (in the "Detection / ONNX Runtime" section):

```
Startup
  is **UI-first**: the window shows immediately and warm-up runs in the
  background; each detection camera's capture thread (`CameraStream`) is created
  only **after its model(s) finish warming**, so `EngineRegistry::get()` for it is
  a cache-hit lookup on the GUI thread — the build never lands on a capture
  thread. Orientation-only cameras stream at once.
```

Replace with:

```
Startup
  is **conditional** (`ui/startup_mode` `cold_start_needs_splash`): a **cold**
  start (models present but no cached `*.engine` → the minutes-long build) shows
  the blocking `StartupScreen` splash and warms behind it, then builds the window;
  a **warm** restart is **UI-first** — the window shows immediately and warm-up
  runs in the background, each detection camera's capture thread (`CameraStream`)
  created only **after its model(s) finish warming** so `EngineRegistry::get()` is
  a cache-hit on the GUI thread. Either way the build never lands on a capture
  thread; orientation-only cameras stream at once on the warm path.
```

- [ ] **Step 2: Update `docs/ARCHITECTURE.md` boot sequence**

Find the numbered boot-sequence step describing `ui::launch` (starts `7. \`main\` hands off to \`ui::launch...`), which currently reads (in part):

```
   which builds the shared `EngineRegistry` + a `WarmupState` (owns the warm-up
   worker thread), then **immediately** builds `MainWindow` (injecting the
   registry + `WarmupState`), calls `MainWindow::apply_startup` + `show()`, and
   enters `QApplication::exec()`. Warm-up runs in the **background**: once the
   window is built (so `CameraGrid` has subscribed), `WarmupState::start()` warms
   every `models/*.onnx` on the worker, emitting `model_ready(file)` per model and
   `finished()` at the end. `CameraGrid` starts model-less/ready cameras at once
   and each pending detection camera as its models come ready — no blocking
   splash. (The old `StartupScreen` splash is retired.)
```

Replace that portion with:

```
   which builds the shared `EngineRegistry`, then picks the launch UX via
   `ui/startup_mode`'s `cold_start_needs_splash`. **Cold** (models present, no
   cached `*.engine`): show the `StartupScreen` splash, warm every `models/*.onnx`
   on the worker while it animates, and on `finished` build + show `MainWindow`
   (with `warmup = nullptr`, so `CameraGrid` starts every camera immediately on
   cache-hits). **Warm** (engine cached): build + show `MainWindow` immediately
   with a `WarmupState`, then `WarmupState::start()` warms in the background and
   `CameraGrid` starts model-less/ready cameras at once and each pending detection
   camera as its models come ready — no splash.
```

- [ ] **Step 3: Update the `docs/ARCHITECTURE.md` TensorRT gotcha**

Find the gotcha bullet that currently reads (in part):

```
  the warm-up worker thread (driven by `ui/warmup_state`, in the background while
  the window is already shown) — never lazily on a capture thread, which froze the
  UI and blocked stream `join()` on teardown (the reason TensorRT was dropped once
  before it was re-added behind the warm-up). Startup is UI-first: a detection
  camera's capture thread is created only after its models finish warming, so
  `get()` on that path is a cache-hit, never a build. Later runs load the cached
  engine from `models/trt_cache/`.
```

Replace that portion with:

```
  the warm-up worker thread — never lazily on a capture thread, which froze the UI
  and blocked stream `join()` on teardown (the reason TensorRT was dropped once
  before it was re-added behind the warm-up). Startup splits by whether that build
  is needed (`ui/startup_mode`): a **cold** start warms behind the blocking
  `StartupScreen` splash before the window (and any capture thread) exists; a
  **warm** restart is UI-first, creating each detection capture thread only after
  its models finish warming so `get()` there is a cache-hit. Later runs load the
  cached engine from `models/trt_cache/`.
```

- [ ] **Step 4: Commit**

```bash
git add CLAUDE.md docs/ARCHITECTURE.md
git commit -m "docs(startup): document conditional (cold-only) splash + warm UI-first"
```

---

## Manual Verification (on the MSYS2/GPU build machine — cannot run in the dev harness)

1. Build + suite: `cmake --build build && ctest --test-dir build` → green (new `startup_mode` cases included).
2. **Cold start** — delete `models/trt_cache/` (or move the `*.engine` files aside) to force a build, then launch: the **`StartupScreen` splash appears** and animates, the log shows the minutes-long TensorRT build on the warm-up worker, then the splash closes and the main window appears with cameras live.
3. **Warm restart** — relaunch with the cache present: **no splash**; the window appears immediately and detection tiles go live within the cache-load time (UI-first).
4. **New-model edge** — with a cache already present, drop a new `*.onnx` in `models/`: launch reads **warm** (no splash), and that new camera's tile shows "Preparing…" until its engine finishes building on the worker, then goes live.
5. Both flows shut down cleanly (splash flow joins the worker in the `finished` handler; warm flow via `WarmupState`'s destructor).

## Self-Review Notes (traceability)

- Spec §"Cold vs warm trigger" (`cold_start_needs_splash`, `.onnx`/`.engine`, `.timing` not warm, case-insensitive) → Task 1 (impl + 5 tests).
- Spec §"`launch()` selector + two helpers" → Task 2 Step 3.
- Spec §"`launch_cold_with_splash` (restored old flow, `warmup=nullptr`)" → Task 2 Steps 1–3.
- Spec §"`launch_warm_ui_first` (current UI-first flow)" → Task 2 Step 3.
- Spec §"Restore `startup_screen.{h,cpp}` + re-add to CMake; keep UI-first machinery" → Task 2 Steps 1–2 (nothing UI-first removed).
- Spec §"cache path must match EngineRegistry" → Task 2 Step 3 (`models_dir`/`cache_dir` shared by the probe and the registry).
- Spec §"Testing: pure probe unit-tested, widgets build+smoke" → Task 1 (unit) + Manual Verification.
- Spec §"Docs" → Task 3.
```
