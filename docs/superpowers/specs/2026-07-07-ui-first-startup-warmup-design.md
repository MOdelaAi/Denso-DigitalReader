# UI-First Startup With Background Warm-Up — Design

**Date:** 2026-07-07
**Status:** Approved (design), pending implementation plan
**Goal:** Make the app window appear immediately on launch instead of blocking
on model warm-up. Cameras that need no model stream at once; each detection
camera's capture thread starts as soon as *its* model(s) finish warming on the
background worker. Subsequent (cache-hit) launches feel fast; the first (cold)
build no longer shows a frozen splash.

## Background

A TensorRT engine cache already exists: `ort_engine.cpp` sets
`trt_engine_cache_enable=1` (`models/trt_cache/`) + `trt_timing_cache_enable=1`,
so the **first** launch builds the engine (minutes) and caches it, and **later**
launches load the cached engine. So a plain "add a cache" is already done.

What is still slow on **every** launch is the *gate*: `startup.cpp` does not show
`MainWindow` until `WarmupWorker::finished` — and warm-up runs a blank inference
over **every** model (CUDA init + engine deserialize + one inference) while the
user stares at the `StartupScreen` splash. On a cache-hit run there is no
minutes-long build, but the window still waits for that load+init sequence.

The lever is therefore *what we do with the existing cache*, not another cache:
show the UI first and warm in the background.

### The hard rule this must preserve

Per `CLAUDE.md` / `docs/ARCHITECTURE.md`: the minutes-long, non-interruptible
TensorRT build must run on the **warm-up worker thread**, "before any capture
thread exists" — never lazily on a capture thread, where it once froze the UI and
blocked stream teardown (`join()`), the reason TensorRT was dropped before. Also
note `EngineRegistry::get()` **builds lazily** if a model isn't cached yet.

This design keeps the invariant by re-scoping it per camera: **a detection
camera's capture thread starts only after its models are ready**, so the build
always happens on the warm-up worker and `get()` for that camera is a cache-hit
lookup on the GUI thread — never a build on a capture thread.

## Current vs new

| | Today | New |
|---|---|---|
| Window shows | after **all** models warmed | **immediately** |
| Orientation-only cameras | wait for warm-up | **stream at once** (no engine) |
| Detection cameras | wait for warm-up | tile "Preparing model…" → streams when ready |
| First (cold) run | frozen splash for minutes | window live; detection tiles say "building engine, may take minutes" |
| `StartupScreen` splash | blocks launch | retired |

## Architecture

### 1. `WarmupWorker` — `warmup_worker.{h,cpp}`

Add a per-model completion signal alongside the existing `progress`/`finished`:

```cpp
signals:
    void progress(QString status);        // existing
    void model_ready(QString filename);   // NEW: emitted after each model load+blank-infer
    void finished();                      // existing: all models done
```

`EngineRegistry::warm_up` already invokes an `on_model` callback *before* each
model; add a second callback (or reuse the loop) to fire **after** a model's
`get()+infer()` succeeds so the worker can emit `model_ready(filename)`. A model
that fails to load does **not** emit ready (handled by fallback on `finished`).

### 2. `WarmupState` (new, GUI-thread `QObject`) — `warmup_state.{h,cpp}`

The single channel the grid subscribes to. Owns the worker `QThread`; slots
consume the worker's queued signals and republish them on the GUI thread:

- state: `std::set<std::string> ready_`, `bool complete_ = false`.
- slots (from worker, queued): `on_model_ready(QString)` → insert + `emit
  model_ready`; `on_finished()` → `complete_ = true` + `emit finished`.
- queries: `bool is_ready(const std::string&) const`, `bool is_complete() const`.
- signals: `model_ready(QString)`, `finished()`.

This makes the ready-set logic unit-testable (Catch2 + `QCoreApplication`),
independent of the grid.

### 3. `EngineRegistry` — add a mutex — `engine_registry.{h,cpp}`

`get()` is now called from **both** the warm-up worker (during `warm_up`) and the
GUI thread (starting a ready camera), so the `engines_` map needs a
`std::mutex`. `get()` locks around the find/emplace; it returns a raw pointer to
an `OrtEngine` that is never erased, so the pointer stays valid lock-free. The
per-frame `infer()` path holds that raw pointer and never touches the registry,
so there is **no** per-frame locking. Update the stale "not internally
synchronized / single-threaded" header note.

### 4. `CameraGrid` — deferred per-camera start — `camera_grid.{h,cpp}`

Holds a `WarmupState*` (passed down alongside `engines_`). `reload()` builds all
tiles up front, then for each camera decides start-now vs pending:

- **No models** (orientation-only) → build `OrientationProcessor` + stream +
  `start()` now (unchanged path; needs no engine).
- **Has models, all `warmup_state_->is_ready(m)`** → build `DetectionProcessor`
  (cache-hit `get()` on GUI thread) + stream + `start()` now.
- **Has models, not all ready** → tile shows a "Preparing model…" placeholder;
  record a **pending** entry `{ camera, tile, required_model_files }`.

