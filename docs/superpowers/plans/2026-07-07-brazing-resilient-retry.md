# Brazing Resilient Retry Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** When a brazing zone POST fails (server down / non-2xx), keep retrying the latest coalesced zone snapshot with exponential backoff until the server accepts it, instead of dropping it.

**Architecture:** A pure, unit-tested state machine (`BrazingRetryPolicy`) holds the pending/delivered/in-flight snapshots + backoff and decides the next action (Send / ArmRetry / None). A thin GUI-thread `QObject` shell (`BrazingReporter`) executes those actions against a `BrazingTransport` interface (implemented by a slimmed `BrazingClient`) and a single-shot `QTimer`. `ZoneAggregator`/`ZoneReporter` are unchanged — they already emit the full coalesced snapshot on change.

**Tech Stack:** C++17, Qt6 (Core/Network), Catch2 v3, CMake + Ninja, MSYS2 UCRT64.

## Global Constraints

- Toolchain: MSYS2 UCRT64. Build: `export PATH=/c/msys64/ucrt64/bin:$PATH; cmake -S . -B build -G Ninja; cmake --build build; ctest --test-dir build`.
- `denso_core` must not link `Qt6::Widgets`; this feature is app-layer only (`src/app/…`) — the core is untouched.
- Delivery model is **latest-value-wins, in-memory only** — no DB, no migration, no persistence across restart.
- Single-flight: at most one POST outstanding at a time; the newest snapshot always wins.
- Retryable = any network error / timeout / non-2xx. Backoff: 1s → ×2 → 30s cap; reset on success or on a new snapshot.
- Follow the repo pattern: pure logic in a std-only unit that is unit-tested; the Qt/threading shell mirrors `ZoneReporter` (no direct unit test, covered by integration smoke).
- The `test-server` endpoint (`d:\workspace\test-server`, `POST /api/brazing/update`) already accepts these payloads — do not change it.

---

## File Structure

- Create `src/app/ui/camera/grid/brazing_retry_policy.h` — pure state-machine API (`RetryAction`, `BrazingRetryPolicy`).
- Create `src/app/ui/camera/grid/brazing_retry_policy.cpp` — the transitions + backoff.
- Create `tests/test_brazing_retry_policy.cpp` — Catch2 unit tests (pure).
- Create `src/app/ui/camera/grid/brazing_transport.h` — `BrazingTransport` interface (pure).
- Modify `src/app/ui/camera/grid/brazing_client.h` / `.cpp` — implement `BrazingTransport::post(zones, done)`; drop the old `send()`.
- Create `src/app/ui/camera/grid/brazing_reporter.h` / `.cpp` — the thin QObject coordinator shell.
- Modify `src/app/ui/camera/grid/camera_grid.h` / `.cpp` — hold a `BrazingReporter` instead of a raw `BrazingClient`; the `ZoneReporter` callback marshals to `reporter->submit(...)`.
- Modify `tests/CMakeLists.txt` — add `test_brazing_retry_policy.cpp` + `brazing_retry_policy.cpp`.
- Modify `src/app/CMakeLists.txt` — add the three new app sources (`brazing_retry_policy.cpp`, `brazing_reporter.cpp`; `brazing_transport.h` is header-only).
- Modify `docs/ARCHITECTURE.md`, `CLAUDE.md`, `README.md` — document the retry behavior.

---

## Task 1: `BrazingRetryPolicy` (pure state machine)

**Files:**
- Create: `src/app/ui/camera/grid/brazing_retry_policy.h`
- Create: `src/app/ui/camera/grid/brazing_retry_policy.cpp`
- Test: `tests/test_brazing_retry_policy.cpp`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: nothing (std only).
- Produces:
  - `struct denso::ui::RetryAction { enum class Kind { None, Send, ArmRetry }; Kind kind = Kind::None; std::map<int,int> snapshot; int delay_ms = 0; };`
  - `class denso::ui::BrazingRetryPolicy` with:
    - `explicit BrazingRetryPolicy(int start_ms = 1000, int cap_ms = 30000);`
    - `RetryAction submit(const std::map<int,int>& snapshot);`
    - `RetryAction on_result(bool ok);`
    - `RetryAction on_retry_tick();`

