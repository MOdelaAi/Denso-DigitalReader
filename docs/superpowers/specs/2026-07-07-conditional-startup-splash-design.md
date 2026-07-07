# Conditional Startup Splash (cold-only) + UI-First Warm Restart — Design

**Date:** 2026-07-07
**Status:** Approved (design), pending implementation plan
**Goal:** Make the splash conditional on whether a TensorRT engine actually has to
be built. On a **cold** first start (no cached engine → the minutes-long build)
show the blocking `StartupScreen` splash and warm up behind it, exactly as before.
On a **warm** restart (engine already cached) do the fast **UI-first** load we just
built — window immediately, no splash.

## Background

The just-merged "UI-first startup" feature (`d171693`,
[`2026-07-07-ui-first-startup-warmup-design.md`](2026-07-07-ui-first-startup-warmup-design.md))
made the window appear immediately on **every** launch and retired the
`StartupScreen` splash. The owner wants the splash back **only for the cold
case** — where there is genuinely a minutes-long TensorRT engine build to wait on
— while keeping the fast UI-first load for warm restarts.

The TensorRT engine cache already exists: `ort_engine.cpp` sets
`trt_engine_cache_enable=1` + `trt_timing_cache_enable=1` over
`models/trt_cache/`. On disk that directory holds one timing-cache file plus one
engine file per built model:

```
TensorrtExecutionProvider_cache_sm89.timing
TensorrtExecutionProvider_TRTKernel_graph_main_graph_<hash>_..._fp16_sm89.engine
```

So "has a prebuilt engine" is a reliable, already-present signal for warm vs cold.

## The hard rule this must preserve

Unchanged from the UI-first design: the minutes-long, non-interruptible TensorRT
build must run on the **warm-up worker thread**, never on a capture thread. Both
flows preserve it:

- **Cold** flow: warm-up runs fully on the worker *before* `MainWindow` is built,
  so no capture thread exists yet (old, proven behavior).
- **Warm** flow: each detection camera's capture thread is created only after its
  models are ready, so `EngineRegistry::get()` is a cache-hit on the GUI thread
  (the UI-first invariant).

## Cold vs warm trigger

A new small unit `src/app/ui/startup_mode.{h,cpp}`:

```cpp
// True when launch() should show the blocking splash: there is ≥1 detection
// model to warm AND no prebuilt TensorRT engine is cached yet (so warm_up will
// run the minutes-long build). A lone timing-cache file does NOT count as warm.
bool cold_start_needs_splash(const std::string& models_dir,
                             const std::string& cache_dir);
```

Logic: `has_onnx(models_dir) && !has_engine_cache(cache_dir)` where

- `has_onnx(models_dir)` — the directory contains ≥1 `*.onnx` (case-insensitive),
  matching `EngineRegistry::warm_up`'s own model scan.
- `has_engine_cache(cache_dir)` — the directory exists and contains ≥1 `*.engine`
  file. The `*.timing` file alone does **not** count (a timing cache can be
  written without a completed engine).

Called from `launch()` with the same paths `EngineRegistry` uses
(`applicationDirPath()/models` and `.../models/trt_cache`).

## Architecture

`launch()` becomes a thin selector over two isolated helpers in `startup.cpp`,
each with one clear job:

```
int launch(app, db, state):
    dir     = applicationDirPath()
    engines = make_shared<EngineRegistry>(dir+"/models", dir+"/models/trt_cache")
    if cold_start_needs_splash(dir+"/models", dir+"/models/trt_cache"):
        return launch_cold_with_splash(app, db, state, engines)
    else:
        return launch_warm_ui_first(app, db, state, engines)
```

### `launch_cold_with_splash` (restored old flow)

Restores the pre-UI-first behavior verbatim, adjusted only for the current
`MainWindow` signature:

1. Build + show `StartupScreen(state->dark)`; `raise()`/`activateWindow()`.
2. Start a `WarmupWorker` on a `QThread`; wire `WarmupWorker::progress →
   StartupScreen::set_status`.
3. On `WarmupWorker::finished` (after `thread->quit(); thread->wait();`): build
   `MainWindow(db, state, engines, /*warmup=*/nullptr)`, `apply_startup()`,
   `show()`, `raise()`, `activateWindow()`, close the splash.