Subscriptions (connected **before** `reload`, so no readiness signal is missed —
`WarmupState` also records readiness in a set that `reload` queries):

- on `model_ready(file)` → for each pending camera whose `required_model_files`
  are now all ready → build its processor + stream, wire to its tile, `start()`;
  drop from pending.
- on `finished()` → **flush** every still-pending camera: build with whatever
  models loaded (skip failed ones), falling back to `OrientationProcessor` when
  none of its models loaded — identical to today's `runs.empty()` fallback.

The pending→start gating decision (`which pending cameras are satisfied by the
ready set`) is small; keep it in the grid but factored as a tiny local helper for
directness.

### 5. `startup.cpp` — show first, warm in background

```
create EngineRegistry + WarmupState (owns worker thread)
build MainWindow(db, state, engines, warmup_state)   // immediate
window->apply_startup(); window->show(); raise(); activateWindow()
warmup_state->start()                                // background worker begins
return app.exec()
```

`MainWindow` gains a `WarmupState*` (passed through to `CameraView` →
`CameraGrid`). The old `finished`-gated MainWindow construction and the
`StartupScreen` splash are removed. Foreground-claim (`raise`/`activateWindow`)
moves to the immediate `show()`.

**Ordering / no lost signals:** the worker is started and MainWindow is built
(subscribing + `reload`) all synchronously before `app.exec()` returns control to
the event loop, so any `model_ready`/`finished` the worker queues is either
already recorded in `WarmupState.ready_` (seen by `reload`'s `is_ready` query) or
delivered as a queued signal after subscription — never dropped.

### 6. `CameraTile` — "Preparing" placeholder — `camera_tile.{h,cpp}`

A "Preparing model…" placeholder state, reusing the existing
connecting/offline placeholder path (just a new label/state). No new layout.

### Data flow

```
startup: create EngineRegistry + WarmupState
   start warm-up worker (background)          build+show MainWindow (instant)
             │                                          │
   per model: load/build engine (worker)       CameraGrid::reload():
     → model_ready(file) ──────────────►          no-model / ready cams → start now
             │                                     others → "Preparing" (pending)
   finished ─────────────────────────►          flush pending (orientation fallback)
```

## Correctness / gotchas

- **No build on a capture thread:** a detection stream is created only after its
  models are ready, so `get()` at that point is a cache-hit GUI-thread lookup;
  the heavy build stays on the warm-up worker. Hard rule preserved.
- **Thread-safety:** `EngineRegistry::get()` mutex covers the only new sharing
  (worker + GUI). `OrtEngine::infer()` across capture threads is already shared
  today (ORT `Session::Run` is thread-safe) — unchanged.
- **Failed model:** never emits `model_ready`; on `finished` the camera falls
  back to orientation-only (today's behavior when a model won't load).
- **Reload during runtime** (e.g. around the camera modal, `release_streams()` /
  `reload()`): by then warm-up is usually complete; `is_ready`/`is_complete`
  still gate correctly, and a mid-warm-up reload simply re-derives pending state.

## Testing & verification

- **Unit (Catch2 + `QCoreApplication`):**
  - `WarmupState`: `is_ready` false→true on `model_ready`; `is_complete` on
    `finished`; signals re-emitted on the GUI thread.
  - `CameraGrid` gating with a **fake `WarmupState`**: an orientation-only camera
    starts immediately; a detection camera stays "Preparing" until its model
    fires `model_ready`, then starts; a multi-model camera waits for **all**; a
    never-ready model falls back on `finished`.
  - `EngineRegistry`: existing tests still pass; add a concurrent-`get()` smoke
    if practical.
- **Build gate:** MSYS2 UCRT64 — `cmake --build build` clean, `ctest` green
  (platform-specific counts as noted in `CLAUDE.md`).
- **On-device (Jetson/GPU) smoke:** cold first launch — window appears at once,
  orientation cameras live, detection tiles show "Preparing … (building engine)",
  then go live; second launch — window appears at once and detection tiles go
  live within the cache-load time. Confirm no UI freeze and clean shutdown mid
  warm-up.

## What does not change

- The TensorRT engine/timing cache config (`ort_engine.cpp`) — reused as-is.
- `WarmupWorker::warm_up`'s build-on-worker model — only a per-model signal is
  added.
- The detection/orientation processor selection per camera (same logic, just
  gated on readiness).

## Out of scope / deferred

- Prebuilt standalone `.engine` via `trtexec` (a different lever; deferred).
- Profiling-driven micro-optimization of CUDA init / deserialize time.
- Any change to first-run **build** duration (unavoidable; only its blocking of
  the window is removed).
- A progress bar / percentage; a per-tile "Preparing…" state is the only UI.

## Relationship to the brazing-retry spec

Independent feature, separate spec
([`2026-07-07-brazing-resilient-retry-design.md`](2026-07-07-brazing-resilient-retry-design.md)).
Shared only in that both touch the camera grid; no ordering dependency.