- [ ] **Step 1: Write the header**

Create `src/app/ui/camera/grid/brazing_retry_policy.h`:

```cpp
// Pure retry state machine for the brazing reporter: holds the latest snapshot
// we want the server to have (pending), the last one it 2xx-acked (delivered),
// and the one currently in flight, plus the backoff counter. Each event method
// returns the single next action for the (thin, Qt) shell to perform. No Qt —
// unit-tested like ZoneAggregator. Single-flight + latest-value-wins live here.
#pragma once

#include <map>

namespace denso::ui {

/// One instruction for the BrazingReporter shell.
struct RetryAction {
    enum class Kind { None, Send, ArmRetry };
    Kind kind = Kind::None;
    std::map<int, int> snapshot;  // the payload to POST, when kind == Send
    int delay_ms = 0;             // retry delay, when kind == ArmRetry
};

class BrazingRetryPolicy {
public:
    /// start_ms = first retry delay; cap_ms = maximum retry delay.
    explicit BrazingRetryPolicy(int start_ms = 1000, int cap_ms = 30000);

    /// A new full snapshot to deliver. Resets backoff; sends now if idle.
    RetryAction submit(const std::map<int, int>& snapshot);

    /// Result of the in-flight POST. ok == true iff HTTP 2xx received.
    RetryAction on_result(bool ok);

    /// The retry timer fired. Re-sends the current pending snapshot if idle.
    RetryAction on_retry_tick();

private:
    RetryAction maybe_send();  // Send iff !in_flight_ && pending_ != delivered_

    int start_ms_;
    int cap_ms_;
    int backoff_ms_ = 0;       // 0 = not backing off; last delay otherwise
    bool in_flight_ = false;
    std::map<int, int> pending_;
    std::map<int, int> delivered_;
    std::map<int, int> in_flight_snap_;
};

} // namespace denso::ui
```

- [ ] **Step 2: Write the failing tests**

Create `tests/test_brazing_retry_policy.cpp`:

