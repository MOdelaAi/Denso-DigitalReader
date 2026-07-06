# Soak-Reliability Hardening Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let the app run unattended for hours/days without crashing, wedging, or leaking — recovering on its own from camera drops and stuck OS/network calls.

**Architecture:** Hardening pass over existing behavior. Three extracted pure helpers (`next_backoff_ms`, `should_emit`, `safe_process`) get real TDD; the structural fixes (capture reconnect loop, `QProcess` timeouts, GUI-thread move, async lifetime) are verified by build + the existing Catch2 suite staying green + on-device smoke, since they need real hardware to exercise end-to-end.

**Tech Stack:** C++17, Qt6 Widgets, OpenCV, ONNX Runtime, Catch2 v3, CMake+Ninja.

## Global Constraints

- **Toolchain:** MSYS2 UCRT64. Every build/test runs after `export PATH=/c/msys64/ucrt64/bin:$PATH`.
- **Configure once:** `cmake -S . -B build -G Ninja` (only if `build/` absent). Rebuild: `cmake --build build`. Test: `ctest --test-dir build --output-on-failure`. Single test by tag: `./build/tests/denso_tests "[tag]"`.
- **Test harness:** Catch2 v3. Tests live in `tests/`. A GUI-target `.cpp` that has no Qt-widget dependency is compiled straight into `denso_tests` (see the existing `grid_layout.cpp`/`fps_meter.cpp` entries) with `src/app` on the include path.
- **Style:** This is a 1:1-heritage port — match the surrounding file's comment density, naming (`snake_case` functions, `kConstant`), and idiom. No unrelated refactoring.
- **Scope:** Hardening only. No relay/threshold/notification feature (separate future effort). No behavior change beyond the fixes below.
- **Fixed values (verbatim):** reconnect backoff = 1000 ms initial, ×2 per failed attempt, capped 10000 ms, reset to 1000 ms after a live frame. `kMaxInFlight = 2` (drop-oldest). `kNetCmdTimeoutMs = 15000`, `kNetCmdGraceMs = 2000`.

**Baseline before starting:** run the full suite once and record the count.
```
export PATH=/c/msys64/ucrt64/bin:$PATH
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```
Expected: all tests pass (record the number, e.g. "71 passed").

---

### Task 1: `stream_pacing` pure helpers (`next_backoff_ms`, `should_emit`)

Both are pure integer flow-control helpers for the capture loop — no Qt/OpenCV — so they live in one small unit and get real TDD.

**Files:**
- Create: `src/app/ui/camera/grid/stream_pacing.h`
- Create: `src/app/ui/camera/grid/stream_pacing.cpp`
- Create: `tests/test_stream_pacing.cpp`
- Modify: `src/app/CMakeLists.txt` (add source to `denso` target)
- Modify: `tests/CMakeLists.txt` (add test + source to `denso_tests`)

**Interfaces:**
- Produces:
  - `int denso::ui::next_backoff_ms(int current_ms);` — reconnect backoff schedule. `0`/negative → 1000 (first attempt). Otherwise `min(current_ms * 2, 10000)`.
  - `bool denso::ui::should_emit(int in_flight, int max_in_flight);` — `true` iff `in_flight < max_in_flight` (drop-oldest backpressure gate).

- [ ] **Step 1: Write the failing test**

Create `tests/test_stream_pacing.cpp`:
```cpp
#include <catch2/catch_test_macros.hpp>

#include "ui/camera/grid/stream_pacing.h"

using denso::ui::next_backoff_ms;
using denso::ui::should_emit;

TEST_CASE("next_backoff_ms ramps 1s -> x2 -> 10s cap", "[stream_pacing]") {
    CHECK(next_backoff_ms(0) == 1000);      // first attempt
    CHECK(next_backoff_ms(-5) == 1000);     // negative treated as first
    CHECK(next_backoff_ms(1000) == 2000);
    CHECK(next_backoff_ms(2000) == 4000);
    CHECK(next_backoff_ms(4000) == 8000);
    CHECK(next_backoff_ms(8000) == 10000);  // capped
    CHECK(next_backoff_ms(10000) == 10000); // stays capped
}

TEST_CASE("should_emit gates on in-flight count", "[stream_pacing]") {
    CHECK(should_emit(0, 2));
    CHECK(should_emit(1, 2));
    CHECK_FALSE(should_emit(2, 2));  // at cap -> drop
    CHECK_FALSE(should_emit(3, 2));  // over cap -> drop
}
```

- [ ] **Step 2: Add the new sources to CMake so the test compiles**

In `tests/CMakeLists.txt`, inside the `add_executable(denso_tests ...)` list, after the `test_frame_convert.cpp` block, add:
```cmake
    test_stream_pacing.cpp
    # stream_pacing is GUI-target code but pure int math (no Qt/OpenCV), so
    # compile it in.
    ${CMAKE_SOURCE_DIR}/src/app/ui/camera/grid/stream_pacing.cpp
```