4. `return app.exec();`

`MainWindow`'s 4th arg is `nullptr`: `CameraView`/`CameraGrid` already handle
`warmup_ == nullptr` by starting **every** camera immediately in `reload()`.
Because warm-up just finished, every `EngineRegistry::get()` is a cache-hit — no
build on a capture thread. No `WarmupState`, no "Preparing" tiles in this flow
(the splash covered the wait).

### `launch_warm_ui_first` (current UI-first flow)

Exactly the current `launch()` body: create `WarmupState`, build + show
`MainWindow(db, state, engines, &warmup)` immediately, then `warmup.start()`.
`CameraGrid` starts model-less/ready cameras at once and each pending detection
camera on `model_ready`, flushing the remainder on `finished`.

### Restored files

- Restore `src/app/ui/startup_screen.{h,cpp}` (deleted in the UI-first merge; bring
  back from git history) and re-add `ui/startup_screen.cpp` to
  `src/app/CMakeLists.txt`.
- Keep everything the UI-first feature added — `WarmupState`, `warmup_gate`
  (`PendingStart`), the `EngineRegistry` `get()` mutex + `warm_up(on_ready)`, and
  the `CameraTile::set_preparing` state — all used by the warm path.

### Data flow

```
launch: create EngineRegistry
   cold_start_needs_splash(models_dir, cache_dir)?
        │ true                                  │ false
   launch_cold_with_splash               launch_warm_ui_first
   show StartupScreen                    build+show MainWindow (instant)
   warm_up on worker (splash animates)   WarmupState.start() (background)
   on finished → build+show MainWindow   CameraGrid starts cams as models
   (warmup=nullptr, all cache-hits)      go ready ("Preparing" until then)
```

## Correctness / gotchas

- **New model added to a cached install:** the cache still has other `*.engine`
  files → classified **warm** → no splash; that one new camera's tile shows
  "Preparing…" while its engine builds on the worker (only that camera waits).
  Safe and arguably ideal.
- **CPU-only machine (no GPU):** the TensorRT EP never writes a `*.engine`, so it
  always reads **cold** → always splash. Accepted: a CPU-only run is a
  degraded/dev path, and warm-up there is still a real (if shorter) wait.
- **Interrupted cold build:** if a prior cold run wrote the `*.timing` file but no
  `*.engine`, `has_engine_cache` is still false → correctly re-classified cold →
  splash again. Good.
- **Cache path:** must be the exact path `EngineRegistry` uses
  (`applicationDirPath()/models/trt_cache`), or the probe and the build disagree.

## Testing & verification

- **Unit (Catch2, temp dirs):** `cold_start_needs_splash` —
  - no `.onnx` in models → false (nothing to warm).
  - `.onnx` present, cache dir empty/absent → true.
  - `.onnx` present, cache has a `*.engine` → false.
  - `.onnx` present, cache has only a `*.timing` (no engine) → true.
  (Precedent for temp-file tests: `test_db.cpp`.)
- **Build gate:** MSYS2 UCRT64 — `cmake --build build` clean, `ctest` green.
- **On-device (GPU) smoke:**
  - Delete `models/trt_cache/` → launch → **splash** shows, engine builds, then
    the main window appears live.
  - Relaunch (cache present) → **no splash**, window appears immediately, detection
    tiles go live within the cache-load time.
  - `startup.cpp`'s two flows both reach `app.exec()` and shut down cleanly.

## What does not change

- The UI-first warm path (window-first, `WarmupState`, per-camera gating,
  "Preparing" tile) — reused as-is for warm restarts.
- The TensorRT engine/timing cache config (`ort_engine.cpp`).
- `EngineRegistry::warm_up`'s build-on-worker model and the `on_ready` callback.
- The per-camera detection/orientation processor selection.

## Out of scope / deferred

- A progress bar/percentage on the splash (the existing `set_status` text is the
  only splash UI).
- Detecting per-model cache hits precisely (which specific `.onnx` is cached):
  the coarse "any engine present" signal is sufficient for the splash decision;
  the warm path already handles a not-yet-cached model per camera.
- Gating cold-detection on whether the GPU provider is actually active (CPU-only
  always-splash is accepted).
```