```cpp
#include "ui/camera/grid/brazing_retry_policy.h"

#include <catch2/catch_test_macros.hpp>

using denso::ui::BrazingRetryPolicy;
using denso::ui::RetryAction;
using Kind = denso::ui::RetryAction::Kind;
using Snap = std::map<int, int>;

TEST_CASE("first submit sends immediately") {
    BrazingRetryPolicy p;
    RetryAction a = p.submit(Snap{{1, 500}, {2, 200}});
    REQUIRE(a.kind == Kind::Send);
    REQUIRE(a.snapshot == Snap{{1, 500}, {2, 200}});
}

TEST_CASE("failure then success delivers the same snapshot on retry") {
    BrazingRetryPolicy p;
    p.submit(Snap{{1, 500}});
    RetryAction f = p.on_result(false);
    REQUIRE(f.kind == Kind::ArmRetry);
    REQUIRE(f.delay_ms == 1000);
    RetryAction t = p.on_retry_tick();
    REQUIRE(t.kind == Kind::Send);
    REQUIRE(t.snapshot == Snap{{1, 500}});
    RetryAction ok = p.on_result(true);
    REQUIRE(ok.kind == Kind::None);  // nothing left to send
}

TEST_CASE("note.txt scenario: failed sends merge forward into one snapshot") {
    BrazingRetryPolicy p;
    // 1. {z1:500,z2:200} -> send -> fail
    REQUIRE(p.submit(Snap{{1, 500}, {2, 200}}).kind == Kind::Send);
    REQUIRE(p.on_result(false).kind == Kind::ArmRetry);
    // 2. new value merges z3 -> full snapshot -> send -> fail
    RetryAction s2 = p.submit(Snap{{1, 500}, {2, 200}, {3, 540}});
    REQUIRE(s2.kind == Kind::Send);
    REQUIRE(s2.snapshot == Snap{{1, 500}, {2, 200}, {3, 540}});
    REQUIRE(p.on_result(false).kind == Kind::ArmRetry);
    // 3. new value z1:600 merges -> send -> success
    RetryAction s3 = p.submit(Snap{{1, 600}, {2, 200}, {3, 540}});
    REQUIRE(s3.kind == Kind::Send);
    REQUIRE(s3.snapshot == Snap{{1, 600}, {2, 200}, {3, 540}});
    REQUIRE(p.on_result(true).kind == Kind::None);
}

TEST_CASE("backoff doubles to the cap and resets on success") {
    BrazingRetryPolicy p(1000, 30000);
    p.submit(Snap{{1, 1}});
    REQUIRE(p.on_result(false).delay_ms == 1000);
    REQUIRE(p.on_retry_tick().kind == Kind::Send);
    REQUIRE(p.on_result(false).delay_ms == 2000);
    REQUIRE(p.on_retry_tick().kind == Kind::Send);
    REQUIRE(p.on_result(false).delay_ms == 4000);
    // …jump ahead: keep failing until the cap
    for (int i = 0; i < 10; ++i) {
        p.on_retry_tick();
        p.on_result(false);
    }
    p.on_retry_tick();
    REQUIRE(p.on_result(false).delay_ms == 30000);  // capped
    // recover
    p.on_retry_tick();
    REQUIRE(p.on_result(true).kind == Kind::None);
    // a fresh failure starts from 1s again
    p.submit(Snap{{1, 2}});
    REQUIRE(p.on_result(false).delay_ms == 1000);
}

TEST_CASE("submit reset backoff to fast start") {
    BrazingRetryPolicy p;
    p.submit(Snap{{1, 1}});
    REQUIRE(p.on_result(false).delay_ms == 1000);
    p.on_retry_tick();
    REQUIRE(p.on_result(false).delay_ms == 2000);
    // a new value arrives -> backoff resets
    RetryAction s = p.submit(Snap{{1, 9}});
    REQUIRE(s.kind == Kind::None);  // a POST is already in flight
    // the in-flight one completes (fail) -> next send is the new value, delay back to 1s
    REQUIRE(p.on_result(false).delay_ms == 1000);
    RetryAction t = p.on_retry_tick();
    REQUIRE(t.kind == Kind::Send);
    REQUIRE(t.snapshot == Snap{{1, 9}});
}

TEST_CASE("single-flight: submit mid-flight defers; newest wins on completion") {
    BrazingRetryPolicy p;
    REQUIRE(p.submit(Snap{{1, 1}}).kind == Kind::Send);   // in flight = {1:1}
    RetryAction mid = p.submit(Snap{{1, 2}});             // arrives mid-flight
    REQUIRE(mid.kind == Kind::None);                       // no second concurrent send
    RetryAction after = p.on_result(true);                // stale {1:1} succeeds
    REQUIRE(after.kind == Kind::Send);                     // …but pending moved on
    REQUIRE(after.snapshot == Snap{{1, 2}});
    REQUIRE(p.on_result(true).kind == Kind::None);         // {1:2} delivered
}

TEST_CASE("retry tick with nothing pending is a no-op") {
    BrazingRetryPolicy p;
    REQUIRE(p.on_retry_tick().kind == Kind::None);
    p.submit(Snap{{1, 1}});
    p.on_result(true);                    // delivered, idle
    REQUIRE(p.on_retry_tick().kind == Kind::None);
}
```

- [ ] **Step 3: Register the test + source in CMake**

In `tests/CMakeLists.txt`, after the `test_brazing_payload.cpp` block (around line 84, before the closing `)` of `add_executable`), add:

```cmake
    test_brazing_retry_policy.cpp
    # brazing_retry_policy is GUI-target code but pure std (no Qt/OpenCV), so
    # compile it in.
    ${CMAKE_SOURCE_DIR}/src/app/ui/camera/grid/brazing_retry_policy.cpp
```