In `src/app/CMakeLists.txt`, immediately after the `ui/camera/grid/camera_stream.cpp` line, add:
```cmake
    ui/camera/grid/stream_pacing.cpp
```

- [ ] **Step 3: Run the test to verify it fails**

```
export PATH=/c/msys64/ucrt64/bin:$PATH
cmake -S . -B build -G Ninja
cmake --build build
```
Expected: FAIL — compile error (`stream_pacing.h`: No such file / `next_backoff_ms` not declared).

- [ ] **Step 4: Write the header**

Create `src/app/ui/camera/grid/stream_pacing.h`:
```cpp
// Pure flow-control helpers for the capture loop: the reconnect backoff schedule
// and the display-queue backpressure gate. No Qt/OpenCV — just int policy, so it
// unit-tests without a camera or the GUI. Used by CameraStream.
#pragma once

namespace denso::ui {

/// Next reconnect delay given the previous one. 0 or negative (the first
/// attempt) returns 1000 ms; otherwise doubles, capped at 10000 ms.
int next_backoff_ms(int current_ms);

/// Whether the capture thread should emit another frame, or drop it: true iff
/// fewer than `max_in_flight` frames are already queued for the GUI (drop-oldest
/// backpressure).
bool should_emit(int in_flight, int max_in_flight);

} // namespace denso::ui
```

- [ ] **Step 5: Write the implementation**

Create `src/app/ui/camera/grid/stream_pacing.cpp`:
```cpp
#include "ui/camera/grid/stream_pacing.h"

#include <algorithm>

namespace denso::ui {

int next_backoff_ms(int current_ms) {
    if (current_ms <= 0) {
        return 1000;
    }
    return std::min(current_ms * 2, 10000);
}

bool should_emit(int in_flight, int max_in_flight) {
    return in_flight < max_in_flight;
}

} // namespace denso::ui
```

- [ ] **Step 6: Run the tests to verify they pass**

```
cmake --build build
./build/tests/denso_tests "[stream_pacing]"
```
Expected: PASS (2 test cases).

- [ ] **Step 7: Run the full suite (no regressions)**

Run: `ctest --test-dir build --output-on-failure`
Expected: baseline count + 2 new, all pass.

- [ ] **Step 8: Commit**

```
git add src/app/ui/camera/grid/stream_pacing.h src/app/ui/camera/grid/stream_pacing.cpp \
        tests/test_stream_pacing.cpp src/app/CMakeLists.txt tests/CMakeLists.txt
git commit -m "feat(stream): pure backoff + backpressure helpers (stream_pacing)"
```

---

### Task 2: `safe_process` exception guard helper

Header-only inline so it needs no CMake source entry; the test subclasses `FrameProcessor` to throw. This is the guard that stops one bad frame from calling `std::terminate()`.

**Files:**
- Create: `src/app/ui/camera/grid/safe_process.h`
- Create: `tests/test_safe_process.cpp`
- Modify: `tests/CMakeLists.txt` (add `test_safe_process.cpp`)

**Interfaces:**
- Consumes: `denso::ui::FrameProcessor` (abstract, `frame_processor.h`) — `QImage process(const QImage&)`.
- Produces: `QImage denso::ui::safe_process(FrameProcessor* p, const QImage& frame) noexcept;` — returns `p->process(frame)`; on any `std::exception` (or null `p`) returns `frame` unchanged. Never throws.

- [ ] **Step 1: Write the failing test**

Create `tests/test_safe_process.cpp`:
```cpp
#include <catch2/catch_test_macros.hpp>

#include "ui/camera/grid/safe_process.h"
#include "ui/camera/grid/frame_processor.h"

#include <QImage>

#include <stdexcept>

using namespace denso::ui;

namespace {
// A processor that always throws — stands in for a malformed-frame cv::Exception.
class ThrowingProcessor : public FrameProcessor {
public:
    QImage process(const QImage&) override {
        throw std::runtime_error("boom");
    }
};
// A processor that inverts the first pixel — proves the result is passed through
// when no exception occurs.
class TaggingProcessor : public FrameProcessor {
public:
    QImage process(const QImage& in) override {
        QImage out = in.copy();
        out.setPixel(0, 0, qRgb(1, 2, 3));
        return out;
    }
};
} // namespace

TEST_CASE("safe_process returns the input frame when the processor throws", "[safe_process]") {
    QImage frame(4, 4, QImage::Format_RGB32);
    frame.fill(qRgb(9, 9, 9));
    ThrowingProcessor p;
    QImage out = safe_process(&p, frame);       // must not throw
    CHECK(out.size() == frame.size());
    CHECK(out.pixel(0, 0) == qRgb(9, 9, 9));     // unchanged input returned
}

TEST_CASE("safe_process passes through a successful result", "[safe_process]") {
    QImage frame(4, 4, QImage::Format_RGB32);
    frame.fill(qRgb(9, 9, 9));
    TaggingProcessor p;
    QImage out = safe_process(&p, frame);
    CHECK(out.pixel(0, 0) == qRgb(1, 2, 3));
}

TEST_CASE("safe_process tolerates a null processor", "[safe_process]") {
    QImage frame(2, 2, QImage::Format_RGB32);
    frame.fill(qRgb(5, 5, 5));
    QImage out = safe_process(nullptr, frame);
    CHECK(out.pixel(0, 0) == qRgb(5, 5, 5));
}
```

