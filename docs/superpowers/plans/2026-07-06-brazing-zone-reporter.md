# Brazing Zone HTTP Reporter Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Read the multi-digit number in each camera ROI ("zone"), and POST the current value of every zone on the machine as one combined JSON body whenever any zone's reading changes.

**Architecture:** Pure units do the work and get real TDD — `zone_assembly` (digits→number), `zone_aggregator` (debounce + change detection), `brazing_payload` (JSON string), and a core `brazing::config` (persist enable+URL). Thin Qt units wire them up: a `ZoneReporter` (`ZoneSink` implementation, thread-safe) marshals a change snapshot to a GUI-thread `BrazingClient` that POSTs via `QNetworkAccessManager`, best-effort. `DetectionProcessor` (which already has the ROI areas + kept digit boxes) grows one optional `ZoneSink*` and feeds the reporter. A v10 migration adds a per-area `zone` number.

**Tech Stack:** C++17, Qt6 Widgets + Qt6::Network, OpenCV, ONNX Runtime, SQLite, Catch2 v3, CMake+Ninja.

## Global Constraints

- **Toolchain:** MSYS2 UCRT64. Every build/test runs after `export PATH=/c/msys64/ucrt64/bin:$PATH`.
- **Configure once:** `cmake -S . -B build -G Ninja` (only if `build/` absent). Rebuild: `cmake --build build`. Test: `ctest --test-dir build --output-on-failure`. Single test by tag: `./build/tests/denso_tests "[tag]"`.
- **Test harness:** Catch2 v3 in `tests/`. A GUI-target `.cpp` with no Qt-widget dependency is compiled straight into `denso_tests` (see the existing `grid_layout.cpp`/`fps_meter.cpp` entries) with `src/app` on the include path. Core (`denso_core`) sources are already linked into `denso_tests`.
- **Doc path case (GOTCHA):** the tracked file is `docs/ARCHITECTURE.md` (uppercase). On this case-insensitive FS, `git add docs/architecture.md` (lowercase) silently stages **nothing**. Always `git add docs/ARCHITECTURE.md`. `CLAUDE.md` and `README.md` are at repo root.
- **Style:** 1:1-heritage port — match the surrounding file's comment density, naming (`snake_case` functions, `kConstant`, `PascalCase` types), and idiom. No unrelated refactoring.
- **Scope:** best-effort latest-value-wins push only. No outbox/queue/idempotency/retry, no auth/TLS, no server side. Do not touch the dormant `ReadingSink` (a separate future feature owns it).
- **Fixed values (verbatim):** debounce `kStableFrames = 5`; HTTP timeout `kBrazingTimeoutMs = 5000`; POST path `/api/brazing/update`; zone number range 1–12 (0/blank = not a reporting zone); `SCHEMA_VERSION = 10`.

**Baseline before starting:** run the full suite once and record the count.
```
export PATH=/c/msys64/ucrt64/bin:$PATH
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```
Expected: all pass (currently **143 passed**). Record the number.

---

### Task 1: v10 migration — per-area `zone` number + repo round-trip

Adds the `zone` column, the `CameraArea::zone` field, and carries it through the area read/replace. This unblocks every later task (assembly + UI read it).

**Files:**
- Modify: `src/core/camera/camera.h`
- Modify: `src/core/db/db.cpp`
- Modify: `src/core/camera/repo.cpp`
- Modify: `tests/test_camera_repo.cpp`

**Interfaces:**
- Produces: `std::optional<int> denso::camera::CameraArea::zone;` — carried by `areas_for` / `replace_areas`.

- [ ] **Step 1: Add the field to the domain struct**

In `src/core/camera/camera.h`, in `struct CameraArea`, after `std::string name;` add:
```cpp
    std::optional<int> zone;  // 1..12 reporting zone, or nullopt (ROI-only)
```
(`<optional>` is already included.)

- [ ] **Step 2: Write the failing test**

In `tests/test_camera_repo.cpp`, add a test that a replaced area's `zone` survives a round-trip. Mirror the existing area tests' setup (open in-memory DB, run migrations, insert a camera). Append:
```cpp
TEST_CASE("replace_areas round-trips the zone number", "[camera_repo]") {
    auto db = denso::db::Db::open_in_memory();
    REQUIRE(db);
    REQUIRE(denso::db::run_migrations(db->handle()));

    denso::camera::Camera cam;
    cam.name = "cam";
    cam.camera_type = "usb";
    const auto cam_id = denso::camera::insert(db->handle(), cam);
    REQUIRE(cam_id);

    denso::camera::CameraArea a;
    a.name = "z";
    a.zone = 3;
    a.points = {{0.1f, 0.1f}, {0.9f, 0.1f}, {0.5f, 0.9f}};
    denso::camera::CameraArea b;
    b.name = "roi_only";  // zone stays nullopt
    b.points = {{0.2f, 0.2f}, {0.8f, 0.2f}, {0.5f, 0.8f}};
    REQUIRE(denso::camera::replace_areas(db->handle(), *cam_id, {a, b}));

    const auto got = denso::camera::areas_for(db->handle(), *cam_id);
    REQUIRE(got.size() == 2);
    CHECK(got[0].zone == 3);
    CHECK_FALSE(got[1].zone.has_value());
}
```

- [ ] **Step 3: Add the migration**

In `src/core/db/db.cpp`, change `constexpr int SCHEMA_VERSION = 9;` to `10`. Then, immediately **after** the `if (version < 9) { … }` block and **before** the `PRAGMA user_version = %1` write, add:
```cpp
    if (version < 10) {
        // Per-ROI reporting zone. A camera_area with a `zone` set is reported to
        // the brazing backend under key "zone<n>"; NULL = ROI-only (confinement,
        // no reporting). Additive; existing areas default to NULL.
        if (!run("ALTER TABLE camera_area ADD COLUMN zone INTEGER")) {
            return false;
        }
    }
```

- [ ] **Step 4: Carry the column in the repo**

