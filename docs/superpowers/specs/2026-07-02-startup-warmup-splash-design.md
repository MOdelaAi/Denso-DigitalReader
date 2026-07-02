# Startup screen with threaded warm-up

**Date:** 2026-07-02
**Status:** Approved (design)

## Problem

On launch, `EngineRegistry::warm_up()` runs on the main thread **before**
`window.show()` (`main.cpp:97`). It is reached inside `MainWindow`
construction:

`MainWindow` ctor (`mainwindow.cpp:48`) → `new CameraView` → `CameraView`
ctor → `reload()` → `CameraGrid::reload()` (`camera_grid.cpp:63`) →
`warm_up()`.

For every `models/*.onnx`, warm-up creates the ORT session (TensorRT → CUDA
→ CPU EP fallback) and runs one blank 640×640 inference to force the
TensorRT engine build (first run: **minutes**, cached to
`models/trt_cache/`; later runs: a load) plus CUDA kernel init.

Result: on a cold first launch there is **nothing on screen for potentially
minutes** — the app looks hung. The other pre-`show()` steps (DB open,
migrations, legacy import, `sync_models`, network `reassert`, settings
load) are fast I/O and are not the concern; they will simply be covered by
the same screen if it is shown first.

## Goal

Show a startup screen immediately that tells the user the program is
starting, stays responsive (animated) during the multi-minute build, and
reports which model is being prepared. The main window replaces it when
warm-up completes.

## Decisions (from brainstorming)

- **Content:** logo + live status text. App icon + "Denso DigitalReader"
  title + a status line that updates per step (e.g. "Preparing model
  denso.onnx…"), plus an indeterminate progress bar as a liveness cue.
- **Threading:** run warm-up on a dedicated **startup worker thread** so
  the screen animates on the main event loop during the build. This
  deviates from the current "main thread" wording in the docs but
  preserves the real invariant it protected: **warm-up completes before
  the window shows and before any capture thread exists.** The screen
  cannot stay live if warm-up blocks the main thread (a single model's
  `infer(blank)` is one uninterruptible call), so threading is required.
- **Orchestration home:** a small `ui/startup` launch helper owns the
  `EngineRegistry`, the screen, and the worker — keeping `main.cpp` a thin
  orchestrator (its hard rule) and threading/UI out of `denso_core`.

## Flow (new)

```
main(): db → migrate → import → sync_models → reassert → load settings
      → return denso::ui::launch(app, conn, state)

launch():
  create EngineRegistry(models/, models/trt_cache/)          [shared_ptr]
  show StartupScreen (frameless, centered, dark-themed)
  start QThread → WarmupWorker::run() → engines->warm_up(progress_cb)
      worker emits progress("denso.onnx") → splash.set_status(...) [queued]
      worker emits finished()
  on finished (main thread):
      quit()+wait() the thread
      build MainWindow(conn, state, engines) → apply_startup() → show()
      splash.finish(window)
  return app.exec()
```

## New components

### `ui/startup_screen.{h,cpp}` — `StartupScreen : QWidget`

Frameless, centered on the primary screen, dark colors from
`theme::palette`. Contents: app icon + "Denso DigitalReader" title +
status `QLabel` + an **indeterminate `QProgressBar`** (`setRange(0, 0)`)
that animates on the event loop so the screen reads as alive even while a
single model builds.

- Slot: `set_status(const QString&)` — updates the status label.
- Uses the existing app icon asset (`assets/icon.png`; resolve the same
  way the window icon is resolved — confirm at implementation time).

### `ui/warmup_worker.{h,cpp}` — `WarmupWorker : QObject`

Holds `std::shared_ptr<EngineRegistry>`. Lives on a `QThread`
(`moveToThread`).

- `run()` slot: calls `engines_->warm_up(cb)` where `cb(filename)` emits
  `progress(QString)`; emits `finished()` at the end.
- Signals: `progress(QString)`, `finished()`.

### `ui/startup.{h,cpp}` — `int launch(QApplication&, QSqlDatabase, std::shared_ptr<Settings>)`

Wires screen + worker + thread, owns the deferred `MainWindow` (heap, built
in the `finished` handler), returns `app.exec()`.

## Changes to existing code

- **`EngineRegistry::warm_up`** gains an optional progress callback:
  `void warm_up(std::function<void(const std::string&)> on_model = {})`.
  It invokes `on_model(filename)` before each model's `infer`. Backward
  compatible (default no-op).
- **Ownership injection:** `EngineRegistry` becomes a `shared_ptr` created
  in `launch()` and passed into `MainWindow` → `CameraView` → `CameraGrid`
  constructors, stored as a member. `CameraGrid::reload()` **drops** its
  lazy `new EngineRegistry` + `warm_up()` block (`camera_grid.cpp:56-64`)
  and uses the injected, already-warm registry.
- **`main.cpp`** ends with `return denso::ui::launch(app, conn, state);`
  instead of constructing/showing the window inline.
- **`CMakeLists.txt`** (`src/app`): add the three new source files.
- **Docs:** update `CLAUDE.md` and `docs/ARCHITECTURE.md` — warm-up now
  runs on a dedicated startup worker thread while the splash animates,
  still completing before the window shows and before any capture thread
  exists.

## Thread-safety notes

- `EngineRegistry` is created on the main thread, `warm_up()` runs on the
  worker (mutating the `engines_` map via `get()` and calling `infer()`).
  Only the worker touches the registry during warm-up.
- The worker is `quit()`+`wait()`ed **before** `MainWindow`/`CameraGrid`
  are built, so the grid's later `get()` calls (main thread) and capture
  threads' cached-pointer use never race with the worker. All `*.onnx`
  are pre-warmed, so those `get()` calls hit existing entries (no map
  mutation).
- ORT sessions are created on the worker thread and later run on capture
  threads; ORT sessions are safe to run cross-thread, and cross-thread use
  already exists today (created on main, run on capture threads).

## Error handling & edge cases

- No models / no GPU → `warm_up` returns fast (or CPU fallback); the
  screen flashes briefly, then the window appears. Acceptable.
- Per-model load/build failure → already logged and skipped inside
  `warm_up`; `finished()` still fires and the window still shows.

## Testing

This is UI + ORT-bound with little pure logic to unit-test (the status
string is trivial). Verification:

- Existing **132 tests remain green** (`ctest`).
- **Manual launch:** the startup screen appears immediately, updates per
  model, the progress bar animates during the build, and the main window
  replaces it when warm-up completes.

No token unit tests will be added for lack of a pure seam worth testing;
this is stated plainly rather than padded.