- [ ] **Step 4: Run tests to verify they fail (no implementation yet)**

```bash
export PATH=/c/msys64/ucrt64/bin:$PATH
cmake -S . -B build -G Ninja
cmake --build build 2>&1 | tail -20
```
Expected: **compile/link failure** — `brazing_retry_policy.cpp` does not exist yet.

- [ ] **Step 5: Write the implementation**

Create `src/app/ui/camera/grid/brazing_retry_policy.cpp`:

```cpp
#include "ui/camera/grid/brazing_retry_policy.h"

#include <algorithm>

namespace denso::ui {

BrazingRetryPolicy::BrazingRetryPolicy(int start_ms, int cap_ms)
    : start_ms_(start_ms), cap_ms_(cap_ms) {}

RetryAction BrazingRetryPolicy::maybe_send() {
    if (in_flight_ || pending_ == delivered_) {
        return {};  // Kind::None
    }
    in_flight_ = true;
    in_flight_snap_ = pending_;
    RetryAction a;
    a.kind = RetryAction::Kind::Send;
    a.snapshot = pending_;
    return a;
}

RetryAction BrazingRetryPolicy::submit(const std::map<int, int>& snapshot) {
    pending_ = snapshot;
    backoff_ms_ = 0;  // a fresh value resets the retry cadence
    return maybe_send();
}

RetryAction BrazingRetryPolicy::on_result(bool ok) {
    in_flight_ = false;
    if (ok) {
        delivered_ = in_flight_snap_;
        backoff_ms_ = 0;
        return maybe_send();  // send again if pending moved on, else None
    }
    backoff_ms_ = (backoff_ms_ == 0)
                      ? start_ms_
                      : std::min(backoff_ms_ * 2, cap_ms_);
    RetryAction a;
    a.kind = RetryAction::Kind::ArmRetry;
    a.delay_ms = backoff_ms_;
    return a;
}

RetryAction BrazingRetryPolicy::on_retry_tick() {
    return maybe_send();
}

} // namespace denso::ui
```

- [ ] **Step 6: Run tests to verify they pass**

```bash
export PATH=/c/msys64/ucrt64/bin:$PATH
cmake --build build 2>&1 | tail -5
ctest --test-dir build -R BrazingRetryPolicy --output-on-failure 2>&1 | tail -20
# (Catch2 test names are the TEST_CASE strings; filter with a broad regex if -R misses)
ctest --test-dir build --output-on-failure 2>&1 | tail -15
```
Expected: all `brazing_retry_policy` cases PASS; full suite still green.

- [ ] **Step 7: Commit**

```bash
git add src/app/ui/camera/grid/brazing_retry_policy.h src/app/ui/camera/grid/brazing_retry_policy.cpp tests/test_brazing_retry_policy.cpp tests/CMakeLists.txt
git commit -m "feat(brazing): pure retry state machine (coalescing, single-flight, backoff)"
```

---

## Task 2: `BrazingTransport` interface + `BrazingClient` refactor

**Files:**
- Create: `src/app/ui/camera/grid/brazing_transport.h`
- Modify: `src/app/ui/camera/grid/brazing_client.h`
- Modify: `src/app/ui/camera/grid/brazing_client.cpp`

**Interfaces:**
- Consumes: nothing new.
- Produces:
  - `struct denso::ui::BrazingTransport { virtual ~BrazingTransport() = default; virtual void post(const std::map<int,int>& zones, std::function<void(bool)> done) = 0; };`
  - `class denso::ui::BrazingClient : public QObject, public BrazingTransport` with `explicit BrazingClient(std::string base_url, QObject* parent = nullptr);` and `void post(const std::map<int,int>& zones, std::function<void(bool)> done) override;` (the old `send()` is removed).

- [ ] **Step 1: Write the transport interface**

Create `src/app/ui/camera/grid/brazing_transport.h`:

