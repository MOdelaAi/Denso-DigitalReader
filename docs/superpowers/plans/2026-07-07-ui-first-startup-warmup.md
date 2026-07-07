# UI-First Startup With Background Warm-Up — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Show the main window immediately on launch instead of blocking on model warm-up; start each detection camera's capture thread only once its model(s) finish warming on the background worker.

**Architecture:** A pure, unit-tested `PendingStart` tracks which cameras are waiting on which model files and reports who becomes startable as models go ready. A thin GUI-thread `WarmupState` QObject owns the warm-up worker thread and relays per-model readiness. `CameraGrid` builds all tiles up front, starts model-less/ready cameras immediately, and starts the rest as `WarmupState` signals readiness (flushing the remainder with the existing orientation fallback on finish). The heavy engine build stays on the warm-up worker — the "no build on a capture thread" rule is preserved by gating each detection camera's start on its models being ready.

**Tech Stack:** C++17, Qt6 (Core/Widgets), OpenCV, ONNX Runtime, Catch2 v3, CMake + Ninja, MSYS2 UCRT64.

## Global Constraints

- Toolchain: MSYS2 UCRT64. Build: `export PATH=/c/msys64/ucrt64/bin:$PATH; cmake -S . -B build -G Ninja; cmake --build build; ctest --test-dir build`. (On PowerShell: `$env:PATH="C:\msys64\ucrt64\bin;$env:PATH"; cmake --build build; if ($?) { ctest --test-dir build }`.)
- **Hard rule — no engine build on a capture thread.** The minutes-long, non-interruptible TensorRT build must run only on the warm-up worker. A detection camera's `CameraStream` (capture thread) must not be created until every model it needs is ready, so `EngineRegistry::get()` for it is a cache-hit lookup, never a build.
- `denso_core` must not link `Qt6::Widgets`; this feature is app-layer only (`src/app/…`).
- The TensorRT engine/timing cache config in `ort_engine.cpp` is reused as-is — do not add another cache.
- The test target (`denso_tests`) links `denso_core` + Catch2 + OpenCV + `Qt6::Gui` only (**no Widgets**). `test_main.cpp` provides a `QCoreApplication`. So: pure/Core-only units are unit-tested; `QWidget`-derived units (`CameraGrid`, `CameraTile`, `CameraView`, `MainWindow`, `startup`) are **build-gated + on-device smoke**, not unit-tested — mirror the repo's pure-logic-tested / thin-Qt-shell-untested split.
- Follow existing patterns: `EngineRegistry::get()` is currently GUI-thread-only; it becomes GUI-thread + warm-up-worker, so it gains a mutex.
- Failed model (won't load) → never reported ready → camera falls back to `OrientationProcessor` on warm-up finish, exactly as today when `runs.empty()`.

---

## File Structure

- Create `src/app/ui/camera/grid/warmup_gate.{h,cpp}` — pure `PendingStart` gating logic.
- Create `tests/test_warmup_gate.cpp` — Catch2 unit tests (pure).
- Create `src/app/ui/warmup_state.{h,cpp}` — GUI-thread `WarmupState` QObject (owns the worker thread, relays readiness).
- Modify `src/app/ui/warmup_worker.{h,cpp}` — add `model_ready(QString)` signal; drive it from a new `on_ready` warm-up callback.
- Modify `src/app/ui/camera/shared/detection/engine_registry.{h,cpp}` — add a `std::mutex` around `get()`; add an `on_ready` callback param to `warm_up`; update the stale threading note.
- Modify `src/app/ui/camera/grid/camera_tile.{h,cpp}` — a "Preparing model…" placeholder state.
- Modify `src/app/ui/camera/grid/camera_grid.{h,cpp}` — hold a `WarmupState*`; defer detection-camera start via `PendingStart`; connect `model_ready`/`finished`.
- Modify `src/app/ui/camera/camera_view.{h,cpp}` — thread a `WarmupState*` through to `CameraGrid`.
- Modify `src/app/ui/mainwindow.{h,cpp}` — accept a `WarmupState*` and pass it to `CameraView`.
- Modify `src/app/ui/startup.{h,cpp}` — build + show `MainWindow` immediately; create `WarmupState` and start the background warm-up; retire the blocking `StartupScreen`.
- Delete `src/app/ui/startup_screen.{h,cpp}` (only `startup.cpp` used it).
- Modify `src/app/CMakeLists.txt` — add `warmup_state.cpp`, `warmup_gate.cpp`; remove `startup_screen.cpp`.
- Modify `tests/CMakeLists.txt` — add `test_warmup_gate.cpp` + `warmup_gate.cpp`.
- Modify `CLAUDE.md`, `docs/ARCHITECTURE.md` — document the UI-first startup + per-camera warm gating.

---

## Task 1: `PendingStart` (pure gating unit)

**Files:**
- Create: `src/app/ui/camera/grid/warmup_gate.h`
- Create: `src/app/ui/camera/grid/warmup_gate.cpp`
- Test: `tests/test_warmup_gate.cpp`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: nothing (std only).
- Produces: `class denso::ui::PendingStart` with:
  - `void add(int64_t camera_id, std::vector<std::string> not_yet_ready_models);`
  - `std::vector<int64_t> ready(const std::string& model);`
  - `std::vector<int64_t> drain();`
  - `bool empty() const;`

- [ ] **Step 1: Write the header**

Create `src/app/ui/camera/grid/warmup_gate.h`:

```cpp
// Pure gating for UI-first startup: which cameras are still waiting on detection
// models to finish warming, and which become startable as each model goes ready.
// No Qt — unit-tested like the other grid policy units. The CameraGrid holds the
// per-camera Qt/data and calls in here to decide when to start a stream.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace denso::ui {

class PendingStart {
public:
    /// Register a camera that cannot start until every file in
    /// `not_yet_ready_models` has been reported ready. Pass only the models that
    /// are NOT already ready at registration time (a camera with none left to
    /// wait for should be started immediately by the caller, not registered).
    void add(int64_t camera_id, std::vector<std::string> not_yet_ready_models);

    /// Mark one model file ready. Returns the camera_ids that are now fully
    /// satisfied (their last outstanding model just went ready); those cameras
    /// are removed from tracking. Order preserved by registration.
    std::vector<int64_t> ready(const std::string& model);

    /// Remove and return every still-pending camera_id (used on warm-up finish
    /// to start the remainder with a fallback processor).
    std::vector<int64_t> drain();

    bool empty() const { return entries_.empty(); }

private:
    struct Entry {
        int64_t camera_id;
        std::vector<std::string> remaining;  // models still not ready
    };
    std::vector<Entry> entries_;
};

} // namespace denso::ui
```

- [ ] **Step 2: Write the failing tests**

Create `tests/test_warmup_gate.cpp`:

```cpp
#include "ui/camera/grid/warmup_gate.h"

#include <catch2/catch_test_macros.hpp>

using denso::ui::PendingStart;
using Ids = std::vector<int64_t>;

TEST_CASE("a single-model camera starts when its model is ready") {
    PendingStart p;
    p.add(7, {"a.onnx"});
    REQUIRE(p.ready("b.onnx") == Ids{});      // unrelated model
    REQUIRE(p.ready("a.onnx") == Ids{7});     // now satisfied
    REQUIRE(p.empty());
    REQUIRE(p.ready("a.onnx") == Ids{});      // already removed
}

TEST_CASE("a multi-model camera waits for all its models") {
    PendingStart p;
    p.add(1, {"a.onnx", "b.onnx"});
    REQUIRE(p.ready("a.onnx") == Ids{});      // still waiting on b
    REQUIRE(p.ready("b.onnx") == Ids{1});     // both ready now
    REQUIRE(p.empty());
}

TEST_CASE("one ready model can satisfy several cameras at once") {
    PendingStart p;
    p.add(1, {"m.onnx"});
    p.add(2, {"m.onnx"});
    p.add(3, {"m.onnx", "n.onnx"});
    REQUIRE(p.ready("m.onnx") == Ids{1, 2});  // 3 still needs n
    REQUIRE_FALSE(p.empty());
    REQUIRE(p.ready("n.onnx") == Ids{3});
    REQUIRE(p.empty());
}

TEST_CASE("drain returns the remaining cameras for fallback") {
    PendingStart p;
    p.add(1, {"a.onnx"});
    p.add(2, {"b.onnx"});
    p.ready("a.onnx");                         // 1 started
    REQUIRE(p.drain() == Ids{2});             // 2 never got its model
    REQUIRE(p.empty());
    REQUIRE(p.drain() == Ids{});
}

TEST_CASE("duplicate ready of an already-satisfied model is a no-op") {
    PendingStart p;
    p.add(1, {"a.onnx"});
    REQUIRE(p.ready("a.onnx") == Ids{1});
    REQUIRE(p.ready("a.onnx") == Ids{});
}
```

- [ ] **Step 3: Register the test + source in CMake**

In `tests/CMakeLists.txt`, after the `brazing_retry_policy.cpp` block (near the end of the `add_executable(denso_tests …)` list), add:

```cmake
    test_warmup_gate.cpp
    # warmup_gate is GUI-target code but pure std (no Qt/OpenCV), so compile it in.
    ${CMAKE_SOURCE_DIR}/src/app/ui/camera/grid/warmup_gate.cpp
```

- [ ] **Step 4: Run tests to verify they fail**

Run: `export PATH=/c/msys64/ucrt64/bin:$PATH; cmake -S . -B build -G Ninja; cmake --build build 2>&1 | tail -20`
Expected: compile/link failure — `warmup_gate.cpp` does not exist yet.

- [ ] **Step 5: Write the implementation**

Create `src/app/ui/camera/grid/warmup_gate.cpp`:

```cpp
#include "ui/camera/grid/warmup_gate.h"

#include <algorithm>
#include <utility>

namespace denso::ui {

void PendingStart::add(int64_t camera_id,
                       std::vector<std::string> not_yet_ready_models) {
    entries_.push_back({camera_id, std::move(not_yet_ready_models)});
}

std::vector<int64_t> PendingStart::ready(const std::string& model) {
    std::vector<int64_t> satisfied;
    for (Entry& e : entries_) {
        auto it = std::find(e.remaining.begin(), e.remaining.end(), model);
        if (it != e.remaining.end()) {
            e.remaining.erase(it);
        }
        if (e.remaining.empty()) {
            satisfied.push_back(e.camera_id);
        }
    }
    entries_.erase(
        std::remove_if(entries_.begin(), entries_.end(),
                       [](const Entry& e) { return e.remaining.empty(); }),
        entries_.end());
    return satisfied;
}

std::vector<int64_t> PendingStart::drain() {
    std::vector<int64_t> ids;
    ids.reserve(entries_.size());
    for (const Entry& e : entries_) {
        ids.push_back(e.camera_id);
    }
    entries_.clear();
    return ids;
}

} // namespace denso::ui
```

Note: an entry added with an empty `remaining` (caller error — such a camera should be started directly) would be reported satisfied on the very next `ready()` call. The caller (Task 6) never registers empty-model cameras, so this does not arise.

- [ ] **Step 6: Run tests to verify they pass**

Run: `cmake --build build 2>&1 | tail -5; ctest --test-dir build --output-on-failure 2>&1 | tail -15`
Expected: all `warmup_gate` cases PASS; full suite green.

- [ ] **Step 7: Commit**

```bash
git add src/app/ui/camera/grid/warmup_gate.h src/app/ui/camera/grid/warmup_gate.cpp tests/test_warmup_gate.cpp tests/CMakeLists.txt
git commit -m "feat(startup): pure PendingStart gate (which cameras start as models warm)"
```

---

## Task 2: `EngineRegistry` — mutex + `on_ready` callback

**Files:**
- Modify: `src/app/ui/camera/shared/detection/engine_registry.h`
- Modify: `src/app/ui/camera/shared/detection/engine_registry.cpp`

**Interfaces:**
- Consumes: nothing new.
- Produces:
  - `InferenceEngine* get(const std::string& filename);` — now internally mutex-guarded (signature unchanged).
  - `void warm_up(std::function<void(const std::string&)> on_model = {}, std::function<void(const std::string&)> on_ready = {});` — `on_ready(filename)` fires **after** a model's engine is loaded + blank-inferred.

- [ ] **Step 1: Update the header**

In `src/app/ui/camera/shared/detection/engine_registry.h`:
- Replace the class-comment's threading note (the lines starting "Not internally synchronized: warm_up() builds…") with:

```cpp
// One shared inference engine per distinct model file. Cameras that attach the
// same model reuse a single Ort::Session (loaded lazily on first request), so
// N cameras on the same model pay for one load, not N. Owns the engines; hand
// out non-owning pointers (never erased, so the pointers stay valid). get() is
// mutex-guarded: it is called from both the warm-up worker (during warm_up) and
// the GUI thread (starting a camera whose models are ready), so the map must be
// synchronized. infer() holds the raw engine pointer and never touches the
// registry, so there is no per-frame locking.
```

- Add the include `#include <mutex>` (near `<map>`).
- Change the `warm_up` declaration to:

```cpp
    /// Load AND warm (one blank inference) every *.onnx in models_dir, on the
    /// warm-up worker thread. `on_model(filename)` fires just before each model is
    /// prepared (progress display); `on_ready(filename)` fires just after it is
    /// successfully loaded + warmed (so the UI can start cameras that use it). A
    /// model that fails to load fires neither on_ready nor a start. Blocking.
    void warm_up(std::function<void(const std::string&)> on_model = {},
                 std::function<void(const std::string&)> on_ready = {});
```

- Add a private member: `std::mutex mutex_;` (below `engines_`).

- [ ] **Step 2: Update the implementation**

In `src/app/ui/camera/shared/detection/engine_registry.cpp`:
- Add `#include <mutex>` at the top (with the other includes).
- Wrap the body of `get()` in a lock:

```cpp
InferenceEngine* EngineRegistry::get(const std::string& filename) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = engines_.find(filename);
    if (it == engines_.end()) {
        auto eng = std::make_unique<OrtEngine>(models_dir_ + "/" + filename,
                                               cache_dir_);
        it = engines_.emplace(filename, std::move(eng)).first;
    }
    OrtEngine* e = it->second.get();
    return (e && e->ok()) ? e : nullptr;
}
```

- Change `warm_up`'s signature and fire `on_ready` after a successful warm. Replace the loop body's success branch:

```cpp
void EngineRegistry::warm_up(std::function<void(const std::string&)> on_model,
                             std::function<void(const std::string&)> on_ready) {
```

and inside the loop, where it currently logs `[warmup] ready`:

```cpp
        if (InferenceEngine* e = get(filename)) {
            e->infer(blank);  // build/load the engine + warm kernels; result discarded
            qInfo().noquote() << "[warmup] ready" << name;
            if (on_ready) {
                on_ready(filename);
            }
        } else {
            qWarning().noquote() << "[warmup] failed to load" << name;
        }
```

- [ ] **Step 3: Build + run the suite**

Run: `cmake --build build 2>&1 | tail -20; ctest --test-dir build --output-on-failure 2>&1 | tail -10`
Expected: clean build; suite green (existing `warm_up` callers still compile — the new param defaults to `{}`).

- [ ] **Step 4: Commit**

```bash
git add src/app/ui/camera/shared/detection/engine_registry.h src/app/ui/camera/shared/detection/engine_registry.cpp
git commit -m "feat(startup): EngineRegistry mutex-guards get(); warm_up gains on_ready callback"
```

---

## Task 3: `WarmupWorker` — `model_ready` signal

**Files:**
- Modify: `src/app/ui/warmup_worker.h`
- Modify: `src/app/ui/warmup_worker.cpp`

**Interfaces:**
- Consumes: `EngineRegistry::warm_up(on_model, on_ready)` (Task 2).
- Produces: `WarmupWorker` now also emits `void model_ready(const QString& filename);`.

- [ ] **Step 1: Add the signal in the header**

In `src/app/ui/warmup_worker.h`, add to the `signals:` block (after `progress`):

```cpp
    void model_ready(const QString& filename);
```

- [ ] **Step 2: Emit it from run()**

In `src/app/ui/warmup_worker.cpp`, pass a second callback to `warm_up`:

```cpp
void WarmupWorker::run() {
    if (engines_) {
        engines_->warm_up(
            [this](const std::string& name) {
                emit progress(QStringLiteral("Preparing model %1…")
                                  .arg(QString::fromStdString(name)));
            },
            [this](const std::string& name) {
                emit model_ready(QString::fromStdString(name));
            });
    }
    emit finished();
}
```

- [ ] **Step 3: Build**

Run: `cmake --build build 2>&1 | tail -10`
Expected: clean build (no test change — this is exercised via Task 4/6 + smoke).

- [ ] **Step 4: Commit**

```bash
git add src/app/ui/warmup_worker.h src/app/ui/warmup_worker.cpp
git commit -m "feat(startup): WarmupWorker emits model_ready per warmed model"
```

---

## Task 4: `WarmupState` (GUI-thread relay + worker owner)

**Files:**
- Create: `src/app/ui/warmup_state.h`
- Create: `src/app/ui/warmup_state.cpp`
- Modify: `src/app/CMakeLists.txt`

**Interfaces:**
- Consumes: `EngineRegistry` (shared_ptr), `WarmupWorker` (Task 3).
- Produces: `class denso::ui::WarmupState : public QObject` with:
  - `explicit WarmupState(std::shared_ptr<EngineRegistry> engines, QObject* parent = nullptr);`
  - `~WarmupState() override;`
  - `void start();` — spins up the worker thread.
  - `bool is_ready(const std::string& filename) const;`
  - `bool is_complete() const;`
  - signals: `void model_ready(const QString& filename);`, `void finished();`

- [ ] **Step 1: Write the header**

Create `src/app/ui/warmup_state.h`:

```cpp
// GUI-thread owner of the background warm-up: spins up the WarmupWorker on its
// own QThread, records which model files have finished warming, and re-emits
// per-model readiness + completion on the GUI thread. The CameraGrid subscribes
// here (and queries is_ready/is_complete at reload) to start each detection
// camera as its models come ready. Records readiness in a set AND emits, so a
// model that finishes before a subscriber connects is not missed.
#pragma once

#include "ui/camera/shared/detection/engine_registry.h"

#include <QObject>
#include <QString>

#include <memory>
#include <set>
#include <string>

class QThread;

namespace denso::ui {

class WarmupWorker;

class WarmupState : public QObject {
    Q_OBJECT

public:
    explicit WarmupState(std::shared_ptr<EngineRegistry> engines,
                         QObject* parent = nullptr);
    ~WarmupState() override;

    /// Start warming on the background thread. Call once, after subscribers have
    /// connected (or rely on is_ready/is_complete for anything that races).
    void start();

    bool is_ready(const std::string& filename) const;
    bool is_complete() const { return complete_; }

signals:
    void model_ready(const QString& filename);
    void finished();

private slots:
    void on_model_ready(const QString& filename);
    void on_finished();

private:
    std::shared_ptr<EngineRegistry> engines_;
    QThread* thread_ = nullptr;
    WarmupWorker* worker_ = nullptr;
    std::set<std::string> ready_;
    bool complete_ = false;
};

} // namespace denso::ui
```

- [ ] **Step 2: Write the implementation**

Create `src/app/ui/warmup_state.cpp`:

```cpp
#include "ui/warmup_state.h"

#include "ui/warmup_worker.h"

#include <QThread>

#include <utility>

namespace denso::ui {

WarmupState::WarmupState(std::shared_ptr<EngineRegistry> engines, QObject* parent)
    : QObject(parent), engines_(std::move(engines)) {}

WarmupState::~WarmupState() {
    if (thread_) {
        thread_->quit();
        thread_->wait();  // join the warm-up worker before we (and it) die
    }
}

void WarmupState::start() {
    if (thread_) {
        return;  // already started
    }
    thread_ = new QThread(this);
    worker_ = new WarmupWorker(engines_);
    worker_->moveToThread(thread_);

    connect(thread_, &QThread::started, worker_, &WarmupWorker::run);
    connect(worker_, &WarmupWorker::model_ready, this, &WarmupState::on_model_ready);
    connect(worker_, &WarmupWorker::finished, this, &WarmupState::on_finished);
    // Clean up the worker when the thread finishes; the thread is a child of this.
    connect(thread_, &QThread::finished, worker_, &QObject::deleteLater);

    thread_->start();
}

void WarmupState::on_model_ready(const QString& filename) {
    ready_.insert(filename.toStdString());
    emit model_ready(filename);
}

void WarmupState::on_finished() {
    complete_ = true;
    thread_->quit();  // let the thread wind down; worker deleteLater's on finished
    emit finished();
}

bool WarmupState::is_ready(const std::string& filename) const {
    return ready_.count(filename) > 0;
}

} // namespace denso::ui
```

- [ ] **Step 3: Register the source in CMake**

In `src/app/CMakeLists.txt`, add near the other `ui/warmup_*`/`ui/startup` entries:

```cmake
    ui/warmup_state.cpp
```

- [ ] **Step 4: Build**

Run: `cmake --build build 2>&1 | tail -15`
Expected: clean build.

- [ ] **Step 5: Commit**

```bash
git add src/app/ui/warmup_state.h src/app/ui/warmup_state.cpp src/app/CMakeLists.txt
git commit -m "feat(startup): WarmupState — owns warm-up thread, relays model readiness"
```

---

## Task 5: `CameraTile` — "Preparing" placeholder

**Files:**
- Modify: `src/app/ui/camera/grid/camera_tile.h`
- Modify: `src/app/ui/camera/grid/camera_tile.cpp`

**Interfaces:**
- Consumes: nothing new.
- Produces: `void CameraTile::set_preparing(bool on);` — shows a "Preparing model…" placeholder until the stream is attached.

- [ ] **Step 1: Add the API in the header**

In `src/app/ui/camera/grid/camera_tile.h`:
- Add a public method after `set_frame_counter`:

```cpp
    /// Show a "Preparing model…" placeholder while this camera's detection model
    /// warms in the background. Cleared once the stream is attached (set_status).
    void set_preparing(bool on);
```

- Add a private member near `status_`:

```cpp
    bool preparing_ = false;  // true = model still warming, no stream yet
```

- [ ] **Step 2: Implement it**

In `src/app/ui/camera/grid/camera_tile.cpp`:
- Add the setter (near `set_status`):

```cpp
void CameraTile::set_preparing(bool on) {
    preparing_ = on;
    update();
}

void CameraTile::set_status(int status) {
    preparing_ = false;  // a real stream is now driving this tile
    status_ = status;
    update();
}
```

(If `set_status` already exists with a body, add the `preparing_ = false;` line at its top rather than duplicating it.)

- In `paintEvent`, in the placeholder branch (where it draws the connecting/offline placeholder text), show the preparing message when `preparing_` is set. Find the placeholder text selection and make it honor `preparing_` first, e.g.:

```cpp
    // (inside the no-frame placeholder branch)
    const QString msg = preparing_ ? QStringLiteral("Preparing model…")
                        : (status_ == static_cast<int>(CameraStream::Status::Offline)
                               ? QStringLiteral("Offline")
                               : QStringLiteral("Connecting…"));
```

Use `msg` where the placeholder currently draws its status word. (Match the existing include of `camera_stream.h` for the `Status` enum; if it isn't already included, include it.)

- [ ] **Step 3: Build**

Run: `cmake --build build 2>&1 | tail -15`
Expected: clean build. (Verified visually in the on-device smoke.)

- [ ] **Step 4: Commit**

```bash
git add src/app/ui/camera/grid/camera_tile.h src/app/ui/camera/grid/camera_tile.cpp
git commit -m "feat(startup): CameraTile shows a Preparing placeholder while a model warms"
```

---

## Task 6: `CameraGrid` — deferred per-camera start

**Files:**
- Modify: `src/app/ui/camera/grid/camera_grid.h`
- Modify: `src/app/ui/camera/grid/camera_grid.cpp`

**Interfaces:**
- Consumes: `PendingStart` (Task 1), `WarmupState` (Task 4), `CameraTile::set_preparing` (Task 5), existing `detection::detection_for`, `OrientationProcessor`/`DetectionProcessor`, `EngineRegistry::get`.
- Produces: `CameraGrid(QSqlDatabase, std::shared_ptr<EngineRegistry>, WarmupState*, QWidget*)`.

- [ ] **Step 1: Header — constructor + members**

In `src/app/ui/camera/grid/camera_grid.h`:
- Add forward declaration `class WarmupState;` (near `class ZoneReporter;`).
- Add `#include "ui/camera/grid/warmup_gate.h"` and `#include <map>` and `#include <string>`.
- Change the constructor to:

```cpp
    explicit CameraGrid(QSqlDatabase db, std::shared_ptr<EngineRegistry> engines,
                        WarmupState* warmup, QWidget* parent = nullptr);
```

- Add private members:

```cpp
    WarmupState* warmup_ = nullptr;   // per-model warm readiness (not owned)
    PendingStart pending_;            // detection cams waiting on their models
    // Data needed to build a pending camera's stream once its models are ready.
    struct PendingCam {
        camera::Camera cam;
        CameraTile* tile;
    };
    std::map<int64_t, PendingCam> pending_cams_;
```

- Add private methods:

```cpp
    void start_one(const camera::Camera& cam, CameraTile* tile);  // build proc+stream, start
    void on_model_ready(const QString& filename);
    void on_warmup_finished();
```

- Add `#include "camera/camera.h"` if not already present (for `camera::Camera`).

- [ ] **Step 2: Implementation — constructor + reload gating**

In `src/app/ui/camera/grid/camera_grid.cpp`:
- Add includes: `#include "ui/warmup_state.h"` and `#include <QString>` (if not present).
- Update the constructor to store `warmup_` and connect its signals:

```cpp
CameraGrid::CameraGrid(QSqlDatabase db, std::shared_ptr<EngineRegistry> engines,
                       WarmupState* warmup, QWidget* parent)
    : QWidget(parent), db_(std::move(db)), engines_(std::move(engines)),
      warmup_(warmup) {
    grid_ = new QGridLayout(this);
    grid_->setContentsMargins(0, 0, 0, 0);
    grid_->setSpacing(0);
    if (warmup_) {
        connect(warmup_, &WarmupState::model_ready, this, &CameraGrid::on_model_ready);
        connect(warmup_, &WarmupState::finished, this, &CameraGrid::on_warmup_finished);
    }
}
```

- In `clear()`, also reset the pending bookkeeping (add near the top, before deleting tiles/streams — the tiles are deleted below so just drop the dangling pointers):

```cpp
    pending_ = PendingStart{};
    pending_cams_.clear();
```

- Rewrite the per-camera body of `reload()`. Replace the loop that builds `proc`/`stream` for each camera with the following gating logic. Keep the tile creation, `set_areas`, and grid placement exactly as before; change only how the processor/stream is decided:

```cpp
        auto* tile = new CameraTile(QString::fromStdString(cam.name));
        std::vector<camera::CameraArea> areas = camera::areas_for(db_, cam.id);
        tile->set_areas(areas);

        grid_->addWidget(tile, i / dims.cols, i % dims.cols);
        tiles_.push_back(tile);

        const detection::CameraDetection det = detection::detection_for(db_, cam.id);
        if (det.models.empty() || warmup_ == nullptr) {
            // No detection (or no warm-up coordinator): start immediately, exactly
            // as before. DetectionProcessor construction below handles the models.
            start_one(cam, tile);
            continue;
        }
        // Which of this camera's models are not yet warm?
        std::vector<std::string> waiting;
        for (const detection::ResolvedModel& rm : det.models) {
            if (!warmup_->is_ready(rm.filename)) {
                waiting.push_back(rm.filename);
            }
        }
        if (waiting.empty()) {
            start_one(cam, tile);  // all models already warm → cache-hit get()
        } else {
            tile->set_preparing(true);
            pending_cams_[cam.id] = PendingCam{cam, tile};
            pending_.add(cam.id, std::move(waiting));
        }
```

- Remove the trailing `start_streams();` call at the end of `reload()` **only if** it would double-start; instead, since streams are now created + started per camera in `start_one`, replace the final `start_streams();` with nothing (each `start_one` starts its own stream). Keep the `relayout_letterbox();` call.

- [ ] **Step 3: Implementation — start_one + warm-up handlers**

Add these methods to `camera_grid.cpp` (factor the processor-building that used to live inline in `reload()` into `start_one`):

```cpp
void CameraGrid::start_one(const camera::Camera& cam, CameraTile* tile) {
    std::vector<camera::CameraArea> areas = camera::areas_for(db_, cam.id);
    const detection::CameraDetection det = detection::detection_for(db_, cam.id);

    std::unique_ptr<FrameProcessor> proc;
    if (det.models.empty()) {
        proc = std::make_unique<OrientationProcessor>(
            static_cast<int>(cam.rotation), cam.pitch, cam.roll);
    } else {
        std::vector<DetectionProcessor::ModelRun> runs;
        for (const detection::ResolvedModel& rm : det.models) {
            InferenceEngine* eng = engines_->get(rm.filename);  // cache-hit here
            if (!eng) continue;  // model failed to load — skip it
            runs.push_back({eng, rm.class_names, rm.classes});
        }
        if (runs.empty()) {
            proc = std::make_unique<OrientationProcessor>(
                static_cast<int>(cam.rotation), cam.pitch, cam.roll);
        } else {
            proc = std::make_unique<DetectionProcessor>(
                static_cast<int>(cam.rotation), cam.pitch, cam.roll,
                std::move(runs), std::move(areas), cam.id,
                /*ReadingSink*/ nullptr, /*ZoneSink*/ reporter_.get());
        }
    }
    tile->set_preparing(false);
    auto* stream = new CameraStream(cam, std::move(proc));
    connect(stream, &CameraStream::frame_ready, tile, &CameraTile::set_frame);
    connect(stream, &CameraStream::status_changed, tile, &CameraTile::set_status);
    tile->set_frame_counter(stream->frame_counter());
    streams_.push_back(stream);
    stream->start();
}

void CameraGrid::on_model_ready(const QString& filename) {
    const std::vector<int64_t> ids = pending_.ready(filename.toStdString());
    for (int64_t id : ids) {
        auto it = pending_cams_.find(id);
        if (it == pending_cams_.end()) continue;
        start_one(it->second.cam, it->second.tile);
        pending_cams_.erase(it);
    }
}

void CameraGrid::on_warmup_finished() {
    // Any camera still waiting has a model that never loaded → start with whatever
    // resolved (start_one falls back to OrientationProcessor when no model loads).
    for (int64_t id : pending_.drain()) {
        auto it = pending_cams_.find(id);
        if (it == pending_cams_.end()) continue;
        start_one(it->second.cam, it->second.tile);
        pending_cams_.erase(it);
    }
}
```

Note the `runs` build in `start_one` calls `engines_->get()` on the GUI thread; for a pending camera this runs only after `WarmupState` reported all its models ready, so `get()` is a cache-hit (no build). For an immediate-start detection camera (all models already warm, or `warmup_ == nullptr`), `get()` is likewise a cache hit after warm-up, or — in the `warmup_ == nullptr` unit/test path — behaves exactly as the old code did.

- [ ] **Step 4: Build + run the suite**

Run: `cmake --build build 2>&1 | tail -20; ctest --test-dir build --output-on-failure 2>&1 | tail -10`
Expected: clean build; suite green (grid is not unit-tested; this compiles it).

- [ ] **Step 5: Commit**

```bash
git add src/app/ui/camera/grid/camera_grid.h src/app/ui/camera/grid/camera_grid.cpp
git commit -m "feat(startup): CameraGrid defers detection-camera start until models warm"
```

---

## Task 7: Thread `WarmupState*` through `CameraView` + `MainWindow`

**Files:**
- Modify: `src/app/ui/camera/camera_view.h`
- Modify: `src/app/ui/camera/camera_view.cpp`
- Modify: `src/app/ui/mainwindow.h`
- Modify: `src/app/ui/mainwindow.cpp`

**Interfaces:**
- Consumes: `WarmupState` (Task 4), `CameraGrid(db, engines, warmup, parent)` (Task 6).
- Produces: `CameraView(db, engines, WarmupState* warmup, parent)` and `MainWindow(db, state, engines, WarmupState* warmup, parent)`.

- [ ] **Step 1: CameraView header**

In `src/app/ui/camera/camera_view.h`:
- Add forward decl `class WarmupState;`.
- Change the constructor to:

```cpp
    explicit CameraView(QSqlDatabase db, std::shared_ptr<EngineRegistry> engines,
                        WarmupState* warmup, QWidget* parent = nullptr);
```

- Add a private member: `WarmupState* warmup_ = nullptr;`.

- [ ] **Step 2: CameraView impl**

In `src/app/ui/camera/camera_view.cpp`:
- Update the constructor signature + init list to store `warmup_`, and pass it to the grid:

```cpp
CameraView::CameraView(QSqlDatabase db, std::shared_ptr<EngineRegistry> engines,
                       WarmupState* warmup, QWidget* parent)
    : QWidget(parent), db_(std::move(db)), engines_(std::move(engines)),
      warmup_(warmup) {
```

- Change the grid construction line to:

```cpp
    grid_ = new CameraGrid(db_, engines_, warmup_);
```

- [ ] **Step 3: MainWindow header**

In `src/app/ui/mainwindow.h`:
- Add forward decl `class WarmupState;`.
- Change the constructor to:

```cpp
    MainWindow(QSqlDatabase db, std::shared_ptr<settings::Settings> state,
               std::shared_ptr<EngineRegistry> engines, WarmupState* warmup,
               QWidget* parent = nullptr);
```

- Add a private member: `WarmupState* warmup_ = nullptr;`.

- [ ] **Step 4: MainWindow impl**

In `src/app/ui/mainwindow.cpp`:
- Update the constructor signature + init list to accept and store `warmup` (add `warmup_(warmup)` to the init list; the parameter is `WarmupState* warmup`).
- Change the `CameraView` construction line (currently `camera_view_ = new CameraView(db_, engines);`) to:

```cpp
    camera_view_ = new CameraView(db_, engines, warmup_);
```

- Add `#include "ui/warmup_state.h"` if the type is needed as complete (forward decl + pointer pass-through is sufficient; include not required unless a method is called).

- [ ] **Step 5: Build**

Run: `cmake --build build 2>&1 | tail -20`
Expected: only remaining break is `startup.cpp` still calling the old `MainWindow(db, state, engines)` — fixed in Task 8. Confirm that is the sole error.

- [ ] **Step 6: Commit**

```bash
git add src/app/ui/camera/camera_view.h src/app/ui/camera/camera_view.cpp src/app/ui/mainwindow.h src/app/ui/mainwindow.cpp
git commit -m "feat(startup): thread WarmupState through CameraView + MainWindow"
```

---

## Task 8: `startup.cpp` — show first, warm in background; retire splash

**Files:**
- Modify: `src/app/ui/startup.cpp`
- Delete: `src/app/ui/startup_screen.h`, `src/app/ui/startup_screen.cpp`
- Modify: `src/app/CMakeLists.txt`

**Interfaces:**
- Consumes: `WarmupState` (Task 4), `MainWindow(db, state, engines, warmup)` (Task 7).
- Produces: unchanged `int launch(QApplication&, QSqlDatabase, std::shared_ptr<settings::Settings>)`.

- [ ] **Step 1: Rewrite `launch()`**

Replace the body of `src/app/ui/startup.cpp` with:

```cpp
#include "ui/startup.h"

#include "ui/camera/shared/detection/engine_registry.h"
#include "ui/mainwindow.h"
#include "ui/warmup_state.h"

#include <QApplication>
#include <QCoreApplication>

#include <memory>
#include <string>

namespace denso::ui {

int launch(QApplication& app, QSqlDatabase db,
           std::shared_ptr<settings::Settings> state) {
    const std::string dir = QCoreApplication::applicationDirPath().toStdString();
    auto engines = std::make_shared<EngineRegistry>(dir + "/models",
                                                    dir + "/models/trt_cache");

    // Warm-up runs in the background; the window shows immediately. WarmupState
    // owns the worker thread and outlives app.exec() (it lives in this scope).
    WarmupState warmup(engines);

    // Build the window first (it subscribes CameraGrid to warmup signals in its
    // ctor), THEN start warming — so any model_ready/finished the worker queues
    // is delivered after the grid has connected (and is_ready() covers anything
    // that raced ahead).
    MainWindow window(db, state, engines, &warmup);
    window.apply_startup();
    window.show();
    window.raise();
    window.activateWindow();  // claim the foreground at launch

    warmup.start();
    return app.exec();
}

} // namespace denso::ui
```

Note: `MainWindow` and `WarmupState` are now stack objects in `launch`'s scope; both outlive `app.exec()`. `WarmupState`'s destructor `quit()`s + `wait()`s the worker thread on teardown.

- [ ] **Step 2: Remove `StartupScreen` from the build and delete it**

- In `src/app/CMakeLists.txt`, delete the `ui/startup_screen.cpp` source line.
- Delete the files:

```bash
git rm src/app/ui/startup_screen.h src/app/ui/startup_screen.cpp
```

- [ ] **Step 3: Build + run the suite**

Run: `cmake --build build 2>&1 | tail -20; ctest --test-dir build --output-on-failure 2>&1 | tail -10`
Expected: clean build (no more `startup_screen` references, `MainWindow` 4-arg ctor satisfied); suite green.

- [ ] **Step 4: Commit**

```bash
git add src/app/ui/startup.cpp src/app/CMakeLists.txt
git commit -m "feat(startup): show the window immediately, warm models in the background; retire the splash"
```

---

## Task 9: Documentation

**Files:**
- Modify: `CLAUDE.md`
- Modify: `docs/ARCHITECTURE.md`

- [ ] **Step 1: Update `CLAUDE.md`**

- In the `src/app/` app-shell area (near `main.cpp` / `ui/startup`), replace the `StartupScreen`/blocking warm-up description with: the window shows immediately; `ui/warmup_state` (`WarmupState`) owns the background warm-up thread and relays per-model readiness; `CameraGrid` starts each detection camera when its models finish warming (orientation-only cameras start at once), gated by the pure `ui/camera/grid/warmup_gate` (`PendingStart`). Note the hard rule is preserved: the TensorRT build stays on the warm-up worker; a detection stream (capture thread) is created only after its models are ready, so `EngineRegistry::get()` is a cache-hit there.
- In the detection section, update the `EngineRegistry` note: `get()` is now mutex-guarded (warm-up worker + GUI thread), and `warm_up` fires an `on_ready(filename)` per warmed model that drives the UI-first start.
- Remove/replace the sentence stating warm-up "completes before the window shows and before any capture thread exists" — it now completes before each *detection* capture thread for that model exists, while the window is already up.

- [ ] **Step 2: Update `docs/ARCHITECTURE.md`**

- In the boot-sequence / startup section, replace the "splash blocks until warm-up finishes, then MainWindow shows" description with the UI-first flow: create `EngineRegistry` + `WarmupState` → build + show `MainWindow` immediately → `WarmupState::start()` warms on the worker → `CameraGrid` starts model-less/ready cameras now and pending ones on `model_ready`, flushing the remainder on `finished` (orientation fallback). Note the `EngineRegistry::get()` mutex and the preserved no-build-on-capture-thread invariant.

- [ ] **Step 3: Commit**

```bash
git add CLAUDE.md docs/ARCHITECTURE.md
git commit -m "docs(startup): document UI-first startup + background warm gating"
```

---

## Manual Verification (on the MSYS2/GPU build machine — cannot run in the dev harness)

1. Build + suite: `cmake --build build && ctest --test-dir build` → green (new `warmup_gate` cases included).
2. **Cold first launch** (delete `models/trt_cache/` to force a build): the window appears immediately; an orientation-only camera (no attached model) streams at once; a detection camera's tile shows "Preparing model…" (the log shows the minutes-long TensorRT build on the warm-up worker), then goes live when its model finishes.
3. **Warm second launch** (cache present): the window appears immediately and detection tiles go live within the cache-load time — no full-screen splash, no multi-second freeze.
4. Confirm no UI freeze during warm-up, and a clean shutdown if the app is closed mid-warm-up (WarmupState joins the worker).
5. Confirm a camera whose model file is corrupt/unloadable ends up orientation-only after warm-up finishes (falls back, no stuck "Preparing").

## Self-Review Notes (traceability)

- Spec §"show window first, warm in background" → Tasks 6 (deferred start) + 8 (startup.cpp).
- Spec §"WarmupState (owns thread, relays readiness, is_ready/is_complete)" → Task 4.
- Spec §"WarmupWorker model_ready signal" → Task 3.
- Spec §"EngineRegistry mutex + stale-note update" → Task 2.
- Spec §"per-tile Preparing state" → Task 5.
- Spec §"pending→start gating, orientation fallback on failed model" → Tasks 1 (PendingStart) + 6 (`on_warmup_finished`/`start_one` fallback).
- Spec §"no lost signals (record in set + emit; connect before start)" → Task 4 (`ready_` set + emit) + Task 8 (build window/connect before `warmup.start()`) + Task 6 (`is_ready` query at reload).
- Spec §"no build on a capture thread preserved" → Task 6 (`start_one` only after ready; cache-hit `get()`).
- Spec §"retire StartupScreen" → Task 8.
- Spec §"testing: WarmupState/gating pure; widgets build+smoke" → Task 1 (pure tests) + Manual Verification.
- Spec §"docs" → Task 9.