In `src/core/camera/repo.cpp`, in `areas_for`'s SELECT, add `zone` to the column list and read it back into the struct. Find the query that selects `name, points` for `camera_area` and change it to `SELECT id, camera_id, name, points, zone …` (append `zone` last so it's the highest column index); when building each `CameraArea`, read that last column into the optional:
```cpp
        // `zone` is the last selected column; use its index (e.g. 4 if the list
        // is id,camera_id,name,points,zone). NULL stays nullopt.
        const QVariant zv = q.value(4);
        if (!zv.isNull()) {
            area.zone = zv.toInt();
        }
```
(Adjust the index `4` to match the actual position of `zone` in your SELECT list.)
In `replace_areas`, add `zone` to the INSERT column list and bind it — bind `QVariant()` (SQL NULL) when `zone` is `nullopt`, else the int:
```cpp
        // in the per-area INSERT bind list, alongside name + points:
        query.addBindValue(a.zone ? QVariant(*a.zone) : QVariant(QMetaType(QMetaType::Int)));
```
(Match the file's existing `QSqlQuery`/`prepare`/`addBindValue` idiom exactly — read the current `areas_for`/`replace_areas` bodies and extend the column list + bind order consistently. `#include <QVariant>` and `#include <QMetaType>` if not present.)

- [ ] **Step 5: Build and run the test**

```
export PATH=/c/msys64/ucrt64/bin:$PATH
cmake --build build
./build/tests/denso_tests "[camera_repo]"
```
Expected: PASS (existing camera_repo tests + the new one).

- [ ] **Step 6: Full suite (no regressions)**

Run: `ctest --test-dir build --output-on-failure`
Expected: baseline + 1, all pass.

- [ ] **Step 7: Commit**

```
git add src/core/camera/camera.h src/core/db/db.cpp src/core/camera/repo.cpp tests/test_camera_repo.cpp
git commit -m "feat(camera): v10 migration — per-area zone number + repo round-trip"
```

---

### Task 2: `zone_assembly` — digits → number, grouped by zone (pure)

The core value-reading step. No Qt/network; depends only on OpenCV value types (`NamedDetection`), `camera.h`, and the existing `area_geometry` point-in-polygon.

**Files:**
- Create: `src/app/ui/camera/grid/zone_reading.h`
- Create: `src/app/ui/camera/grid/zone_assembly.h`
- Create: `src/app/ui/camera/grid/zone_assembly.cpp`
- Create: `tests/test_zone_assembly.cpp`
- Modify: `src/app/CMakeLists.txt`, `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `denso::ui::NamedDetection{cv::Rect box; float conf; std::string name;}` (`merge_detections.h`); `denso::camera::CameraArea` (with `zone`, Task 1); `denso::camera::point_in_polygon` (`area_geometry.h`).
- Produces:
  - `struct denso::ui::ZoneReading { int zone_no; int value; float conf; };` (in `zone_reading.h`)
  - `std::optional<int> denso::ui::assemble_zone_value(const std::vector<NamedDetection>& digits_in_zone);`
  - `std::vector<ZoneReading> denso::ui::group_into_zones(const std::vector<NamedDetection>& kept, const std::vector<camera::CameraArea>& areas, float frame_w, float frame_h);`

- [ ] **Step 1: Write the failing test**

Create `tests/test_zone_assembly.cpp`:
```cpp
#include <catch2/catch_test_macros.hpp>

#include "ui/camera/grid/zone_assembly.h"

#include <opencv2/core.hpp>

using denso::ui::assemble_zone_value;
using denso::ui::group_into_zones;
using denso::ui::NamedDetection;
using denso::camera::CameraArea;
using denso::camera::Point;

static NamedDetection digit(int x, const char* name) {
    return NamedDetection{cv::Rect(x, 0, 10, 20), 0.9f, name};
}

TEST_CASE("assemble_zone_value reads digits left-to-right", "[zone_assembly]") {
    // Supplied out of order; x-center ordering must produce 500.
    std::vector<NamedDetection> d = {digit(40, "0"), digit(10, "5"), digit(25, "0")};
    auto v = assemble_zone_value(d);
    REQUIRE(v.has_value());
    CHECK(*v == 500);
}

TEST_CASE("assemble_zone_value collapses leading zeros", "[zone_assembly]") {
    std::vector<NamedDetection> d = {digit(10, "0"), digit(25, "5"), digit(40, "0")};
    CHECK(assemble_zone_value(d) == 50);
}

TEST_CASE("assemble_zone_value on empty or non-digit is nullopt", "[zone_assembly]") {
    CHECK_FALSE(assemble_zone_value({}).has_value());
    CHECK_FALSE(assemble_zone_value({digit(10, "x")}).has_value());
}

TEST_CASE("group_into_zones assigns digits to their area and skips zoneless", "[zone_assembly]") {
    // Two rectangular zones side by side in a 100x100 frame (normalized).
    CameraArea left;
    left.zone = 1;
    left.points = {{0.0f, 0.0f}, {0.5f, 0.0f}, {0.5f, 1.0f}, {0.0f, 1.0f}};
    CameraArea right;
    right.zone = 2;
    right.points = {{0.5f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}, {0.5f, 1.0f}};
    CameraArea roi_only;  // zone == nullopt → excluded
    roi_only.points = left.points;

    // digits at x≈15,30 land in the left zone → "12"; x≈70 lands in right → "3".
    std::vector<NamedDetection> kept = {digit(10, "1"), digit(25, "2"), digit(65, "3")};
    auto zones = group_into_zones(kept, {left, right, roi_only}, 100.0f, 100.0f);

    REQUIRE(zones.size() == 2);
    CHECK(zones[0].zone_no == 1);
    CHECK(zones[0].value == 12);
    CHECK(zones[1].zone_no == 2);
    CHECK(zones[1].value == 3);
}
```

- [ ] **Step 2: Register in CMake**

In `tests/CMakeLists.txt`, after the `test_frame_convert.cpp` block (or any existing entry), add:
```cmake
    test_zone_assembly.cpp
    # zone_assembly is GUI-target code but pure (OpenCV value types + core
    # geometry, no widgets/Qt-net), so compile it in.
    ${CMAKE_SOURCE_DIR}/src/app/ui/camera/grid/zone_assembly.cpp
```
In `src/app/CMakeLists.txt`, after the `ui/camera/grid/stream_pacing.cpp` line, add:
```cmake
    ui/camera/grid/zone_assembly.cpp
```

- [ ] **Step 3: Run to verify it fails**

```
export PATH=/c/msys64/ucrt64/bin:$PATH
cmake -S . -B build -G Ninja
cmake --build build
```
Expected: FAIL — `zone_assembly.h`: No such file or directory.

- [ ] **Step 4: Write `zone_reading.h`**

Create `src/app/ui/camera/grid/zone_reading.h`:
```cpp
// One assembled zone value: which reporting zone, its current number, and the
// aggregate confidence (min digit conf). Shared by the assembly step and the
// debounce aggregator — a tiny header so the aggregator stays free of OpenCV.
#pragma once

namespace denso::ui {

struct ZoneReading {
    int   zone_no = 0;
    int   value   = 0;
    float conf    = 0.0f;
};

} // namespace denso::ui
```

- [ ] **Step 5: Write `zone_assembly.h`**

Create `src/app/ui/camera/grid/zone_assembly.h`:
```cpp
// Turn detected digit boxes into per-zone numbers. Digits inside a zone's ROI
// polygon are sorted left-to-right (by box x-center) and concatenated into an
// integer. Pure — OpenCV value types + core point-in-polygon, no Qt/network — so
// it unit-tests without a camera. Used by DetectionProcessor on the capture
// thread.
#pragma once

#include "camera/camera.h"                                 // CameraArea
#include "ui/camera/grid/zone_reading.h"                   // ZoneReading
#include "ui/camera/shared/detection/merge_detections.h"   // NamedDetection

#include <optional>
#include <vector>

namespace denso::ui {

/// Concatenate the digits (each detection's `name`, a single 0-9 char) in
/// left-to-right box order and parse to int. nullopt when empty or unparseable.
std::optional<int> assemble_zone_value(const std::vector<NamedDetection>& digits_in_zone);

/// For each area with a zone number, collect the kept digits whose box center
/// (normalized by frame_w/frame_h) lies inside the area polygon, assemble, and
/// emit a ZoneReading. Areas with no zone number, or that assemble to nothing,
/// are skipped. `conf` is the min digit confidence in the zone.
std::vector<ZoneReading> group_into_zones(const std::vector<NamedDetection>& kept,
                                          const std::vector<camera::CameraArea>& areas,
                                          float frame_w, float frame_h);

} // namespace denso::ui
```

- [ ] **Step 6: Write `zone_assembly.cpp`**

Create `src/app/ui/camera/grid/zone_assembly.cpp`:
```cpp
#include "ui/camera/grid/zone_assembly.h"

#include "camera/area_geometry.h"  // point_in_polygon

#include <algorithm>
#include <limits>
#include <string>

namespace denso::ui {

std::optional<int> assemble_zone_value(const std::vector<NamedDetection>& digits_in_zone) {
    if (digits_in_zone.empty()) {
        return std::nullopt;
    }
    std::vector<const NamedDetection*> ordered;
    ordered.reserve(digits_in_zone.size());
    for (const NamedDetection& d : digits_in_zone) {
        ordered.push_back(&d);
    }
    // Left-to-right by box x-center.
    std::sort(ordered.begin(), ordered.end(),
              [](const NamedDetection* a, const NamedDetection* b) {
                  return (a->box.x + a->box.width * 0.5) < (b->box.x + b->box.width * 0.5);
              });

    std::string digits;
    for (const NamedDetection* d : ordered) {
        digits += d->name;  // class name is the digit label ("0".."9")
    }
    // Every char must be a decimal digit, else it's not a number we can send.
    if (digits.empty() ||
        !std::all_of(digits.begin(), digits.end(),
                     [](unsigned char c) { return c >= '0' && c <= '9'; })) {
        return std::nullopt;
    }
    try {
        return std::stoi(digits);  // leading zeros collapse; "500" -> 500
    } catch (const std::exception&) {
        return std::nullopt;  // overflow / not representable
    }
}

std::vector<ZoneReading> group_into_zones(const std::vector<NamedDetection>& kept,
                                          const std::vector<camera::CameraArea>& areas,
                                          float frame_w, float frame_h) {
    std::vector<ZoneReading> out;
    if (frame_w <= 0.0f || frame_h <= 0.0f) {
        return out;
    }
    for (const camera::CameraArea& area : areas) {
        if (!area.zone) {
            continue;  // ROI-only area — not reported
        }
        std::vector<NamedDetection> in_zone;
        float min_conf = std::numeric_limits<float>::max();
        for (const NamedDetection& d : kept) {
            const camera::Point c{
                (d.box.x + d.box.width * 0.5f) / frame_w,
                (d.box.y + d.box.height * 0.5f) / frame_h};
            if (camera::point_in_polygon(area.points, c)) {
                in_zone.push_back(d);
                min_conf = std::min(min_conf, d.conf);
            }
        }
        if (const auto v = assemble_zone_value(in_zone)) {
            out.push_back(ZoneReading{*area.zone, *v, min_conf});
        }
    }
    return out;
}

} // namespace denso::ui
```

- [ ] **Step 7: Build + run the tag, then full suite**

```
cmake --build build
./build/tests/denso_tests "[zone_assembly]"
ctest --test-dir build --output-on-failure
```
Expected: `[zone_assembly]` passes (4 cases); full suite baseline + (Task 1) + 4.

- [ ] **Step 8: Commit**

```
git add src/app/ui/camera/grid/zone_reading.h src/app/ui/camera/grid/zone_assembly.h \
        src/app/ui/camera/grid/zone_assembly.cpp tests/test_zone_assembly.cpp \
        src/app/CMakeLists.txt tests/CMakeLists.txt
git commit -m "feat(zone): assemble digits into per-zone numbers (zone_assembly)"
```

---

### Task 3: `zone_aggregator` — debounce + change detection (pure)

Machine-wide state: holds each zone's stable value across all cameras, and returns the full snapshot to send only when a stable value changed. No Qt, no mutex (the reporter adds the lock).

**Files:**
- Create: `src/app/ui/camera/grid/zone_aggregator.h`
- Create: `src/app/ui/camera/grid/zone_aggregator.cpp`
- Create: `tests/test_zone_aggregator.cpp`
- Modify: `src/app/CMakeLists.txt`, `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `denso::ui::ZoneReading` (`zone_reading.h`).
- Produces:
  - `constexpr int denso::ui::kStableFrames = 5;`
  - `class denso::ui::ZoneAggregator` with `explicit ZoneAggregator(int stable_frames = kStableFrames);` and `std::optional<std::map<int,int>> observe(const std::vector<ZoneReading>& zones);` (returns the full `{zone_no→value}` snapshot when any zone's stable value changed vs last sent).

- [ ] **Step 1: Write the failing test**

Create `tests/test_zone_aggregator.cpp`:
```cpp
#include <catch2/catch_test_macros.hpp>

#include "ui/camera/grid/zone_aggregator.h"

using denso::ui::ZoneAggregator;
using denso::ui::ZoneReading;

static std::vector<ZoneReading> obs(int zone, int value) {
    return {ZoneReading{zone, value, 0.9f}};
}

TEST_CASE("value sends only after it is stable for kStableFrames", "[zone_aggregator]") {
    ZoneAggregator agg(3);  // stable after 3 identical observations
    CHECK_FALSE(agg.observe(obs(1, 500)).has_value());  // 1
    CHECK_FALSE(agg.observe(obs(1, 500)).has_value());  // 2
    auto snap = agg.observe(obs(1, 500));               // 3 -> stable + new
    REQUIRE(snap.has_value());
    CHECK((*snap).at(1) == 500);
}

TEST_CASE("an unchanged stable value does not re-send", "[zone_aggregator]") {
    ZoneAggregator agg(1);  // stable immediately
    REQUIRE(agg.observe(obs(1, 500)).has_value());       // first send
    CHECK_FALSE(agg.observe(obs(1, 500)).has_value());   // same -> no send
}

TEST_CASE("a change in one zone sends the full snapshot of all zones", "[zone_aggregator]") {
    ZoneAggregator agg(1);
    REQUIRE(agg.observe(obs(1, 500)).has_value());
    REQUIRE(agg.observe(obs(2, 200)).has_value());
    auto snap = agg.observe(obs(1, 510));  // zone1 changed
    REQUIRE(snap.has_value());
    CHECK((*snap).at(1) == 510);
    CHECK((*snap).at(2) == 200);  // full snapshot carries zone2 too
}

TEST_CASE("a frame missing a zone keeps its last stable value", "[zone_aggregator]") {
    ZoneAggregator agg(1);
    REQUIRE(agg.observe(obs(1, 500)).has_value());     // zone1 stable at 500
    CHECK(agg.observe(obs(2, 200)).has_value());       // zone2 new -> sends
    CHECK_FALSE(agg.observe(obs(2, 200)).has_value()); // zone2 unchanged -> no send
    // Observations that never mention zone 1 must not have reset or cleared it:
    // a later change still carries zone1's retained 500 base alongside the change.
    auto s2 = agg.observe(obs(1, 505));                // zone1 changes 500 -> 505
    REQUIRE(s2.has_value());
    CHECK((*s2).at(1) == 505);
    CHECK((*s2).at(2) == 200);  // zone2 retained
}
```

- [ ] **Step 2: Register in CMake**

In `tests/CMakeLists.txt`, after the `test_zone_assembly.cpp` block, add:
```cmake
    test_zone_aggregator.cpp
    # zone_aggregator is pure std (no Qt/OpenCV), so compile it in.
    ${CMAKE_SOURCE_DIR}/src/app/ui/camera/grid/zone_aggregator.cpp
```
In `src/app/CMakeLists.txt`, after `ui/camera/grid/zone_assembly.cpp`, add:
```cmake
    ui/camera/grid/zone_aggregator.cpp
```

- [ ] **Step 3: Run to verify it fails**

```
cmake --build build
```
Expected: FAIL — `zone_aggregator.h`: No such file or directory.

- [ ] **Step 4: Write `zone_aggregator.h`**

Create `src/app/ui/camera/grid/zone_aggregator.h`:
```cpp
// Machine-wide zone state with per-zone debounce. Each camera feeds its zones in
// via observe(); a value must repeat kStableFrames times before it's "stable".
// When any zone's stable value differs from what was last sent, observe() returns
// the full latest-value snapshot ({zone_no -> value}) to POST. Pure (std only) —
// the ZoneReporter wraps it with a mutex and the network marshal.
#pragma once

#include "ui/camera/grid/zone_reading.h"

#include <map>
#include <optional>
#include <vector>

namespace denso::ui {

constexpr int kStableFrames = 5;  // identical observations before a value is sent

class ZoneAggregator {
public:
    explicit ZoneAggregator(int stable_frames = kStableFrames);

    /// Feed one camera's assembled zones. Returns the full snapshot to send when
    /// any zone's stable value changed vs the last sent snapshot, else nullopt.
    /// Zones absent from `zones` are left untouched (occlusion tolerance).
    std::optional<std::map<int, int>> observe(const std::vector<ZoneReading>& zones);

private:
    struct Debounce {
        int candidate = 0;   // value currently accumulating
        int count = 0;       // consecutive observations of `candidate`
        bool has_stable = false;
        int stable = 0;      // last value that reached stability
    };

    int stable_frames_;
    std::map<int, Debounce> zones_;   // zone_no -> debounce state
    std::map<int, int> last_sent_;    // zone_no -> value in the last snapshot
};

} // namespace denso::ui
```

- [ ] **Step 5: Write `zone_aggregator.cpp`**

Create `src/app/ui/camera/grid/zone_aggregator.cpp`:
```cpp
#include "ui/camera/grid/zone_aggregator.h"

#include <algorithm>

namespace denso::ui {

ZoneAggregator::ZoneAggregator(int stable_frames)
    : stable_frames_(std::max(1, stable_frames)) {}

std::optional<std::map<int, int>> ZoneAggregator::observe(
    const std::vector<ZoneReading>& zones) {
    bool changed = false;

    for (const ZoneReading& z : zones) {
        Debounce& d = zones_[z.zone_no];
        if (z.value == d.candidate) {
            ++d.count;
        } else {
            d.candidate = z.value;
            d.count = 1;
        }
        if (d.count >= stable_frames_) {
            if (!d.has_stable || d.stable != d.candidate) {
                d.has_stable = true;
                d.stable = d.candidate;
                // Did this differ from what we last sent for this zone?
                const auto it = last_sent_.find(z.zone_no);
                if (it == last_sent_.end() || it->second != d.stable) {
                    changed = true;
                }
            }
        }
    }

    if (!changed) {
        return std::nullopt;
    }

    // Build the full snapshot of every zone that has ever reached a stable value.
    std::map<int, int> snapshot;
    for (const auto& [zone_no, d] : zones_) {
        if (d.has_stable) {
            snapshot[zone_no] = d.stable;
        }
    }
    last_sent_ = snapshot;
    return snapshot;
}

} // namespace denso::ui
```

- [ ] **Step 6: Build + run the tag, then full suite**

```
cmake --build build
./build/tests/denso_tests "[zone_aggregator]"
ctest --test-dir build --output-on-failure
```
Expected: `[zone_aggregator]` passes; full suite still green (+4).

- [ ] **Step 7: Commit**

```
git add src/app/ui/camera/grid/zone_aggregator.h src/app/ui/camera/grid/zone_aggregator.cpp \
        tests/test_zone_aggregator.cpp src/app/CMakeLists.txt tests/CMakeLists.txt
git commit -m "feat(zone): per-zone debounce + change-snapshot aggregator"
```

---

### Task 4: `brazing::config` — persist enable + base URL (core)

Two-field config over the existing `settings` key/value table, in `denso_core` so it's unit-tested off-device (like the other repos).

**Files:**
- Create: `src/core/brazing/config.h`
- Create: `src/core/brazing/config.cpp`
- Create: `tests/test_brazing_config.cpp`
- Modify: `src/core/CMakeLists.txt`, `tests/CMakeLists.txt`

**Interfaces:**
- Produces:
  - `struct denso::brazing::BrazingConfig { bool enabled = false; std::string base_url; };`
  - `BrazingConfig denso::brazing::load(const QSqlDatabase& db);`
  - `void denso::brazing::save(const QSqlDatabase& db, const BrazingConfig& cfg);`

- [ ] **Step 1: Write the failing test**

Create `tests/test_brazing_config.cpp`:
```cpp
#include <catch2/catch_test_macros.hpp>

#include "brazing/config.h"
#include "db/db.h"

TEST_CASE("brazing config round-trips through the settings table", "[brazing_config]") {
    auto db = denso::db::Db::open_in_memory();
    REQUIRE(db);
    REQUIRE(denso::db::run_migrations(db->handle()));

    // Defaults when nothing is stored.
    const denso::brazing::BrazingConfig def = denso::brazing::load(db->handle());
    CHECK_FALSE(def.enabled);
    CHECK(def.base_url.empty());

    denso::brazing::BrazingConfig cfg;
    cfg.enabled = true;
    cfg.base_url = "http://192.168.1.50:8098";
    denso::brazing::save(db->handle(), cfg);

    const denso::brazing::BrazingConfig got = denso::brazing::load(db->handle());
    CHECK(got.enabled);
    CHECK(got.base_url == "http://192.168.1.50:8098");
}
```

- [ ] **Step 2: Register in CMake**

In `src/core/CMakeLists.txt`, add `brazing/config.cpp` to the `denso_core` source list (place it near the other feature dirs like `settings/repo.cpp` / `network/…`). In `tests/CMakeLists.txt`, add `test_brazing_config.cpp` to the `add_executable(denso_tests …)` list (no source path — `config.cpp` is in the linked `denso_core`).

- [ ] **Step 3: Run to verify it fails**

```
cmake -S . -B build -G Ninja
cmake --build build
```
Expected: FAIL — `brazing/config.h`: No such file or directory.

- [ ] **Step 4: Write `config.h`**

Create `src/core/brazing/config.h`:
```cpp
// The brazing HTTP reporter's persisted config: whether reporting is on, and the
// server base URL (path is fixed at /api/brazing/update). Stored as two rows in
// the `settings` key/value table (keys "brazing.enabled" / "brazing.base_url").
// Qt-free of widgets — denso_core only.
#pragma once

#include <QSqlDatabase>

#include <string>

namespace denso::brazing {

struct BrazingConfig {
    bool        enabled = false;
    std::string base_url;  // e.g. "http://192.168.1.50:8098"; empty = unset
};

/// Load config, defaulting to {enabled=false, base_url=""} for missing keys.
BrazingConfig load(const QSqlDatabase& db);

/// Persist both fields. Write errors are silently ignored (a DB hiccup must
/// never crash the UI).
void save(const QSqlDatabase& db, const BrazingConfig& cfg);

} // namespace denso::brazing
```

- [ ] **Step 5: Write `config.cpp`**

Create `src/core/brazing/config.cpp` (mirrors `settings/repo.cpp`'s get/set idiom):
```cpp
#include "brazing/config.h"

#include <QSqlQuery>
#include <QString>
#include <QVariant>

#include <optional>

namespace denso::brazing {
namespace {

std::optional<QString> get(const QSqlDatabase& db, const QString& key) {
    QSqlQuery q(db);
    q.prepare(QStringLiteral("SELECT value FROM settings WHERE key = ?"));
    q.addBindValue(key);
    if (q.exec() && q.next()) {
        return q.value(0).toString();
    }
    return std::nullopt;
}

void set(const QSqlDatabase& db, const QString& key, const QString& value) {
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "INSERT INTO settings (key, value) VALUES (?, ?) "
        "ON CONFLICT(key) DO UPDATE SET value = excluded.value"));
    q.addBindValue(key);
    q.addBindValue(value);
    q.exec();
}

} // namespace

BrazingConfig load(const QSqlDatabase& db) {
    BrazingConfig out;
    if (const auto v = get(db, QStringLiteral("brazing.enabled"))) {
        out.enabled = (*v == QStringLiteral("1"));
    }
    if (const auto v = get(db, QStringLiteral("brazing.base_url"))) {
        out.base_url = v->toStdString();
    }
    return out;
}

void save(const QSqlDatabase& db, const BrazingConfig& cfg) {
    set(db, QStringLiteral("brazing.enabled"),
        cfg.enabled ? QStringLiteral("1") : QStringLiteral("0"));
    set(db, QStringLiteral("brazing.base_url"), QString::fromStdString(cfg.base_url));
}

} // namespace denso::brazing
```

- [ ] **Step 6: Build + run the tag, then full suite**

```
cmake --build build
./build/tests/denso_tests "[brazing_config]"
ctest --test-dir build --output-on-failure
```
Expected: `[brazing_config]` passes; full suite green.

- [ ] **Step 7: Commit**

```
git add src/core/brazing/config.h src/core/brazing/config.cpp tests/test_brazing_config.cpp \
        src/core/CMakeLists.txt tests/CMakeLists.txt
git commit -m "feat(brazing): persist reporter enable + base URL (core config)"
```

---

### Task 5: `brazing_payload` — combined JSON body (pure)

The exact `{"zone1":500,"zone2":200}` string, isolated from Qt so it's unit-tested and reused by the client. `std::map` key order gives deterministic zone ordering.

**Files:**
- Create: `src/app/ui/camera/grid/brazing_payload.h`
- Create: `src/app/ui/camera/grid/brazing_payload.cpp`
- Create: `tests/test_brazing_payload.cpp`
- Modify: `src/app/CMakeLists.txt`, `tests/CMakeLists.txt`

**Interfaces:**
- Produces: `std::string denso::ui::build_brazing_payload(const std::map<int,int>& zones);`

- [ ] **Step 1: Write the failing test**

Create `tests/test_brazing_payload.cpp`:
```cpp
#include <catch2/catch_test_macros.hpp>

#include "ui/camera/grid/brazing_payload.h"

using denso::ui::build_brazing_payload;

TEST_CASE("build_brazing_payload emits zones in ascending order", "[brazing_payload]") {
    CHECK(build_brazing_payload({{2, 200}, {1, 500}}) ==
          R"({"zone1":500,"zone2":200})");
}

TEST_CASE("build_brazing_payload of a single zone", "[brazing_payload]") {
    CHECK(build_brazing_payload({{2, 500}}) == R"({"zone2":500})");
}

TEST_CASE("build_brazing_payload of no zones is an empty object", "[brazing_payload]") {
    CHECK(build_brazing_payload({}) == "{}");
}
```

- [ ] **Step 2: Register in CMake**

In `tests/CMakeLists.txt`, after the `test_zone_aggregator.cpp` block, add:
```cmake
    test_brazing_payload.cpp
    # brazing_payload is pure std::string (no Qt/OpenCV), so compile it in.
    ${CMAKE_SOURCE_DIR}/src/app/ui/camera/grid/brazing_payload.cpp
```
In `src/app/CMakeLists.txt`, after `ui/camera/grid/zone_aggregator.cpp`, add:
```cmake
    ui/camera/grid/brazing_payload.cpp
```

- [ ] **Step 3: Run to verify it fails**

```
cmake --build build
```
Expected: FAIL — `brazing_payload.h`: No such file or directory.

- [ ] **Step 4: Write header + implementation**

Create `src/app/ui/camera/grid/brazing_payload.h`:
```cpp
// The combined POST body for /api/brazing/update: {"zone<n>": value, ...}, keys
// in ascending zone order (std::map). Pure std::string so it unit-tests without
// Qt; the BrazingClient wraps it in the HTTP request.
#pragma once

#include <map>
#include <string>

namespace denso::ui {

std::string build_brazing_payload(const std::map<int, int>& zones);

} // namespace denso::ui
```
Create `src/app/ui/camera/grid/brazing_payload.cpp`:
```cpp
#include "ui/camera/grid/brazing_payload.h"

namespace denso::ui {

std::string build_brazing_payload(const std::map<int, int>& zones) {
    std::string out = "{";
    bool first = true;
    for (const auto& [zone_no, value] : zones) {
        if (!first) {
            out += ",";
        }
        first = false;
        out += "\"zone";
        out += std::to_string(zone_no);
        out += "\":";
        out += std::to_string(value);
    }
    out += "}";
    return out;
}

} // namespace denso::ui
```

- [ ] **Step 5: Build + run the tag, then full suite**

```
cmake --build build
./build/tests/denso_tests "[brazing_payload]"
ctest --test-dir build --output-on-failure
```
Expected: `[brazing_payload]` passes (3 cases); full suite green.

- [ ] **Step 6: Commit**

```
git add src/app/ui/camera/grid/brazing_payload.h src/app/ui/camera/grid/brazing_payload.cpp \
        tests/test_brazing_payload.cpp src/app/CMakeLists.txt tests/CMakeLists.txt
git commit -m "feat(brazing): combined zone JSON payload builder"
```

---

### Task 6: `BrazingClient` — async best-effort POST (Qt6::Network)

A GUI-thread `QObject` that POSTs the combined payload with a bounded timeout and logs-and-drops on failure. No retry. Not unit-tested (network); verified by build + the test-server smoke.

**Files:**
- Create: `src/app/ui/camera/grid/brazing_client.h`
- Create: `src/app/ui/camera/grid/brazing_client.cpp`
- Modify: `src/app/CMakeLists.txt`

**Interfaces:**
- Consumes: `build_brazing_payload` (Task 5).
- Produces: `class denso::ui::BrazingClient : public QObject` with `explicit BrazingClient(std::string base_url, QObject* parent = nullptr);` and `void send(const std::map<int,int>& zones);`.

- [ ] **Step 1: Write `brazing_client.h`**

Create `src/app/ui/camera/grid/brazing_client.h`:
```cpp
// Pushes the combined zone snapshot to the brazing backend. Lives on the GUI
// thread; send() fires an async POST to {base_url}/api/brazing/update via
// QNetworkAccessManager with a bounded timeout. Best-effort: a failed/slow/
// unreachable POST is logged (throttled) and dropped — no queue, no retry (the
// next zone change re-sends the full snapshot). The ZoneReporter marshals
// snapshots here with common::post_to_gui.
#pragma once

#include <QObject>
#include <QString>

#include <map>
#include <string>

class QNetworkAccessManager;

namespace denso::ui {

class BrazingClient : public QObject {
    Q_OBJECT

public:
    explicit BrazingClient(std::string base_url, QObject* parent = nullptr);

    /// POST {"zone<n>": value, ...}. No-op if base_url is empty.
    void send(const std::map<int, int>& zones);

private:
    QString base_url_;
    QNetworkAccessManager* nam_ = nullptr;
};

} // namespace denso::ui
```

- [ ] **Step 2: Write `brazing_client.cpp`**

Create `src/app/ui/camera/grid/brazing_client.cpp`:
```cpp
#include "ui/camera/grid/brazing_client.h"

#include "ui/camera/grid/brazing_payload.h"

#include <QByteArray>
#include <QDebug>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

namespace denso::ui {

namespace {
constexpr int kBrazingTimeoutMs = 5000;  // abort a stuck POST (soak-safe)
}

BrazingClient::BrazingClient(std::string base_url, QObject* parent)
    : QObject(parent),
      base_url_(QString::fromStdString(base_url)),
      nam_(new QNetworkAccessManager(this)) {
    // Trim a trailing slash so base_url + path doesn't double up.
    while (base_url_.endsWith('/')) {
        base_url_.chop(1);
    }
}

void BrazingClient::send(const std::map<int, int>& zones) {
    if (base_url_.isEmpty()) {
        return;
    }
    const QUrl url(base_url_ + QStringLiteral("/api/brazing/update"));
    if (!url.isValid()) {
        qWarning().noquote() << "[brazing] invalid base URL:" << base_url_;
        return;
    }
    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::ContentTypeHeader,
                  QByteArrayLiteral("application/json"));
    req.setTransferTimeout(kBrazingTimeoutMs);

    const QByteArray body =
        QByteArray::fromStdString(build_brazing_payload(zones));
    QNetworkReply* reply = nam_->post(req, body);
    QObject::connect(reply, &QNetworkReply::finished, reply, [reply] {
        if (reply->error() != QNetworkReply::NoError) {
            // Best-effort: log and drop. Next change re-sends the full snapshot.
            qWarning().noquote() << "[brazing] POST failed:" << reply->errorString();
        }
        reply->deleteLater();
    });
}

} // namespace denso::ui
```
(Note: `qWarning` is already routed to `denso.log`; Qt itself throttles nothing, but a best-effort POST only fires on a stable *change*, so the log can't spin.)

- [ ] **Step 3: Register in CMake and build**

In `src/app/CMakeLists.txt`, after `ui/camera/grid/brazing_payload.cpp`, add:
```cmake
    ui/camera/grid/brazing_client.cpp
```
(`denso` already links `Qt6::Network` — see `target_link_libraries(denso …)`.) Then:
```
export PATH=/c/msys64/ucrt64/bin:$PATH
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```
Expected: clean build; full suite still green (no new tests this task).

- [ ] **Step 4: Commit**

```
git add src/app/ui/camera/grid/brazing_client.h src/app/ui/camera/grid/brazing_client.cpp \
        src/app/CMakeLists.txt
git commit -m "feat(brazing): async best-effort HTTP client (QNetworkAccessManager)"
```

---

### Task 7: `ZoneReporter` + processor seam + grid wiring (integration)

The `ZoneSink` seam on the processor, the thread-safe reporter wrapping the aggregator, and the grid ownership/wiring that makes zone POSTs actually happen. Verified by build + suite-green + test-server smoke.

**Files:**
- Modify: `src/app/ui/camera/grid/frame_processor.h`
- Modify: `src/app/ui/camera/grid/frame_processor.cpp`
- Create: `src/app/ui/camera/grid/zone_reporter.h`
- Create: `src/app/ui/camera/grid/zone_reporter.cpp`
- Modify: `src/app/ui/camera/grid/camera_grid.h`
- Modify: `src/app/ui/camera/grid/camera_grid.cpp`
- Modify: `src/app/CMakeLists.txt`

**Interfaces:**
- Consumes: `group_into_zones` (Task 2), `ZoneAggregator` (Task 3), `BrazingClient` (Task 6), `brazing::load` (Task 4), `common::post_to_gui`.
- Produces:
  - `struct denso::ui::ZoneSink { virtual void on_zones(int64_t camera_id, const std::vector<ZoneReading>& zones) = 0; };` (in `frame_processor.h`)
  - `class denso::ui::ZoneReporter : public ZoneSink` with `explicit ZoneReporter(std::function<void(const std::map<int,int>&)> on_snapshot, int stable_frames = kStableFrames);`

- [ ] **Step 1: Add the `ZoneSink` seam + processor member**

In `src/app/ui/camera/grid/frame_processor.h`, add the include `#include "ui/camera/grid/zone_reading.h"` near the other includes, then after the `ReadingSink` struct add:
```cpp
/// Optional per-frame hook that receives the camera's assembled zone values (the
/// brazing reporter). Same capture-thread hot-path contract as ReadingSink: the
/// implementation MUST hand off (no blocking / no I/O inline) and return.
struct ZoneSink {
    virtual ~ZoneSink() = default;
    virtual void on_zones(int64_t camera_id, const std::vector<ZoneReading>& zones) = 0;
};
```
In `class DetectionProcessor`, change the constructor declaration to take a trailing `ZoneSink*`:
```cpp
    DetectionProcessor(int degrees, double pitch, double roll,
                       std::vector<ModelRun> models,
                       std::vector<denso::camera::CameraArea> areas = {},
                       int64_t camera_id = 0, ReadingSink* sink = nullptr,
                       ZoneSink* zone_sink = nullptr);
```
and add the member after `ReadingSink* sink_ = nullptr;`:
```cpp
    ZoneSink* zone_sink_ = nullptr;  // non-owning; null = no zone reporting
```

- [ ] **Step 2: Feed the reporter from `process()`**

In `src/app/ui/camera/grid/frame_processor.cpp`, add the include `#include "ui/camera/grid/zone_assembly.h"`. Update the constructor definition to store the new arg (match the current initializer list; append `, zone_sink_(zone_sink)` and add the parameter). Then, right after the existing `ReadingSink` block (the `if (sink_) { … sink_->on_reading(…); }`), add:
```cpp
    if (zone_sink_) {
        zone_sink_->on_zones(camera_id_, group_into_zones(kept, areas_, w, h));
    }
```
(`kept`, `areas_`, `w`, `h` are all already in scope in `process()`.)

- [ ] **Step 3: Write `zone_reporter.h`**

Create `src/app/ui/camera/grid/zone_reporter.h`:
```cpp
// The machine's ZoneSink: every camera's DetectionProcessor feeds its assembled
// zones here (from capture threads), so it locks a mutex around a ZoneAggregator.
// When a zone's stable value changes, it invokes on_snapshot with the full
// {zone_no -> value} map — the wiring passes a callback that marshals to the GUI
// thread's BrazingClient (post_to_gui), so capture threads never touch the
// network.
#pragma once

#include "ui/camera/grid/frame_processor.h"   // ZoneSink
#include "ui/camera/grid/zone_aggregator.h"

#include <functional>
#include <map>
#include <mutex>

namespace denso::ui {

class ZoneReporter : public ZoneSink {
public:
    explicit ZoneReporter(std::function<void(const std::map<int, int>&)> on_snapshot,
                          int stable_frames = kStableFrames);

    void on_zones(int64_t camera_id, const std::vector<ZoneReading>& zones) override;

private:
    std::function<void(const std::map<int, int>&)> on_snapshot_;
    std::mutex mutex_;
    ZoneAggregator aggregator_;
};

} // namespace denso::ui
```

- [ ] **Step 4: Write `zone_reporter.cpp`**

Create `src/app/ui/camera/grid/zone_reporter.cpp`:
```cpp
#include "ui/camera/grid/zone_reporter.h"

#include <utility>

namespace denso::ui {

ZoneReporter::ZoneReporter(std::function<void(const std::map<int, int>&)> on_snapshot,
                           int stable_frames)
    : on_snapshot_(std::move(on_snapshot)), aggregator_(stable_frames) {}

void ZoneReporter::on_zones(int64_t /*camera_id*/,
                            const std::vector<ZoneReading>& zones) {
    std::optional<std::map<int, int>> snapshot;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot = aggregator_.observe(zones);
    }
    if (snapshot && on_snapshot_) {
        on_snapshot_(*snapshot);  // marshals to the GUI thread (set at wiring)
    }
}

} // namespace denso::ui
```

- [ ] **Step 5: Own the reporter + client in the grid (header)**

In `src/app/ui/camera/grid/camera_grid.h`, add forward declarations near `class CameraStream;`:
```cpp
class BrazingClient;
class ZoneReporter;
```
and add members after `std::shared_ptr<EngineRegistry> engines_;`:
```cpp
    std::unique_ptr<BrazingClient> brazing_client_;  // GUI-thread HTTP sender
    std::unique_ptr<ZoneReporter> reporter_;         // shared ZoneSink (machine)
```
(`<memory>` is already included.)

- [ ] **Step 6: Create + wire + tear down in the grid (source)**

In `src/app/ui/camera/grid/camera_grid.cpp`, add includes:
```cpp
#include "brazing/config.h"
#include "ui/camera/grid/brazing_client.h"
#include "ui/camera/grid/zone_reporter.h"
#include "ui/common/async_runner.h"   // post_to_gui

#include <functional>
#include <map>
```
At the **end of `clear()`** (after the tiles are deleted), reset the reporter then the client — streams are already stopped/joined above, so no capture thread can still call the reporter:
```cpp
    reporter_.reset();
    brazing_client_.reset();
```
In `reload()`, **after** `clear()` and the early `cams.empty()` return, before the tile loop, create the client + reporter when enabled:
```cpp
    const brazing::BrazingConfig bcfg = brazing::load(db_);
    if (bcfg.enabled && !bcfg.base_url.empty()) {
        brazing_client_ = std::make_unique<BrazingClient>(bcfg.base_url);
        BrazingClient* client = brazing_client_.get();
        reporter_ = std::make_unique<ZoneReporter>(
            [this, client](const std::map<int, int>& snap) {
                // Called from a capture thread; hop to the GUI thread to POST.
                common::post_to_gui(client, [client, snap] { client->send(snap); });
            });
    }
```
Then, in the `DetectionProcessor` construction (the `else` branch that builds `runs`), pass the camera id + the reporter as the zone sink:
```cpp
            proc = std::make_unique<DetectionProcessor>(
                static_cast<int>(cam.rotation), cam.pitch, cam.roll,
                std::move(runs), std::move(areas), cam.id, /*ReadingSink*/ nullptr,
                /*ZoneSink*/ reporter_.get());
```
(`reporter_.get()` is `nullptr` when reporting is disabled, so the processor pays nothing — matching the existing sink guard.)

- [ ] **Step 7: Register `zone_reporter.cpp` and build**

In `src/app/CMakeLists.txt`, after `ui/camera/grid/brazing_client.cpp`, add:
```cmake
    ui/camera/grid/zone_reporter.cpp
```
Then:
```
export PATH=/c/msys64/ucrt64/bin:$PATH
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```
Expected: clean build; full suite green (no new tests this task — logic is covered by Tasks 2/3/5).

- [ ] **Step 8: On-device / test-server smoke (manual — record result)**

Start the stand-in server: `cd d:/workspace/test-server && python server.py --host 0.0.0.0 --port 8098`. In the app, enable reporting + set the base URL to `http://<pc-ip>:8098` (Task 9 UI, or seed the DB), draw an ROI on a live camera and set its zone number (Task 8 UI), then change the displayed number. Confirm a `POST /api/brazing/update` with `{"zoneN": …}` and the right value lands in the server log (`curl http://localhost:8098/api/state`). Stop the server and confirm the app keeps running (best-effort drop, no hang/freeze). Record: value correct? no hang on server-down?

- [ ] **Step 9: Commit**

```
git add src/app/ui/camera/grid/frame_processor.h src/app/ui/camera/grid/frame_processor.cpp \
        src/app/ui/camera/grid/zone_reporter.h src/app/ui/camera/grid/zone_reporter.cpp \
        src/app/ui/camera/grid/camera_grid.h src/app/ui/camera/grid/camera_grid.cpp \
        src/app/CMakeLists.txt
git commit -m "feat(brazing): ZoneSink seam + reporter + grid wiring (zones POST on change)"
```

---

### Task 8: Areas UI — per-area zone number input

Adds a zone-number control to the Areas step so an operator sets which ROI is which reporting zone. Verified by build + manual UI smoke.

**Files:**
- Modify: `src/app/ui/camera/dialog/areas_page.h`
- Modify: `src/app/ui/camera/dialog/areas_page.cpp`

- [ ] **Step 1: Add the spin box to the header**

In `src/app/ui/camera/dialog/areas_page.h`, add `class QSpinBox;` to the forward declarations (next to `class QLineEdit;`), and add a member after `QLineEdit* name_edit_ = nullptr;`:
```cpp
    QSpinBox* zone_edit_ = nullptr;  // 0 = ROI-only; 1..12 = reporting zone
```

- [ ] **Step 2: Build + wire the spin box**

In `src/app/ui/camera/dialog/areas_page.cpp`, add `#include <QSpinBox>`. After the `name_edit_` block (the `side->addWidget(name_edit_);` line), add a labelled zone spin box that writes the selected area's `zone` (0 → nullopt):
```cpp
    side->addWidget(dim_label(QStringLiteral("Report as zone (0 = none)")));
    zone_edit_ = new QSpinBox;
    zone_edit_->setRange(0, 12);
    connect(zone_edit_, QOverload<int>::of(&QSpinBox::valueChanged), this,
            [this](int z) {
                const int row = list_->currentRow();
                if (row >= 0 && row < static_cast<int>(areas_.size())) {
                    auto& a = areas_[static_cast<size_t>(row)];
                    a.zone = (z == 0) ? std::optional<int>{} : std::optional<int>{z};
                }
            });
    side->addWidget(zone_edit_);
```

- [ ] **Step 3: Seed + reset the spin box on selection/new/load**

In `select_area()`, after `name_edit_->setText(...)`, add:
```cpp
    zone_edit_->setValue(a.zone.value_or(0));
```
In the `+ New area` button lambda and in `load()` (wherever `name_edit_->clear();` runs), add `zone_edit_->setValue(0);` alongside it so a fresh area starts zoneless.

- [ ] **Step 4: Build and full suite**

```
export PATH=/c/msys64/ucrt64/bin:$PATH
cmake --build build
ctest --test-dir build --output-on-failure
```
Expected: clean build; suite green (no new tests — persistence is covered by Task 1).

- [ ] **Step 5: Manual UI smoke (record result)**

Open the Camera wizard → Areas step, draw an ROI, set its zone to e.g. 2, Finish; reopen the camera and confirm the Areas step shows zone 2 for that ROI (round-trips through `replace_areas`/`areas_for`). Record: persisted?

- [ ] **Step 6: Commit**

```
git add src/app/ui/camera/dialog/areas_page.h src/app/ui/camera/dialog/areas_page.cpp
git commit -m "feat(areas): per-ROI zone number input"
```

---

### Task 9: Settings UI — Server panel (enable + base URL)

A compact "Server" page in the Settings dialog to toggle reporting and set the base URL, persisted via `brazing::config`. Verified by build + manual UI smoke.

**Files:**
- Modify: `src/app/ui/settings/settings_dialog.h`
- Modify: `src/app/ui/settings/settings_dialog.cpp`

- [ ] **Step 1: Declare the builder + widgets**

In `src/app/ui/settings/settings_dialog.h`, add `class QLineEdit;` to the forward declarations. Add a private method declaration next to `build_about()`:
```cpp
    QWidget* build_server();
```
and members near the other page widgets:
```cpp
    // Server (brazing reporter)
    QCheckBox* brazing_enabled_ = nullptr;
    QLineEdit* brazing_url_ = nullptr;
```

- [ ] **Step 2: Build the panel and add it to the nav**

In `src/app/ui/settings/settings_dialog.cpp`, add includes `#include "brazing/config.h"` and `#include <QLineEdit>` (and `<QCheckBox>`/`<QPushButton>` if not already present). Implement `build_server()` mirroring `build_system()`'s layout idiom, loading current config and saving on a button:
```cpp
QWidget* SettingsDialog::build_server() {
    auto* page = new QWidget;
    auto* v = new QVBoxLayout(page);

    const brazing::BrazingConfig cfg = brazing::load(db_);

    brazing_enabled_ = new QCheckBox(QStringLiteral("Send zone readings to server"));
    brazing_enabled_->setChecked(cfg.enabled);
    v->addWidget(brazing_enabled_);

    brazing_url_ = new QLineEdit(QString::fromStdString(cfg.base_url));
    brazing_url_->setPlaceholderText(QStringLiteral("http://192.168.1.50:8098"));
    v->addWidget(brazing_url_);

    auto* save = new QPushButton(QStringLiteral("Save"));
    connect(save, &QPushButton::clicked, this, [this] {
        brazing::BrazingConfig out;
        out.enabled = brazing_enabled_->isChecked();
        out.base_url = brazing_url_->text().trimmed().toStdString();
        brazing::save(db_, out);
    });
    v->addWidget(save);
    v->addStretch(1);
    return page;
}
```
Then register the page in the nav where the other panels are added (find where `build_about()` / `build_network()` results are added to `stack_` and `nav_`) and add a "Server" entry with `stack_->addWidget(build_server());` and the matching `nav_` item, following the exact pattern used for the sibling panels.

- [ ] **Step 3: Build and full suite**

```
cmake --build build
ctest --test-dir build --output-on-failure
```
Expected: clean build; suite green.

- [ ] **Step 4: Manual UI smoke (record result)**

Open Settings → Server, tick enable, set the URL, Save; reopen Settings and confirm both persisted (via `brazing::load`). Record: persisted? (Note: the grid reads config on `reload()` — toggling requires the camera view to rebuild to take effect; acceptable for v1.)

- [ ] **Step 5: Commit**

```
git add src/app/ui/settings/settings_dialog.h src/app/ui/settings/settings_dialog.cpp
git commit -m "feat(settings): Server panel — enable + base URL for zone reporting"
```

---

### Task 10: Documentation — README + architecture + CLAUDE

Developer-facing docs for the new subsystem.

**Files:**
- Modify: `README.md`
- Modify: `docs/ARCHITECTURE.md` (uppercase — see Global Constraints gotcha)
- Modify: `CLAUDE.md`

- [ ] **Step 1: README — "Brazing zone reporting" section**

In `README.md`, add a section for other developers covering: what a zone is (a named ROI with a zone number, 1–12); how digits are assembled left-to-right into a number; the combined payload shape (`POST {base}/api/brazing/update` → `{"zone1":500,"zone2":200}`); the Settings → Server enable + base URL; how to set a zone number on an ROI (Camera wizard → Areas); the best-effort/latest-value-wins semantics (no queue); and how to test against `d:\workspace\test-server` (`python server.py --port 8098`, then `curl http://localhost:8098/api/state`).

- [ ] **Step 2: ARCHITECTURE + CLAUDE — pipeline + schema**

In `docs/ARCHITECTURE.md`, add the reporter pipeline to the camera/detection section: `DetectionProcessor` → `group_into_zones` → `ZoneReporter` (mutex + `ZoneAggregator` debounce) → `post_to_gui` → `BrazingClient` (async, 5 s timeout, best-effort). Note the new `zone` column in the persistence model. In `CLAUDE.md`, add layout-table rows for `zone_assembly`, `zone_aggregator`, `zone_reporter`, `brazing_payload`, `brazing_client`, and `src/core/brazing/config`, plus a one-line note in the detection/camera section.

- [ ] **Step 3: Commit**

```
git add README.md docs/ARCHITECTURE.md CLAUDE.md
git commit -m "docs: brazing zone HTTP reporter — README + architecture + CLAUDE"
```

---

## Final verification

- [ ] Full suite green: `ctest --test-dir build --output-on-failure` — baseline (143) + new unit tests (1 camera_repo + 4 zone_assembly + 4 zone_aggregator + 1 brazing_config + 3 brazing_payload = 13), no regressions.
- [ ] Clean build, no new warnings in touched files.
- [ ] Test-server smoke recorded for Task 7 (needs the server + a live camera). UI smoke recorded for Tasks 8 & 9.
- [ ] Update the memory checkpoint with commits + branch (`feature/brazing-zone-reporter`), then delete this plan file per [[plan-cleanup-on-completion]] (keep the spec).

## Spec coverage map

| Spec item | Task |
|-----------|------|
| Zone = named ROI + `zone` number (migration v10) | 1 |
| Digits → number, left-to-right, grouped by zone | 2 |
| Debounce (stable-change) + latest-value snapshot | 3 |
| Server config (enable + base URL) persistence | 4 |
| Combined `{"zoneN":v}` payload | 5 |
| Async best-effort POST, bounded timeout | 6 |
| ZoneSink seam + reporter + grid wiring (thread-safe, off capture thread) | 7 |
| Areas UI zone number | 8 |
| Settings "Server" panel | 9 |
| README + architecture + CLAUDE docs | 10 |
| Deferred (outbox/auth/heartbeat/ReadingSink/uniqueness-enforcement) | — (spec, no task) |