```cpp
// The one-shot POST seam the BrazingReporter drives. Abstract so the reporter's
// retry logic can be exercised with a fake in tests, and so the real Qt-network
// client is the only thing that touches QNetworkAccessManager.
#pragma once

#include <functional>
#include <map>

namespace denso::ui {

struct BrazingTransport {
    virtual ~BrazingTransport() = default;

    /// Fire one POST of `zones`. Invoke `done(ok)` on completion, on the GUI
    /// thread. `ok` is true iff an HTTP 2xx was received; false on any network
    /// error, timeout, or non-2xx response.
    virtual void post(const std::map<int, int>& zones,
                      std::function<void(bool)> done) = 0;
};

} // namespace denso::ui
```

- [ ] **Step 2: Update the client header**

Replace `src/app/ui/camera/grid/brazing_client.h` with:

```cpp
// Transport for the brazing backend: one async POST of the combined zone
// snapshot to {base_url}/api/brazing/update via QNetworkAccessManager, with a
// bounded timeout. Reports the outcome via the `done` callback — it holds NO
// retry/pending state (the BrazingReporter owns that). Lives on the GUI thread.
#pragma once

#include "ui/camera/grid/brazing_transport.h"

#include <QObject>
#include <QString>

#include <functional>
#include <map>
#include <string>

class QNetworkAccessManager;

namespace denso::ui {

class BrazingClient : public QObject, public BrazingTransport {
    Q_OBJECT

public:
    explicit BrazingClient(std::string base_url, QObject* parent = nullptr);

    /// POST {"zone<n>": value, ...}. Calls done(false) immediately if base_url
    /// is empty. done(ok): ok == HTTP 2xx.
    void post(const std::map<int, int>& zones,
              std::function<void(bool)> done) override;

private:
    QString base_url_;
    QNetworkAccessManager* nam_ = nullptr;
};

} // namespace denso::ui
```

- [ ] **Step 3: Update the client implementation**

Replace the body of `src/app/ui/camera/grid/brazing_client.cpp` (keep the trailing-slash trim in the constructor) with:

```cpp
#include "ui/camera/grid/brazing_client.h"

#include "ui/camera/grid/brazing_payload.h"

#include <QByteArray>
#include <QDebug>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>
#include <QVariant>

namespace denso::ui {

namespace {
constexpr int kBrazingTimeoutMs = 5000;  // abort a stuck POST (soak-safe)
}

BrazingClient::BrazingClient(std::string base_url, QObject* parent)
    : QObject(parent),
      base_url_(QString::fromStdString(base_url)),
      nam_(new QNetworkAccessManager(this)) {
    while (base_url_.endsWith('/')) {
        base_url_.chop(1);
    }
}

void BrazingClient::post(const std::map<int, int>& zones,
                         std::function<void(bool)> done) {
    if (base_url_.isEmpty()) {
        if (done) done(false);
        return;
    }
    const QUrl url(base_url_ + QStringLiteral("/api/brazing/update"));
    if (!url.isValid()) {
        qWarning().noquote() << "[brazing] invalid base URL:" << base_url_;
        if (done) done(false);
        return;
    }
    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::ContentTypeHeader,
                  QByteArrayLiteral("application/json"));
    req.setTransferTimeout(kBrazingTimeoutMs);

    const QByteArray body =
        QByteArray::fromStdString(build_brazing_payload(zones));
    QNetworkReply* reply = nam_->post(req, body);
    QObject::connect(reply, &QNetworkReply::finished, reply, [reply, done] {
        const int status =
            reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const bool ok =
            reply->error() == QNetworkReply::NoError && status >= 200 && status < 300;
        if (!ok) {
            qWarning().noquote()
                << "[brazing] POST failed (will retry):" << reply->errorString();
        }
        reply->deleteLater();
        if (done) done(ok);
    });
}

} // namespace denso::ui
```

- [ ] **Step 4: Build to verify it compiles (call sites in camera_grid still use send(); expect that one break)**

