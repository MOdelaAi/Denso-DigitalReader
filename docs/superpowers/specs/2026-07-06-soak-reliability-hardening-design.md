# Soak-Reliability Hardening — Design

**Date:** 2026-07-06
**Status:** Approved (design), pending implementation plan
**Goal:** Let the app run unattended for hours/days without crashing, wedging, or
leaking — recovering on its own from camera drops and stuck OS/network calls.

## Background

A three-part read-only audit (capture path, ONNX/ORT engine, network layer,
app-lifetime resource growth) found the reliability surface is small and
concentrated. Confirmed *non*-issues (no fix needed): `OrtEngine::infer()` on a
shared engine is thread-safe (no shared mutable scratch, `Ort::Session::Run` is
concurrent-safe); ORT session/env/allocator are created once and reused; the DB
connection is opened once (WAL) and reused with all queries finalized; every
`cv::VideoCapture` site is RAII-clean; there are no repeating `QTimer`s; and
`CameraGrid::reload()` tears down cleanly with no per-camera accumulation.

The defects below are what remains. Scope for this pass: **Tier 1 + Tier 2**.
Tier 3 is deferred and documented at the end.

## Fixes

### A. Capture / stream path — `src/app/ui/camera/grid/camera_stream.cpp`

**A1 — Camera auto-reconnect (Tier 1, #1).**
Today `run()` is a single pass: open once, and on any read failure it `break`s
and the worker thread exits (`camera_stream.cpp:134`); if the camera is
unreachable at startup it `return`s (`:111`). Either way the feed is dead (red
"Offline") until the app restarts.

Restructure `run()` into an outer reconnect loop:

```
while (!stop_):
    emit Connecting
    open capture (usb index, or rtsp GStreamer→FFMPEG fallback — unchanged)
    if not opened:
        emit Offline
        interruptible_backoff()      # also recovers a startup-unreachable camera
        continue
    apply usb resolution (unchanged)
    emit Live
    inner read loop:
        on read fail / empty frame → break   # not return
        process + emit (unchanged)
        pace via existing chunked precise_sleep
    cap.release()
    if not stop_:
        emit Offline
        interruptible_backoff()
```

- **Backoff schedule:** 1s initial, ×2 per failed attempt, capped at 10s; reset
  to 1s after a successful live frame. Fast recovery from a blink, no hammering
  a truly-dead camera.
- The backoff wait polls `stop_` in the existing 20ms (`kStopPollMs`) chunks via
  `precise_sleep`, so `stop()`/teardown stays prompt.
- **Extracted for test:** `int next_backoff_ms(int current_ms)` — pure, unit-tested
  (1000→2000→…→10000 cap).
- The status transitions (Connecting/Live/Offline) already drive `CameraTile`;
  no grid or tile change is required.

**A2 — Exception guard on the capture thread (Tier 1, #2).**
`run()` is the body of a raw `std::thread` lambda (`camera_stream.cpp:73`). The
OpenCV pre-processing (`cvtColor`, `blobFromImage`) and YOLO decode run *outside*
`OrtEngine::infer()`'s try block, and `DetectionProcessor::process()` /
`run()` have no try/catch. A single malformed/odd-sized frame throws a
`cv::Exception` that escapes the thread function → `std::terminate()` → the whole
process dies.

Wrap the per-frame work in a helper:

```
QImage safe_process(FrameProcessor* p, const QImage& img) noexcept:
    try:    return p ? p->process(img) : img
    catch (const std::exception& e):  log (throttled); return img
```

`run()` calls `safe_process(processor_.get(), img)` instead of
`processor_->process(img)`. A bad frame is logged and skipped; the loop lives on.
Logging is throttled (e.g. at most once per second per stream) so a persistently
bad state can't spam the log. **Extracted for test:** `safe_process` is unit-tested
with a throwing `FrameProcessor` stub (returns the input image, does not throw).

**A3 — Frame backpressure (Tier 2, #5).**
`emit frame_ready(QImage)` is a queued cross-thread signal (`:139`) with no
drop/coalescing. Under sustained GUI-thread lag the queued full-resolution
`QImage` events accumulate unboundedly → memory spike / OOM.

Add a shared counter `std::shared_ptr<std::atomic<int>> queued_` to
`CameraStream`. Before emitting:

```
if should_emit(queued_->load(), kMaxInFlight):    # kMaxInFlight = 2
    queued_->fetch_add(1)
    emit frame_ready(img)
# else: drop this frame (drop-oldest)
```

`CameraTile::set_frame` decrements the *same* counter after taking the frame. The
counter is a `shared_ptr` held by both the stream and the tile (handed over at
wiring time in `CameraGrid::reload`), so lifetime is safe regardless of teardown
order — `CameraGrid::clear()` deletes streams before tiles, but the shared
`atomic` survives until both are gone. (Correctness note: `clear()` stops/joins
every worker first, then runs its deletes synchronously on the GUI thread with no
event-loop reentry, so no queued `set_frame` fires during teardown anyway; the
`shared_ptr` removes any dependence on that ordering.) **Extracted for test:**
`bool should_emit(int queued, int max)` — pure, unit-tested.

**A4 — Waitable-timer handle leak (Tier 2, #6).**
`static thread_local HANDLE timer = CreateWaitableTimerExW(...)` (`:43`) is never
`CloseHandle`d; one kernel handle leaks per worker thread, i.e. per stream per
reload. Wrap it in a tiny RAII holder:

```
struct WaitableTimer {
    HANDLE h = CreateWaitableTimerExW(nullptr, nullptr,
                   CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    ~WaitableTimer() { if (h) CloseHandle(h); }
};
static thread_local WaitableTimer timer;   // dtor runs at thread exit
```

No behavior change; the handle is released when the thread ends. Windows-only
block, unchanged on other platforms.

### B. Network path (Tier 1)

**B1 — Finite `QProcess` timeout (#3).**
`windows_backend.cpp:30,44` and `linux_backend.cpp:24` call
`waitForFinished(-1)`. A stuck `netsh`/`ipconfig`/`nmcli` hangs the calling
thread forever. Replace with `waitForFinished(kNetCmdTimeoutMs)` where
`kNetCmdTimeoutMs = 15000`; on timeout `p.kill(); p.waitForFinished(kGraceMs)`
(e.g. 2000ms) and treat the command as failed (`run` returns empty / stderr;
`run_checked` reports failure the same way it does for a nonzero exit).

**B2 — `apply_config` off the GUI thread (#4).**
`network_panel.cpp:94` calls `apply_config` directly in `apply_net_config`,
unlike refresh/scan/connect which use `common::run_on_worker`. Combined with the
infinite wait, a stuck `netsh` freezes the entire UI. Move the `apply_config`
call onto `run_on_worker`, posting success/failure back to the GUI, matching the
sibling handlers. (Depends on C-fixes for lifetime safety.)

**B3 — Boot `reassert` deferred and bounded (#7).**
`main.cpp:88` runs `reassert(...)` → `apply_config` synchronously *before*
`app.exec()`; a stuck CLI at boot means the window never appears. Defer it to the
first event-loop tick (`QTimer::singleShot(0, ...)`) and make it best-effort
(bounded by B1's timeout, exceptions swallowed). The window shows first; network
reassert runs just after, and a failure/stall degrades gracefully instead of
blocking startup.

### C. Async lifetime & UI guard — `src/app/ui/common/async_runner.cpp`, `network_panel.cpp` (Tier 2)

**C1 — In-flight guard (#8).**
`refresh_network` (`:79`), `scan_wifi` (`:109`), `connect_wifi` (`:131`), and
(after B2) apply each call `run_on_worker` with no guard; repeated clicks spawn
unbounded worker threads. Disable the triggering control (or set a per-action
in-flight flag) until that action's GUI callback returns, then re-enable.

**C2 — Worker lifetime safety (#9).**
`run_on_worker` (`async_runner.cpp:11-15`) creates a self-deleting `QThread` and
its lambda captures the panel by raw `this`; after a long `QProcess` call it
invokes `post_to_gui(this, …)`. If the panel/dialog is destroyed mid-operation,
that dereferences a freed object → use-after-free crash. Fix in two layers:
1. Guard the GUI callback with a validity check on the `QObject` context
   (`QPointer`), so a destroyed target simply skips its callback.
2. Track outstanding workers on the panel and `wait()` for them in the panel's
   destructor, so no worker can outlive its target and the QPointer race window
   is closed. `run_on_worker` returns/records the `QThread` so the owner can join
   it.

## Deferred (Tier 3) — documented, not implemented

- **Reading-table retention.** The `reading` table (schema v9) is append-only
  with no pruning/retention/VACUUM. It is inert today (no `ReadingSink` is wired;
  zero rows written), but its hot-path writer is per-frame (~15 fps/camera). This
  is a hard **prerequisite for the future logging-sink feature**: that feature
  MUST add batching + an age/row-count retention policy before enabling per-frame
  inserts, or it becomes unbounded DB growth.
- **`EngineRegistry::get()` threading invariant.** Unsynchronized `std::map`
  insert; safe *only* because `get()` is called exclusively from the GUI thread
  (`CameraGrid::reload`) after all capture threads are joined, and warm-up is
  joined before the grid is built. Document this invariant in the header; add a
  mutex only if `get()` ever moves off the GUI thread.
- **Model hot-reload.** A changed `.onnx` is not picked up until restart (no
  eviction / mtime check). Bounded, not a leak.
- **Wi-Fi PSK temp file.** The WLAN profile XML with the plaintext PSK is written
  to a fixed temp path and removed after use; a crash between write and remove
  leaks the secret, and concurrent connects race the path. Secret hygiene, low
  stability impact. Fix later with a unique per-call path + scope-guard removal.

## Testing & verification

- **Unit tests** (existing ctest suite) for the three extracted pure helpers:
  `next_backoff_ms`, `safe_process` (throwing-stub processor), `should_emit`.
- **Build gate:** MSYS2 UCRT64 — `cmake --build build` clean, `ctest` green.
- **On-device smoke** (needs real hardware, not unit-testable): pull a
  camera/RTSP source mid-run and confirm the tile goes Offline then reconnects
  when restored; hang/deny a network apply and confirm the UI stays responsive
  and boot still shows the window; long-run watch for flat memory/handle counts.

## Out of scope

No relay/threshold/notification feature (a separate future effort; see
`note.txt`). No new features — this pass only hardens existing behavior.