- [ ] **Step 2: Register the test in CMake**

In `tests/CMakeLists.txt`, after the `test_stream_pacing.cpp` block from Task 1, add:
```cmake
    test_safe_process.cpp
```
(No extra source needed — `safe_process.h` is header-only and `frame_processor.h`'s only non-value dependency, `inference_engine.h`, needs just `opencv2/core.hpp`, which `denso_tests` already links.)

- [ ] **Step 3: Run to verify it fails**

```
cmake --build build
```
Expected: FAIL — `safe_process.h`: No such file or directory.

- [ ] **Step 4: Write the header**

Create `src/app/ui/camera/grid/safe_process.h`:
```cpp
// Exception firewall for the capture thread. process() runs OpenCV pre/post work
// and model decode outside any try block, in a raw std::thread body — a single
// malformed frame throwing there would escape the thread function and call
// std::terminate(), killing the whole process. safe_process() swallows it: log,
// return the frame unprocessed, keep the capture loop alive.
#pragma once

#include "ui/camera/grid/frame_processor.h"

#include <QImage>

#include <exception>

namespace denso::ui {

inline QImage safe_process(FrameProcessor* p, const QImage& frame) noexcept {
    if (!p) {
        return frame;
    }
    try {
        return p->process(frame);
    } catch (const std::exception&) {
        return frame;  // caller logs (throttled); never let it escape the thread
    } catch (...) {
        return frame;
    }
}

} // namespace denso::ui
```

- [ ] **Step 5: Run the tests to verify they pass**

```
cmake --build build
./build/tests/denso_tests "[safe_process]"
```
Expected: PASS (3 test cases).

- [ ] **Step 6: Commit**

```
git add src/app/ui/camera/grid/safe_process.h tests/test_safe_process.cpp tests/CMakeLists.txt
git commit -m "feat(stream): safe_process exception guard for the capture thread"
```

---

### Task 3: Capture-thread reconnect loop + exception guard + timer-handle RAII

Restructures `CameraStream::run()` into a reconnect loop, routes every frame through `safe_process`, and plugs the waitable-timer handle leak. All three touch `camera_stream.cpp`. Not unit-testable (needs a real camera); verified by build + suite-green + on-device smoke.

**Files:**
- Modify: `src/app/ui/camera/grid/camera_stream.cpp`

**Interfaces:**
- Consumes: `next_backoff_ms` (Task 1), `safe_process` (Task 2).

- [ ] **Step 1: Add includes**

In `src/app/ui/camera/grid/camera_stream.cpp`, alongside the existing grid/shared includes near the top, add:
```cpp
#include "ui/camera/grid/stream_pacing.h"   // next_backoff_ms
#include "ui/camera/grid/safe_process.h"    // safe_process
```

- [ ] **Step 2: Fix the waitable-timer handle leak (A4)**

In the anonymous namespace of `camera_stream.cpp`, inside `precise_sleep`, replace the raw handle:
```cpp
#ifdef _WIN32
    static thread_local HANDLE timer = CreateWaitableTimerExW(
        nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (timer) {
```
with an RAII holder whose destructor runs at thread exit:
```cpp
#ifdef _WIN32
    struct WaitableTimer {
        HANDLE h = CreateWaitableTimerExW(nullptr, nullptr,
                       CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        ~WaitableTimer() { if (h) CloseHandle(h); }
    };
    static thread_local WaitableTimer timer_holder;
    const HANDLE timer = timer_holder.h;
    if (timer) {
```
(The rest of the `#ifdef _WIN32` body — `SetWaitableTimer`/`WaitForSingleObject` — is unchanged; it still references `timer`.)

- [ ] **Step 3: Add an interruptible backoff helper**

Just above `void CameraStream::run()`, add a small member-free helper in the anonymous namespace that waits `ms` while polling a stop flag (reusing `precise_sleep` + the existing 20 ms poll granularity):
```cpp
// Sleep `ms` in stop-responsive chunks; returns early if `stop` flips. Keeps the
// reconnect wait from delaying teardown.
bool wait_or_stop(int ms, const std::atomic<bool>& stop) {
    using namespace std::chrono;
    auto remaining = duration_cast<steady_clock::duration>(milliseconds(ms));
    const auto poll = duration_cast<steady_clock::duration>(milliseconds(kStopPollMs));
    while (remaining > steady_clock::duration::zero()) {
        if (stop.load()) return false;
        const auto chunk = std::min(remaining, poll);
        precise_sleep(chunk);
        remaining -= chunk;
    }
    return !stop.load();
}
```
Add `#include <atomic>` if not already present (the header already declares `std::atomic<bool>`, so it is; confirm).

- [ ] **Step 4: Restructure `run()` into a reconnect loop (A1) + route frames through `safe_process` (A2)**

Replace the entire body of `CameraStream::run()` (from `emit status_changed(... Connecting ...)` through the final `cap.release();`) with:
```cpp
void CameraStream::run() {
    using namespace std::chrono;
    const auto interval = milliseconds(kDisplayIntervalMs);
    const auto poll = duration_cast<steady_clock::duration>(milliseconds(kStopPollMs));
    int backoff_ms = 0;  // 0 -> first attempt is immediate; grows on failure

    while (!stop_.load()) {
        emit status_changed(static_cast<int>(Status::Connecting));

        cv::VideoCapture cap;
        // Fail fast instead of hanging on an unreachable camera (mirrors snapshot).
        const std::vector<int> params = {
            cv::CAP_PROP_OPEN_TIMEOUT_MSEC, 5000,
            cv::CAP_PROP_READ_TIMEOUT_MSEC, 5000,
        };
        if (cam_.camera_type == "usb") {
            const int index = cam_.index ? static_cast<int>(*cam_.index) : 0;
            cap.open(index, cv::CAP_ANY, params);
        } else {
            const QString rtsp = cam_.rtsp ? QString::fromStdString(*cam_.rtsp) : QString();
            const QString user = cam_.username ? QString::fromStdString(*cam_.username) : QString();
            const QString pass = cam_.password ? QString::fromStdString(*cam_.password) : QString();
            const std::string url = with_credentials(rtsp, user, pass).toStdString();
            // Prefer GStreamer (drops stale frames, low latency); fall back to
            // FFMPEG if gst can't open (no plugins on the host).
            cap.open(rtsp_gst_pipeline(url), cv::CAP_GSTREAMER);
            if (!cap.isOpened()) {
                qWarning().noquote() << "[stream]" << QString::fromStdString(cam_.name)
                                     << "GStreamer open failed — falling back to FFMPEG";
                cap.open(url, cv::CAP_FFMPEG, params);
            }
        }

        if (!cap.isOpened()) {
            qWarning().noquote() << "[stream]" << QString::fromStdString(cam_.name)
                                 << "failed to open — retrying";
            emit status_changed(static_cast<int>(Status::Offline));
            backoff_ms = next_backoff_ms(backoff_ms);
            if (!wait_or_stop(backoff_ms, stop_)) break;
            continue;  // reconnect
        }

        // USB-only capture-resolution request (see note in the header history).
        if (cam_.camera_type == "usb" && cam_.width > 0 && cam_.height > 0) {
            cap.set(cv::CAP_PROP_FRAME_WIDTH, cam_.width);
            cap.set(cv::CAP_PROP_FRAME_HEIGHT, cam_.height);
        }
        emit status_changed(static_cast<int>(Status::Live));

        cv::Mat frame;
        while (!stop_.load()) {
            const auto t0 = steady_clock::now();
            if (!cap.read(frame) || frame.empty()) {
                break;  // dropped — fall through to reconnect (was: return)
            }
            backoff_ms = 0;  // a live frame resets the reconnect schedule
            const QImage img = mat_to_qimage(frame);
            emit frame_ready(safe_process(processor_.get(), img));

            // Cap the display rate; chunked sleep stays responsive to stop().
            auto remaining = interval - (steady_clock::now() - t0);
            while (remaining > steady_clock::duration::zero() && !stop_.load()) {
                const auto chunk = std::min(remaining, poll);
                precise_sleep(chunk);
                remaining -= chunk;
            }
        }
        cap.release();

        if (stop_.load()) break;
        // Reached only on a mid-stream drop: signal offline and back off before
        // reopening.
        emit status_changed(static_cast<int>(Status::Offline));
        backoff_ms = next_backoff_ms(backoff_ms);
        if (!wait_or_stop(backoff_ms, stop_)) break;
    }
}
```
Note: the backpressure counter (`queued_`) is wired in Task 4 — this task still uses a plain `emit frame_ready(...)`. Leave the emit as shown here; Task 4 replaces that one line.

- [ ] **Step 5: Build and run the full suite (no regressions)**

```
export PATH=/c/msys64/ucrt64/bin:$PATH
cmake --build build
ctest --test-dir build --output-on-failure
```
Expected: clean build; baseline+2 tests still pass (this task adds no tests).

- [ ] **Step 6: On-device smoke (manual — record result)**

On the target machine: launch the app with a camera live, then pull the source (unplug USB / block the RTSP host). Confirm the tile turns Offline and, on restoring the source within ~10 s, reconnects to Live without an app restart. Also confirm quitting while a camera is Offline exits promptly (no hang). Record: reconnected? teardown prompt?

- [ ] **Step 7: Commit**

```
git add src/app/ui/camera/grid/camera_stream.cpp
git commit -m "fix(stream): auto-reconnect loop, capture-thread exception guard, timer-handle RAII"
```

---

### Task 4: Frame backpressure wiring (drop-oldest)

Adds the shared in-flight counter to `CameraStream`, gates the emit with `should_emit` (Task 1), decrements in the tile, and wires the shared counter in the grid. Verified by build + suite-green (logic already unit-tested).

**Files:**
- Modify: `src/app/ui/camera/grid/camera_stream.h`
- Modify: `src/app/ui/camera/grid/camera_stream.cpp`
- Modify: `src/app/ui/camera/grid/camera_tile.h`
- Modify: `src/app/ui/camera/grid/camera_tile.cpp`
- Modify: `src/app/ui/camera/grid/camera_grid.cpp`

**Interfaces:**
- Produces:
  - `std::shared_ptr<std::atomic<int>> CameraStream::frame_counter() const;`
  - `void CameraTile::set_frame_counter(std::shared_ptr<std::atomic<int>> counter);`

- [ ] **Step 1: Add the counter to `CameraStream` (header)**

In `src/app/ui/camera/grid/camera_stream.h` (`<atomic>`, `<memory>`, `<thread>` are already included), in the `public:` section after `void stop();` add:
```cpp
    /// Shared drop-oldest backpressure counter: frames emitted but not yet
    /// consumed by the tile. Handed to the paired CameraTile so it can decrement.
    std::shared_ptr<std::atomic<int>> frame_counter() const { return queued_; }
```
and in the `private:` members, after `std::atomic<bool> stop_{false};` add:
```cpp
    std::shared_ptr<std::atomic<int>> queued_{
        std::make_shared<std::atomic<int>>(0)};
```

- [ ] **Step 2: Gate the emit in `CameraStream::run()`**

First add `constexpr int kMaxInFlight = 2;` to the file's anonymous-namespace constants, next to `kDisplayIntervalMs`. Then, in the inner read loop from Task 3, replace exactly these two lines:
```cpp
            const QImage img = mat_to_qimage(frame);
            emit frame_ready(safe_process(processor_.get(), img));
```
with the backpressure gate (leave the preceding `backoff_ms = 0;` line from Task 3 exactly as it is — it is not part of this replacement):
```cpp
            if (should_emit(queued_->load(), kMaxInFlight)) {
                const QImage img = mat_to_qimage(frame);
                queued_->fetch_add(1);
                emit frame_ready(safe_process(processor_.get(), img));
            }
            // else: GUI is behind — drop this frame (drop-oldest).
```

- [ ] **Step 3: Add the counter to `CameraTile`**

In `src/app/ui/camera/grid/camera_tile.h`: add `#include <atomic>` and `#include <memory>`, then in `public:` after `set_areas(...)` add:
```cpp
    /// The paired stream's backpressure counter; decremented as each frame is
    /// consumed. Optional — a tile without one just never decrements.
    void set_frame_counter(std::shared_ptr<std::atomic<int>> counter);
```
and in `private:` after `FpsMeter meter_;` add:
```cpp
    std::shared_ptr<std::atomic<int>> frame_counter_;  // null = no backpressure
```

- [ ] **Step 4: Decrement in `CameraTile::set_frame`**

In `camera_tile.cpp`, add the setter and decrement. After the constructor add:
```cpp
void CameraTile::set_frame_counter(std::shared_ptr<std::atomic<int>> counter) {
    frame_counter_ = std::move(counter);
}
```
and change `set_frame` so the counter drops as the frame is taken:
```cpp
void CameraTile::set_frame(const QImage& frame) {
    if (frame_counter_) {
        frame_counter_->fetch_sub(1);  // consumed one queued frame
    }
    frame_ = frame;
    meter_.tick(FpsMeter::clock::now());  // one displayed frame → update the rate
    update();
}
```

- [ ] **Step 5: Wire the shared counter in the grid**

In `src/app/ui/camera/grid/camera_grid.cpp`, in `reload()`, right after the two `connect(stream, ...)` lines (currently ~104–105), add:
```cpp
        tile->set_frame_counter(stream->frame_counter());
```

- [ ] **Step 6: Build and run the full suite**

```
cmake --build build
ctest --test-dir build --output-on-failure
```
Expected: clean build; all tests pass.

- [ ] **Step 7: On-device smoke (manual — record result)**

Run with all four cameras live for a few minutes and confirm the feeds display normally (backpressure never starves a keeping-up GUI: `should_emit` only drops when 2 are already in flight). Optional: drag a modal or stall the GUI and confirm memory does not run away.

- [ ] **Step 8: Commit**

```
git add src/app/ui/camera/grid/camera_stream.h src/app/ui/camera/grid/camera_stream.cpp \
        src/app/ui/camera/grid/camera_tile.h src/app/ui/camera/grid/camera_tile.cpp \
        src/app/ui/camera/grid/camera_grid.cpp
git commit -m "fix(stream): drop-oldest frame backpressure to bound the GUI queue"
```

---

### Task 5: Finite `QProcess` timeout in the network backends

Replaces `waitForFinished(-1)` with a bounded wait + kill in both platform runners. Verified by build + suite-green (shelling out isn't cleanly unit-testable; the existing `test_netsh`/`test_nmcli` cover the pure command/parse builders, which are untouched).

**Files:**
- Modify: `src/core/network/windows/windows_backend.cpp`
- Modify: `src/core/network/linux/linux_backend.cpp`

- [ ] **Step 1: Windows — bound `run()` and `run_checked()`**

In `src/core/network/windows/windows_backend.cpp`, add timeout constants at the top of the anonymous namespace (after `namespace {`):
```cpp
constexpr int kNetCmdTimeoutMs = 15000;  // cap a stuck netsh/ipconfig
constexpr int kNetCmdGraceMs = 2000;     // grace after kill() before giving up
```
In `run()`, replace:
```cpp
    if (!p.waitForStarted()) return {};
    p.waitForFinished(-1);
    return QString::fromUtf8(p.readAllStandardOutput()).toStdString();
```
with:
```cpp
    if (!p.waitForStarted()) return {};
    if (!p.waitForFinished(kNetCmdTimeoutMs)) {
        p.kill();
        p.waitForFinished(kNetCmdGraceMs);
        return {};  // timed out — treat as no output (mirrors spawn failure)
    }
    return QString::fromUtf8(p.readAllStandardOutput()).toStdString();
```
In `run_checked()`, replace:
```cpp
    p.waitForFinished(-1);
    if (p.exitStatus() == QProcess::NormalExit && p.exitCode() == 0) return;
```
with:
```cpp
    if (!p.waitForFinished(kNetCmdTimeoutMs)) {
        p.kill();
        p.waitForFinished(kNetCmdGraceMs);
        throw std::runtime_error("netsh " + args.join(' ').toStdString() +
                                 ": timed out after 15s");
    }
    if (p.exitStatus() == QProcess::NormalExit && p.exitCode() == 0) return;
```

- [ ] **Step 2: Linux — bound `run()`**

In `src/core/network/linux/linux_backend.cpp`, add the same two constants after `namespace {`, then in `run()` replace:
```cpp
    if (!p.waitForStarted()) return {};
    p.waitForFinished(-1);
    return QString::fromUtf8(p.readAllStandardOutput()).toStdString();
```
with:
```cpp
    if (!p.waitForStarted()) return {};
    if (!p.waitForFinished(kNetCmdTimeoutMs)) {
        p.kill();
        p.waitForFinished(kNetCmdGraceMs);
        return {};  // timed out — treat as no output
    }
    return QString::fromUtf8(p.readAllStandardOutput()).toStdString();
```

- [ ] **Step 3: Build and run the full suite**

```
cmake --build build
ctest --test-dir build --output-on-failure
```
Expected: clean build; all tests pass.

- [ ] **Step 4: Commit**

```
git add src/core/network/windows/windows_backend.cpp src/core/network/linux/linux_backend.cpp
git commit -m "fix(network): finite QProcess timeout so a stuck CLI can't hang the thread"
```

---

### Task 6: Async worker lifetime safety

Makes `run_on_worker` track its thread and guard the GUI post against a destroyed target, and has panels wait for outstanding workers on destruction. This must precede Task 7 (which moves more work onto workers). Verified by build + suite-green + manual teardown smoke.

**Files:**
- Modify: `src/app/ui/common/async_runner.h`
- Modify: `src/app/ui/common/async_runner.cpp`
- Modify: `src/app/ui/settings/network_panel.h`
- Modify: `src/app/ui/settings/network_panel.cpp`

**Interfaces:**
- Produces:
  - `QThread* denso::ui::common::run_on_worker(std::function<void()> work);` — now **returns** the worker thread (still self-deletes on finish) so the owner can track/join it.
  - `post_to_gui` unchanged in signature, but callers guard `this` with `QPointer` at the call site.

- [ ] **Step 1: `run_on_worker` returns the thread**

In `async_runner.h`, add `class QThread;` forward declaration and change the declaration:
```cpp
/// Run `work` on a throwaway worker QThread (auto-deleted when it finishes).
/// Returns the QThread* so the caller can track it and wait() on teardown; do
/// not delete it (it self-deletes via deleteLater on finish).
class QThread;
QThread* run_on_worker(std::function<void()> work);
```
In `async_runner.cpp`, change the definition to return the thread:
```cpp
QThread* run_on_worker(std::function<void()> work) {
    auto* thread = QThread::create(std::move(work));
    QObject::connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();
    return thread;
}
```

- [ ] **Step 2: Track workers on the panel and join on destruction**

In `src/app/ui/settings/network_panel.h`: add includes `#include <QPointer>`, `#include <vector>`, and `class QThread;`. Add a private member:
```cpp
    std::vector<QPointer<QThread>> workers_;  // outstanding async workers
```
and declare a destructor:
```cpp
    ~NetworkPanel() override;
```
In `network_panel.cpp`, add the destructor (near the constructor) that waits for any still-running worker so no lambda can post to a freed `this`:
```cpp
NetworkPanel::~NetworkPanel() {
    for (const QPointer<QThread>& w : workers_) {
        if (w && w->isRunning()) {
            w->wait();
        }
    }
}
```

- [ ] **Step 3: Route each handler's worker through the tracker + guard the GUI post**

In `network_panel.cpp`, in `refresh_network()`, `scan_wifi()`, and `connect_wifi()`, change the `common::run_on_worker([...]{ ... })` calls so the thread is recorded and the `post_to_gui(this, ...)` is guarded by a `QPointer`. Pattern (apply to all three), e.g. for `refresh_network`:
```cpp
void NetworkPanel::refresh_network() {
    refresh_btn_->setText(QStringLiteral("Loading…"));
    QPointer<NetworkPanel> self(this);
    workers_.push_back(common::run_on_worker([this, self] {
        const network::NetworkSnapshot snap = network::backend()->snapshot();
        if (!self) return;  // panel gone — skip the post
        common::post_to_gui(this, [this, snap] {
            eth_card_->set_status(to_net_status(snap.ethernet));
            wifi_card_->set_status(to_net_status(snap.wifi));
            refresh_btn_->setText(QStringLiteral("Refresh"));
        });
    }));
}
```
Apply the same two changes (capture `QPointer<NetworkPanel> self`, `if (!self) return;` before `post_to_gui`, and `workers_.push_back(...)` around the call) to `scan_wifi()` and `connect_wifi()`. Prune finished entries opportunistically at the top of each handler:
```cpp
    workers_.erase(std::remove_if(workers_.begin(), workers_.end(),
                                  [](const QPointer<QThread>& w) { return !w; }),
                   workers_.end());
```
(add `#include <algorithm>` to `network_panel.cpp`).

- [ ] **Step 4: Build and run the full suite**

```
cmake --build build
ctest --test-dir build --output-on-failure
```
Expected: clean build; all tests pass.

- [ ] **Step 5: On-device smoke (manual — record result)**

Open Settings → Network, click Refresh/Scan, and immediately close the dialog while the worker is running. Confirm no crash on close (the destructor waits; the guarded post is skipped). Record result.

- [ ] **Step 6: Commit**

```
git add src/app/ui/common/async_runner.h src/app/ui/common/async_runner.cpp \
        src/app/ui/settings/network_panel.h src/app/ui/settings/network_panel.cpp
git commit -m "fix(async): track workers, join on teardown, guard GUI post (no UAF)"
```

---

### Task 7: `apply_config` off the GUI thread + in-flight click guards

Moves the blocking `apply_config` onto a worker (so a stuck `netsh` can't freeze the UI) and disables the triggering controls while an action is in flight. Depends on Task 6. Verified by build + suite-green + manual smoke.

**Files:**
- Modify: `src/app/ui/settings/network_panel.cpp`

- [ ] **Step 1: Move `apply_net_config` onto a worker**

In `network_panel.cpp`, replace the body of `apply_net_config` with a worker-based version mirroring the sibling handlers (persist stays on the GUI thread; the blocking `apply_config` moves off it; the result posts back). Note the counter/guard pattern from Task 6:
```cpp
void NetworkPanel::apply_net_config(const std::string& iface, const NetConfigUi& ui) {
    const network::NetConfig cfg = from_ui_config(iface, ui);
    network::save(db_, cfg);  // app owns the truth; persist before pushing
    const NetConfigUi canonical = to_ui_config(cfg);
    (iface == "wifi" ? wifi_config_ : eth_config_) = canonical;
    NetCard* card = iface == "wifi" ? wifi_card_ : eth_card_;
    card->set_config(canonical);
    card->set_config_status(QStringLiteral("Applying…"));

    QPointer<NetworkPanel> self(this);
    workers_.push_back(common::run_on_worker([this, self, cfg, card] {
        QString status;
        try {
            network::backend()->apply_config(cfg);
            status = QStringLiteral("Applied");
        } catch (const std::exception& e) {
            status = QStringLiteral("Error: %1").arg(QString::fromUtf8(e.what()));
        }
        if (!self) return;
        common::post_to_gui(this, [card, status] { card->set_config_status(status); });
    }));
}
```
(`card` is safe to capture: `NetCard`s outlive the panel-scoped workers because the destructor from Task 6 joins them before any child widget is destroyed.)

- [ ] **Step 2: Guard against overlapping clicks**

Disable each triggering control while its action runs, re-enabling in the GUI post. For `refresh_network`, disable `refresh_btn_` at entry (`refresh_btn_->setEnabled(false)`) and re-enable it inside the posted lambda next to the `setText("Refresh")`. For `scan_wifi`/`connect_wifi`/apply, the `NetCard` already reflects a busy state (`set_scanning`, `set_config_status("Applying…")`, `set_connect_status("Connecting…")`); add a simple guard so a second click during that window is ignored — a per-panel `bool net_busy_ = false;` set true at the start of each handler and cleared in each posted lambda. Add the member to `network_panel.h` (private) and the checks:
```cpp
    // at the top of refresh_network / scan_wifi / connect_wifi / apply_net_config:
    if (net_busy_) return;
    net_busy_ = true;
    // in each handler's posted GUI lambda (the last statement):
    net_busy_ = false;
```
(For `apply_net_config`, still run `network::save` before the `net_busy_` early-return? No — put the `if (net_busy_) return;` as the very first line so a double-click can't double-apply.)

- [ ] **Step 3: Build and run the full suite**

```
cmake --build build
ctest --test-dir build --output-on-failure
```
Expected: clean build; all tests pass.

- [ ] **Step 4: On-device smoke (manual — record result)**

Apply an Ethernet/Wi-Fi config and confirm the UI stays responsive during the apply (no freeze), the status shows "Applying…" then the result, and rapid double-clicks don't launch duplicate applies. Record result.

- [ ] **Step 5: Commit**

```
git add src/app/ui/settings/network_panel.h src/app/ui/settings/network_panel.cpp
git commit -m "fix(network): apply_config off the GUI thread + in-flight click guard"
```

---

### Task 8: Defer + bound the boot network reassert

Moves the synchronous boot `reassert` off the pre-event-loop path so a stuck CLI can't stop the window from appearing. Bounded already by Task 5's timeout. Verified by build + suite-green + manual boot smoke.

**Files:**
- Modify: `src/app/main.cpp`

- [ ] **Step 1: Defer reassert to the first event-loop tick**

In `src/app/main.cpp`, the reassert currently runs before `denso::ui::launch(...)`. `launch` sets up the app and (eventually) calls `app.exec()`. Move the reassert loop into a `QTimer::singleShot(0, ...)` so it runs just after the event loop starts and the main window is shown. Replace the block at lines ~86–91:
```cpp
    // The app owns network config: reassert it to the OS at boot. Non-fatal —
    // a failed apply is logged, never blocks startup.
    for (const auto& [iface, err] : denso::network::reassert(conn, *denso::network::backend())) {
        qWarning().noquote() << QStringLiteral("network: failed to apply %1 config: %2")
                                    .arg(QString::fromStdString(iface), QString::fromStdString(err));
    }
```
with a deferred, best-effort version (add `#include <QTimer>` at the top of `main.cpp`):
```cpp
    // The app owns network config: reassert it to the OS shortly after the UI is
    // up (singleShot(0) → first event-loop tick), not before it. A slow or stuck
    // CLI (now bounded by the backend's QProcess timeout) can no longer keep the
    // window from appearing; failures are logged, never fatal.
    QTimer::singleShot(0, [conn] {
        try {
            for (const auto& [iface, err] :
                 denso::network::reassert(conn, *denso::network::backend())) {
                qWarning().noquote()
                    << QStringLiteral("network: failed to apply %1 config: %2")
                           .arg(QString::fromStdString(iface), QString::fromStdString(err));
            }
        } catch (const std::exception& e) {
            qWarning().noquote() << "network: reassert failed:" << e.what();
        }
    });
```
(`conn` is the `QSqlDatabase` connection value used just below by `launch`; capturing it by value is fine — it is a lightweight handle. Confirm the capture compiles; if `conn`'s type is a reference/name that can't be copied into the lambda, capture the connection name string instead and re-open by name inside.)

- [ ] **Step 2: Build and run the full suite**

```
cmake --build build
ctest --test-dir build --output-on-failure
```
Expected: clean build; all tests pass.

- [ ] **Step 3: On-device smoke (manual — record result)**

Boot the app on the target and confirm the main window appears promptly (reassert no longer blocks startup); check the log shows the reassert running just after launch. Record result.

- [ ] **Step 4: Commit**

```
git add src/app/main.cpp
git commit -m "fix(startup): defer + bound boot network reassert so the UI shows first"
```

---

## Final verification

- [ ] Full suite green: `ctest --test-dir build --output-on-failure` — baseline + 5 new test cases (2 in Task 1, 3 in Task 2), no regressions.
- [ ] Clean build with no new warnings in the touched files.
- [ ] On-device smoke results recorded for Tasks 3, 4, 6, 7, 8 (each needs real hardware).
- [ ] Update the memory checkpoint ([[denso-digitalreader-cpp-port]] / a new soak-hardening note) with commits and the branch (`feature/soak-reliability-hardening`), then delete this plan file per [[plan-cleanup-on-completion]] (keep the spec).

## Spec coverage map

| Spec fix | Task |
|----------|------|
| A1 camera auto-reconnect | 3 (+ backoff helper in 1) |
| A2 capture-thread exception guard | 2 + 3 |
| A3 frame backpressure | 1 (`should_emit`) + 4 |
| A4 waitable-timer handle leak | 3 |
| B1 finite QProcess timeout | 5 |
| B2 apply_config off GUI thread | 7 |
| B3 boot reassert deferred/bounded | 8 |
| C1 in-flight click guard | 7 |
| C2 async worker lifetime safety | 6 |
| Tier 3 (reading retention, registry sync, hot-reload, PSK temp) | deferred — documented in spec, no task |