```bash
export PATH=/c/msys64/ucrt64/bin:$PATH
cmake --build build 2>&1 | tail -20
```
Expected: `brazing_client.cpp` compiles; the **only** error is in `camera_grid.cpp` still calling the removed `send()` / constructing `BrazingClient` for `brazing_client_`. That is fixed in Task 4 — do not fix it here beyond confirming it's the sole failure.

> If subagent-driven and a clean per-task build is required, reorder: do Task 3 and Task 4 before building. The three Qt-glue tasks (2–4) form one buildable unit; commit them together at the end of Task 4 if a green build per commit is mandatory. Otherwise commit Task 2 now with the known transient camera_grid break.

- [ ] **Step 5: Commit**

```bash
git add src/app/ui/camera/grid/brazing_transport.h src/app/ui/camera/grid/brazing_client.h src/app/ui/camera/grid/brazing_client.cpp
git commit -m "refactor(brazing): BrazingTransport seam; client reports ok via callback"
```

---

## Task 3: `BrazingReporter` (thin coordinator shell)

**Files:**
- Create: `src/app/ui/camera/grid/brazing_reporter.h`
- Create: `src/app/ui/camera/grid/brazing_reporter.cpp`
- Modify: `src/app/CMakeLists.txt`

**Interfaces:**
- Consumes: `BrazingRetryPolicy` (Task 1), `BrazingTransport` (Task 2).
- Produces:
  - `class denso::ui::BrazingReporter : public QObject` with `explicit BrazingReporter(std::unique_ptr<BrazingTransport> transport, QObject* parent = nullptr);` and `void submit(const std::map<int,int>& snapshot);`

- [ ] **Step 1: Write the header**

Create `src/app/ui/camera/grid/brazing_reporter.h`:

```cpp
// GUI-thread coordinator: turns a stream of full zone snapshots into reliable
// delivery. Owns a BrazingRetryPolicy (decides what to do), a BrazingTransport
// (does the POST), and a single-shot retry QTimer. submit() is called via
// common::post_to_gui from the ZoneReporter, so everything here runs on the GUI
// thread — no locking. Mirrors ZoneReporter as a thin shell (no unit test;
// covered by the integration smoke). Retry state is in-memory only.
#pragma once

#include "ui/camera/grid/brazing_retry_policy.h"
#include "ui/camera/grid/brazing_transport.h"

#include <QObject>

#include <map>
#include <memory>

class QTimer;

namespace denso::ui {

class BrazingReporter : public QObject {
    Q_OBJECT

public:
    explicit BrazingReporter(std::unique_ptr<BrazingTransport> transport,
                             QObject* parent = nullptr);
    ~BrazingReporter() override;

    /// Hand in the latest full zone snapshot to (eventually) deliver.
    void submit(const std::map<int, int>& snapshot);

private:
    void apply(const RetryAction& action);  // execute one policy instruction

    std::unique_ptr<BrazingTransport> transport_;
    BrazingRetryPolicy policy_;
    QTimer* retry_timer_ = nullptr;  // single-shot; owned via QObject parent
};

} // namespace denso::ui
```

- [ ] **Step 2: Write the implementation**

Create `src/app/ui/camera/grid/brazing_reporter.cpp`:

```cpp
#include "ui/camera/grid/brazing_reporter.h"

#include <QTimer>

#include <utility>

namespace denso::ui {

BrazingReporter::BrazingReporter(std::unique_ptr<BrazingTransport> transport,
                                 QObject* parent)
    : QObject(parent), transport_(std::move(transport)) {
    retry_timer_ = new QTimer(this);
    retry_timer_->setSingleShot(true);
    QObject::connect(retry_timer_, &QTimer::timeout, this,
                     [this] { apply(policy_.on_retry_tick()); });
}

BrazingReporter::~BrazingReporter() = default;

void BrazingReporter::submit(const std::map<int, int>& snapshot) {
    apply(policy_.submit(snapshot));
}

void BrazingReporter::apply(const RetryAction& action) {
    switch (action.kind) {
        case RetryAction::Kind::None:
            return;
        case RetryAction::Kind::Send:
            transport_->post(action.snapshot, [this](bool ok) {
                // Back on the GUI thread (BrazingClient invokes done from the
                // reply handler, which runs on the GUI thread).
                apply(policy_.on_result(ok));
            });
            return;
        case RetryAction::Kind::ArmRetry:
            retry_timer_->start(action.delay_ms);
            return;
    }
}

} // namespace denso::ui
```

- [ ] **Step 3: Register the new app sources in CMake**

In `src/app/CMakeLists.txt`, find where `brazing_client.cpp` is listed in the `denso` target sources and add the two new sources next to it:

```cmake
    ui/camera/grid/brazing_retry_policy.cpp
    ui/camera/grid/brazing_reporter.cpp
```
(`brazing_transport.h` is header-only — no source entry needed. Verify the exact list style by reading the file first; match the surrounding entries.)

- [ ] **Step 4: Commit (build happens with Task 4, which fixes the call site)**

```bash
git add src/app/ui/camera/grid/brazing_reporter.h src/app/ui/camera/grid/brazing_reporter.cpp src/app/CMakeLists.txt
git commit -m "feat(brazing): BrazingReporter shell (policy + transport + retry timer)"
```

---

## Task 4: Wire `BrazingReporter` into `CameraGrid`

**Files:**
- Modify: `src/app/ui/camera/grid/camera_grid.h`
- Modify: `src/app/ui/camera/grid/camera_grid.cpp`

**Interfaces:**
- Consumes: `BrazingReporter` (Task 3), `BrazingClient` (Task 2), `ZoneReporter` (unchanged), `common::post_to_gui` (unchanged).
- Produces: nothing downstream.

- [ ] **Step 1: Swap the member in the header**

In `src/app/ui/camera/grid/camera_grid.h`:
- Change the forward declaration `class BrazingClient;` to `class BrazingReporter;`.
- Change the member `std::unique_ptr<BrazingClient> brazing_client_;` to `std::unique_ptr<BrazingReporter> brazing_reporter_;  // GUI-thread reliable sender`.

- [ ] **Step 2: Update the includes + wiring in reload()**

In `src/app/ui/camera/grid/camera_grid.cpp`:
- Replace `#include "ui/camera/grid/brazing_client.h"` with:
  ```cpp
  #include "ui/camera/grid/brazing_client.h"
  #include "ui/camera/grid/brazing_reporter.h"
  ```
- Replace the reporter-wiring block in `reload()`:

  ```cpp
    const brazing::BrazingConfig bcfg = brazing::load(db_);
    if (bcfg.enabled && !bcfg.base_url.empty()) {
        brazing_reporter_ = std::make_unique<BrazingReporter>(
            std::make_unique<BrazingClient>(bcfg.base_url));
        BrazingReporter* reporter = brazing_reporter_.get();
        reporter_ = std::make_unique<ZoneReporter>(
            [reporter](const std::map<int, int>& snap) {
                common::post_to_gui(reporter,
                                    [reporter, snap] { reporter->submit(snap); });
            });
    }
  ```

- [ ] **Step 3: Update teardown in clear()**

In `clear()`, replace `brazing_client_.reset();` with `brazing_reporter_.reset();`. (The ordering is unchanged: streams are stopped/joined first, then `reporter_`, then the brazing sender — so no capture thread can still reach it, and the reporter's timer is destroyed with it.)

- [ ] **Step 4: Build the whole app + run the suite**

```bash
export PATH=/c/msys64/ucrt64/bin:$PATH
cmake --build build 2>&1 | tail -20
ctest --test-dir build --output-on-failure 2>&1 | tail -15
```
Expected: clean build; full suite green (the new policy tests included).

- [ ] **Step 5: Commit**

```bash
git add src/app/ui/camera/grid/camera_grid.h src/app/ui/camera/grid/camera_grid.cpp
git commit -m "feat(brazing): route zone snapshots through BrazingReporter (retry on failure)"
```

---

## Task 5: Documentation

**Files:**
- Modify: `docs/ARCHITECTURE.md`
- Modify: `CLAUDE.md`
- Modify: `README.md`

- [ ] **Step 1: Update `CLAUDE.md`**

In the `camera/grid` table, update the `brazing_client` row and add a reporter row. Replace the existing `brazing_client.{h,cpp}` description with two entries:

```
| `ui/camera/grid/brazing_retry_policy.{h,cpp}` | Pure (unit-tested) retry state machine: holds pending/delivered/in-flight snapshots + backoff (1s→×2→30s cap); `submit`/`on_result`/`on_retry_tick` each return the next `RetryAction` (Send/ArmRetry/None). Single-flight + latest-value-wins. |
| `ui/camera/grid/brazing_reporter.{h,cpp}` | GUI-thread shell over `BrazingRetryPolicy` + a single-shot retry `QTimer` + a `BrazingTransport`. `submit()` (marshaled from `ZoneReporter`) drives reliable, coalescing delivery — a downed server is retried until it 2xx-acks the latest snapshot. In-memory only. |
| `ui/camera/grid/brazing_client.{h,cpp}` | `BrazingTransport` impl: one async best-effort POST of the snapshot to `{base_url}/api/brazing/update` (`QNetworkAccessManager`, 5s timeout); reports 2xx-or-not via a `done(ok)` callback. No retry state (that's the reporter's). |
```

- [ ] **Step 2: Update `docs/ARCHITECTURE.md`**

Find the brazing / zone-reporter description (added by the brazing-zone-reporter feature) and change the delivery wording from best-effort log-and-drop to: the `BrazingReporter` coalescing single-flight retry (latest-value-wins, exponential backoff 1s→30s, in-memory) driving the `BrazingClient` transport. Note the capture thread → `ZoneReporter` → `post_to_gui` → `BrazingReporter` path is unchanged upstream.

- [ ] **Step 3: Update `README.md`**

In the brazing section, add a sentence: "If the server is unreachable, the reporter keeps retrying the latest zone values (exponential backoff, up to 30 s between tries) and delivers them once the server returns — the newest reading always wins; nothing is queued or persisted."

- [ ] **Step 4: Commit**

```bash
git add docs/ARCHITECTURE.md CLAUDE.md README.md
git commit -m "docs(brazing): document resilient coalescing retry"
```

---

## Manual Verification (after all tasks)

Not a code step — run once on a machine that can build/run the app (GPU target for full detection, but the retry path is testable with any camera or even none if a zone value can be produced):

1. Start the test server: `cd d:/workspace/test-server && python server.py --port 8098`.
2. In the app Settings → Server panel: enable, base URL `http://127.0.0.1:8098`, save; reload the camera grid.
3. Change a zone's number → confirm a `{"zoneN":…}` POST lands in the test-server log.
4. **Stop** the test server. Change zone values a few times → confirm the app stays responsive and logs `[brazing] POST failed (will retry)` with a growing backoff, and does **not** freeze.
5. **Restart** the test server → confirm exactly the **latest** merged snapshot arrives (current values), without a backlog of intermediate values.

## Self-Review Notes (traceability)

- Spec §"coalescing single-flight retry" → Task 1 (`maybe_send` gate + tests incl. mid-flight + note.txt scenario).
- Spec §"exponential backoff 1s→30s, reset on success/new value" → Task 1 (`on_result`/`submit` backoff + tests).
- Spec §"BrazingTransport seam / slimmed client / done(ok)=2xx" → Task 2.
- Spec §"BrazingReporter thin shell + QTimer + in-memory" → Task 3.
- Spec §"wiring swaps client for reporter, teardown order" → Task 4.
- Spec §"docs deliverables (ARCHITECTURE/CLAUDE/README)" → Task 5.
- Spec §"integration smoke (test-server, stop/restart)" → Manual Verification.
