# Operating Modes — Switch-and-Reset Target Mode — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Introduce a persisted appliance "operating mode" (`digit_reader` | `ball_leveler`) and an explicit, destructive `Switch and Reset Target Mode` transaction that preserves camera connections while destroying only the mode-owned processing workspace.

**Architecture:** A new `denso::mode` domain (Qt-free, in `denso_core`) owns the `mode.target` settings key and the atomic reset transaction. The GUI (MainWindow) orchestrates confirm → teardown → reset → rebuild, reusing `CameraGrid`'s single authoritative teardown primitive. No schema migration — the mode key rides the existing `settings` key/value table; schema stays v13. The Floating Ball algorithm is explicitly **out of scope** — `ball_leveler` lands on an "unavailable in this release" state.

**Tech Stack:** C++20 / Qt6 Widgets + Sql / CMake / Catch2 v3 (unit + Qt-offscreen integration). Platform-split inference backend is untouched.

**Authoritative spec:** `docs/superpowers/specs/2026-07-21-operating-modes-switch-and-reset-design.md` (APPROVED — Revision 3a). Every decision A1–A4 and the mandatory R2 prerequisite are **locked**; this plan implements them and must not reinterpret them.

## Global Constraints

Copied verbatim from the spec and repo hard rules. Every task implicitly includes these.

- **Schema stays v13.** No migration is added by any slice. (spec §4.2, §12.1)
- **`denso_core` never links `Qt6::Widgets`, OpenCV, ORT, or TensorRT.** The mode domain + reset transaction are Qt-free (Core/Sql only). (CLAUDE.md hard rules)
- **`digit_reader` is the default and the migration target for every existing installation.** Absent/invalid/corrupt `mode.target` ⇒ `digit_reader`, never the newer mode. (spec §2, §4.1, §8)
- **The full existing `ctest` suite must pass unchanged**; a `digit_reader` appliance that never switches must behave identically. **Do not hard-code an absolute test total** (it changes as this plan adds cases and a second test target). Instead, on **each** platform: capture `ctest -N` before starting and after each slice; require **every non-intentionally-skipped test to pass**; the **only** permitted skip is the known Windows symlink case (#32), which runs and passes on the Jetson. Report the two targets — `denso_tests` (fast, backend-free) and `denso_integration_tests` (widgets/backend) — separately when useful. (spec §8, §12.1)
- **`main.cpp` stays a thin orchestrator** — no business logic. The reset transaction lives in core. (CLAUDE.md)
- **Foreign keys are inert** (`PRAGMA foreign_keys` is never enabled). Isolation is one explicit transaction, verified by test, not a schema guarantee. (spec §3.3, §1.1)
- **No new `ZoneIssue::Kind`, `GlobalBlocker::Kind`, or `ZoneCause` bit without a real producer** (the no-speculative-enum rule). A future Leveler `ZoneCause` takes the next free bit `1u << 5`, never the vacant `1u << 2`. (spec §9; `zone_health.h:17-26`)
- **status.json values are stable strings / decimal-string ids**; the file is a format. `mode`/`mode_setup_required` are OMITTED, never guessed, when the DB cannot be read. (spec §9)
- **Build/test toolchain = MSYS2 UCRT64:** `export PATH=/c/msys64/ucrt64/bin:$PATH` then `cmake -S . -B build -G Ninja && cmake --build build && ctest --test-dir build`. Run a tag subset via `./build/tests/denso_tests "[tag]"` (NOT `ctest -R`, which reports "no tests" as success). (CLAUDE.md, AGENTS.md)
- **Jetson `192.168.1.81` is RESERVED for the user's manual `.deb` testing — do NOT access, configure, operate, or reference it in any automated/remote validation.** On-device mode validation is `192.168.1.15` ONLY. (spec §13; user directive)
- **Do not modify, stage, or delete `packaging/denso-digitalreader.service`** (pre-existing untracked file).
- **Catch2 test names are CLI arguments:** ASCII only, never start with `--`, no `→`. (CLAUDE.md)

## File Structure (decomposition)

New files (all created by their owning slice):

| Path | Target | Responsibility |
|---|---|---|
| `src/core/mode/mode.h` | `denso_core` | `TargetMode` enum + `to_string`/`parse_target_mode`/`from_index` (pure). |
| `src/core/mode/config.{h,cpp}` | `denso_core` | `mode.target` load/save over the `settings` table + `mode_setup_required` predicate (`std::optional<bool>`). Modeled on `brazing/config.cpp`. |
| `src/core/mode/reset.{h,cpp}` | `denso_core` | `preview_counts()` (checked read) + `switch_and_reset()` (the atomic transaction). Takes `QSqlDatabase`, returns a typed result, pops no UI. |
| `src/app/camera/callback_generation.h` | `denso_camera` (header-only pure) | `callback_is_current(uint64_t,uint64_t)` — the Slice 0 generation-guard predicate, backend/widget-free so the unit test links no Qt Widgets/backend. |
| `src/app/ui/settings/mode_confirm_dialog.{h,cpp}` | `denso_app` | Destructive-confirmation modal with real counts (A3). |
| `src/app/ui/settings/mode_confirm_text.{h,cpp}` | `denso_core`-adjacent pure (compiled into `denso_app` **and** `denso_tests`, like `grid_layout.cpp`) | `mode_confirm_body()` pure copy builder. |
| `tests/test_mode.cpp`, `test_mode_reset.cpp`, `test_mode_confirm_text.cpp`, `test_callback_generation.cpp` | `denso_tests` (fast, backend-free) | Pure unit tests. |
| `tests/test_mode_teardown.cpp`, `test_camera_view_states.cpp`, `test_mode_switch_flow.cpp` | `denso_integration_tests` (Qt Widgets + backend graph, model-less) | Offscreen lifecycle/widget tests. |
| `tests/integration_main.cpp` | `denso_integration_tests` | Offscreen `QApplication` + Catch2 session entry point. |

Modified files touch: `camera_grid.{h,cpp}`, `camera_view.{h,cpp}`, `mainwindow.{h,cpp}`, `settings_dialog.{h,cpp}`, `camera_stream.{h,cpp}`, `status_file.{h,cpp}`, `paths.{h,cpp}`, `camera/repo.{h,cpp}`, `main.cpp`, `startup.cpp`, `run_headless.cpp` (only its status writer, if any), plus CMake lists and docs. Exact ranges are named per slice.

### Integration-test harness — LOCKED (Option A: `denso_integration_tests`), built in Slice 4 Task 4.0

`denso_tests` today links only `denso_core` + the three pure subsystem libs (`tests/CMakeLists.txt:91`) and has **no** widget/integration tests. The offscreen tests this plan needs for `CameraView`/`CameraGrid`/`MainWindow`/`CameraStream` transitively pull `EngineRegistry` (`src/app/detection/engine_registry.cpp`), which links the **inference backend** (ORT on Windows, TensorRT on Jetson). To keep `denso_tests` fast and backend-free, **Option A is locked**: a second Catch2 target `denso_integration_tests`. This is **Slice 4, Task 4.0** (the first task of Slice 4, before the teardown seam), so Slices 4/6/7 can place their widget tests in it.

**Exact CMake strategy — a `denso_app` OBJECT library, with the integration target defined in `tests/` (not `src/app/`).** Two hard facts drive this (both verified against the tree): the top-level order is `add_subdirectory(src/core)` → `src/app` → `tests` (`CMakeLists.txt:50-52`), and **`Catch2::Catch2` is created only inside `tests/CMakeLists.txt`** (its `FetchContent`). So the test target **must** be declared in `tests/`, after `FetchContent_MakeAvailable(Catch2)`; declaring it in `src/app` cannot see Catch2. `CMAKE_AUTOMOC`/`AUTORCC` are ON globally (`CMakeLists.txt:11-12`). Refactor `src/app/CMakeLists.txt`:
1. `add_library(denso_app OBJECT …)` holding **every source currently in `add_executable(denso …)` EXCEPT `main.cpp` and EXCEPT `resources.qrc`** — including `cli/run_headless.cpp`, `cli/migrate_coordinator.cpp`, the logging/shell/settings/camera/detection/brazing sources, `ui/camera/grid/camera_grid.cpp`, `camera/camera_stream.cpp`, `detection/engine_registry.cpp`, `detection/model_sync.cpp`, and — inside the existing `if(WIN32)/else()` — the platform backend (`ort_engine.cpp` / `trt_engine.cpp`). **An OBJECT library, NOT STATIC:** object libraries link **all** their objects into every consumer, so no moc/static-initializer object can be silently culled by static-archive extraction (the STATIC-lib hazard Codex flagged). `target_link_libraries(denso_app PUBLIC denso_core denso_detection denso_brazing denso_camera Qt6::Widgets Qt6::Multimedia Qt6::Network ${OpenCV_LIBS} <backend>)` (`<backend>` = `onnxruntime` on WIN32, `${TRT_NVINFER} CUDA::cudart` else); `target_compile_definitions(denso_app PUBLIC APP_VERSION="0.1.0")`; `target_include_directories(denso_app PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${OpenCV_INCLUDE_DIRS} [${TRT_INCLUDE_DIR}])`.
2. `add_executable(denso WIN32 main.cpp resources.qrc)` + `target_link_libraries(denso PRIVATE denso_app)`. **`resources.qrc` stays DIRECTLY on the `denso` executable** (not in the library) so `:/icon.png` and the theme resources are guaranteed compiled + auto-registered into the shipped exe — a resource object buried in a library can fail to auto-initialise. **All existing POST_BUILD steps** (ORT DLL copy, model copy, GPU-EP copy — `src/app/CMakeLists.txt:140-165`) **stay on the `denso` target**.
3. In **`tests/CMakeLists.txt`, AFTER `FetchContent_MakeAvailable(Catch2)`**: `add_executable(denso_integration_tests tests/integration_main.cpp <integration test sources>)` + `target_link_libraries(denso_integration_tests PRIVATE denso_app Catch2::Catch2)`; `catch_discover_tests(denso_integration_tests)` so it joins the normal `ctest` run. The integration target does **not** use `:/icon.png`, so it needs no `resources.qrc`.
   - **Windows only:** a POST_BUILD copies the ORT runtime DLLs (`onnxruntime.dll` + the 3 provider DLLs from `${ORT_DIR}/lib/...`) beside the test exe — the import lib means the exe needs them at load — but **NOT** `models/` (the tests are model-less, so nothing scans/loads an engine). On Jetson TRT is a system lib; no copy needed.
   - `tests/integration_main.cpp`: `qputenv("QT_QPA_PLATFORM","offscreen");` then construct `QApplication` and run `Catch2::Session().run(argc, argv)`. One `QApplication` for the whole target.
4. **No "byte-identical" claim.** Regrouping objects into an OBJECT library changes how objects are grouped, not how they are compiled; sources, defines, link libraries and POST_BUILD steps are unchanged, so app **behaviour** is preserved — but the emitted binary may differ and the plan does **not** assert binary identity. (Ship-artifact byte-reproducibility is a property of the `.deb` packaging tree, untouched by this dev-target refactor.) Task 4.0's acceptance is: `denso` still builds, links, **shows its window icon**, and runs; the integration target links and `ctest` discovers it.

**Why it stays GPU-free:** every seeded camera in these tests is **model-less** (active + `setup_complete=1`, **no** attached `camera_model` rows), so `CameraGrid::start_one` selects `OrientationProcessor` and never calls `engines_->get()` (`camera_grid.cpp:226-273`) — no engine is ever deserialized or built, no CUDA/TRT/ORT inference runs, and it works headless offscreen. `EngineRegistry` is *constructed* (empty required-set) but never asked for an engine.

**Source ownership of tests:**
- `denso_tests` (fast, backend-free — UNCHANGED link line): all pure unit tests — `test_mode.cpp`, `test_mode_reset.cpp`, `test_mode_confirm_text.cpp` (pure copy builder), `test_callback_generation.cpp` (pure helper), `test_paths.cpp`, `test_status_file.cpp`.
- `denso_integration_tests` (widgets + backend graph): `test_mode_teardown.cpp`, `test_camera_view_states.cpp`, `test_mode_switch_flow.cpp`.

**No silent fallback:** if the harness cannot be built in the time box, that is a blocking issue to raise — not a reason to quietly drop the widget-level proofs. The core invariants (Slices 0–3) remain fully covered in `denso_tests` regardless.

---

## Slice 0 — Stale-generation callback isolation (MANDATORY PREREQUISITE)

**This slice MUST land as its own commit, be reviewed, and be merged before Slice 1 begins.** It changes **no** operating-mode behaviour and is separately reviewable. (spec §5, §10-R2, §11-R8)

**Goal:** Guarantee that a failure/status callback queued by an *old* inference-worker generation cannot inhibit a camera in the *new* generation after a `CameraGrid` rebuild — even when the same retained camera id still exists.

**Dependencies:** none (must precede all mode work).

**Files:**
- Create: `src/app/camera/callback_generation.h` — a pure, header-only, backend/widget-free predicate in the `denso_camera` include root.
- Modify: `src/app/ui/camera/grid/camera_grid.h` — add a generation counter member; `#include "camera/callback_generation.h"`.
- Modify: `src/app/ui/camera/grid/camera_grid.cpp:97-207` (`reload`/`clear`), `:257-263` (`WorkerFailedFn`), `:285-291` (`status_changed` cause lambda).
- Create: `tests/test_callback_generation.cpp` (includes ONLY the pure header — no `camera_grid.h`).
- Modify: `tests/CMakeLists.txt` (register the new test source in `denso_tests`).

**APIs and ownership:**
- `src/app/camera/callback_generation.h` (pure — `denso_camera` already links to `denso_tests`, so the unit test needs no Qt Widgets/OpenCV/backend):
```cpp
#pragma once
#include <cstdint>
namespace denso::camera {
// A queued worker callback belongs to grid generation `captured`; it must be
// dropped once the grid has rebuilt (generation advanced). Pure + unit-tested.
inline bool callback_is_current(uint64_t captured, uint64_t live) {
    return captured == live;
}
}
```
- Add private member `uint64_t generation_ = 0;` to `CameraGrid`; `#include "camera/callback_generation.h"` in `camera_grid.h`. Also expose a test-only `uint64_t generation() const { return generation_; }` accessor (consumed by Slice 8's integration proof that teardown advances the generation).
- `clear()` (`camera_grid.cpp:57`) increments `generation_` at the very top (before any stop/delete), so every teardown invalidates the epoch the just-torn-down workers captured.
- `WorkerFailedFn` and the `status_changed` cause lambda capture `gen = generation_` by value at construction time in `start_one` and, inside the GUI-thread body, drop the event when `!camera::callback_is_current(gen, generation_)`. This is the "generation token" arm of the spec's choice; it composes with the existing `if (!health_) return;` guard.
- Rationale for the token over processor-owned context: the `WorkerFailedFn` is deliberately marshalled with the **grid** as Qt context (`post_to_gui(this, …)`) because it must reach `health_`, which the processor does not own; a bare context swap would regress that. The snapshot path already uses processor-owned context (`post_to_gui(reporter, …)`, `:133-138`) and stays as-is — this slice does not touch it.

**Explicitly unchanged behavior:** the healthy inhibit/recovery path, the snapshot/`ZoneReporter` marshaling, tile rendering, warm-up. A same-generation worker failure still inhibits exactly as today.

- [ ] **Step 1: Write the failing test** (`tests/test_callback_generation.cpp`) — includes ONLY the pure header, so it links no Qt Widgets/backend:
```cpp
#include <catch2/catch_test_macros.hpp>
#include "camera/callback_generation.h"

TEST_CASE("a callback from the same generation is delivered", "[callback_generation]") {
    CHECK(denso::camera::callback_is_current(7, 7));
}
TEST_CASE("a callback from an older generation is dropped", "[callback_generation]") {
    // Old worker captured gen 3; grid has since rebuilt to gen 4.
    CHECK_FALSE(denso::camera::callback_is_current(3, 4));
}
```

- [ ] **Step 2: Run the test — expect a compile failure** (`camera/callback_generation.h` missing)

Run: `./build/tests/denso_tests "[callback_generation]"`
Expected: FAIL until the header is added.

- [ ] **Step 3: Add the pure header + generation member; wire clear() and the two lambdas**

- Create `src/app/camera/callback_generation.h` (content above).
- In `camera_grid.h`: `#include "camera/callback_generation.h"`; add `uint64_t generation_ = 0;` beside `last_applied_seq_`.
- In `clear()` first line (before `pending_ = PendingStart{};`): `++generation_;`
- In `start_one`, change the `WorkerFailedFn` (`:257-263`) to capture the epoch and gate on it:
```cpp
/*WorkerFailedFn*/ [this, id = cam.id, gen = generation_](int64_t, bool failed) {
    common::post_to_gui(this, [this, id, failed, gen] {
        if (!health_ || !camera::callback_is_current(gen, generation_)) return;
        health_->set_cause(id, health::ZoneCause::InferenceWorkerFailed, failed);
    });
},
```
- In the `status_changed` cause connection (`:285-291`), capture and gate the same way:
```cpp
connect(stream, &CameraStream::status_changed, this,
        [this, id = cam.id, gen = generation_](int s) {
            if (!health_ || !camera::callback_is_current(gen, generation_)) return;
            health_->set_cause(id, health::ZoneCause::CaptureOffline,
                               s == static_cast<int>(CameraStream::Status::Offline));
        });
```

- [ ] **Step 4: Run the test — expect PASS**

Run: `./build/tests/denso_tests "[callback_generation]"`
Expected: PASS.

- [ ] **Step 5: Confirm the `status_changed` lifetime claim in the plan record**

The spec (§5) requires *confirming*, not assuming, that the queued `status_changed` path is safe. The generation gate now makes it safe by construction (an old-generation `status_changed` is dropped even if Qt happened to deliver it). Record in the commit body: "status_changed is now generation-guarded, not merely relying on Qt dropping events for a deleted stream." No behavior change for the live generation.

- [ ] **Step 6: Full build + suite**

Run: `cmake --build build && ctest --test-dir build`
Expected: record the `ctest -N` TOTAL before and after (it rises by exactly the two new `[callback_generation]` cases); every non-intentionally-skipped test passes; only the known Windows symlink SKIP (#32) remains, rendered Failed by ctest on Windows and passing on the Jetson. Do not hard-code an absolute total — compare the delta.

- [ ] **Step 7: Commit (Slice 0 — its own commit, to be reviewed + merged before Slice 1)**

```bash
git add src/app/camera/callback_generation.h \
        src/app/ui/camera/grid/camera_grid.h src/app/ui/camera/grid/camera_grid.cpp \
        tests/test_callback_generation.cpp tests/CMakeLists.txt
git commit -m "fix(camera): isolate worker callbacks by grid generation"
```

**Success criteria:** an old-generation `WorkerFailedFn`/`status_changed` delivered after a rebuild does not inhibit a camera in the new generation (unit-proven here; integration-proven in Slice 8). No operating-mode code exists yet.

**Failure/rollback behavior:** N/A (pure additive guard). If the suite regresses, revert the commit; nothing downstream depends on it yet.

**Proposed commit boundary:** exactly one commit; **merge gate before Slice 1.**

---

## Slice 1 — Mode domain and persistence

**Goal:** A `TargetMode` type that round-trips through the `settings` table under key `mode.target`, defaulting to `digit_reader` on absent/invalid/corrupt input, with no schema change.

**Dependencies:** Slice 0 merged.

**Files:**
- Create: `src/core/mode/mode.h`, `src/core/mode/config.h`, `src/core/mode/config.cpp`
- Create: `tests/test_mode.cpp`
- Modify: `src/core/CMakeLists.txt:9-42` (add `mode/config.cpp` to `denso_core`), `tests/CMakeLists.txt` (add `test_mode.cpp`).

**Interfaces:**
- Produces (`mode.h`):
```cpp
namespace denso::mode {
enum class TargetMode { DigitReader, BallLeveler };
const char* to_string(TargetMode m);          // "digit_reader" | "ball_leveler"
TargetMode parse_target_mode(const std::string& s);  // unknown/empty/corrupt → DigitReader
}
```
- Produces (`config.h`):
```cpp
namespace denso::mode {
TargetMode load(const QSqlDatabase& db);       // absent/corrupt key → DigitReader
bool save(const QSqlDatabase& db, TargetMode m);  // checked upsert; returns success
}
```
- `parse_target_mode` follows the `parse_display_mode` contract (`settings/display.h:18`): any unknown token resolves to `DigitReader`, **never** `BallLeveler`.
- `save` returns a checked bool (unlike `brazing::save`, which the spec §6.3 criticizes for being unchecked). Note: the switch-and-reset transaction (Slice 3) writes `mode.target` **directly inside its own transaction**, NOT via `save`, so `save` is used only by non-transactional callers/tests.

**Explicitly NOT done:** `mode.target` is **not** a field on `settings::Settings`. `MainWindow::on_reset_defaults` (`mainwindow.cpp:299-318`) assigns a default-constructed `Settings` and calls `settings::save`; a field there would be wiped by "Reset to defaults". An independent key is untouched by that path (`settings::save` does not delete unknown keys — `settings/repo.cpp:76-102`). Verified in the Slice-1 test.

- [ ] **Step 1: Write the failing tests** (`tests/test_mode.cpp`)

```cpp
#include <catch2/catch_test_macros.hpp>
#include "mode/mode.h"
#include "mode/config.h"
#include "settings/repo.h"
#include "settings/settings.h"
#include "db/db.h"

using denso::mode::TargetMode;

TEST_CASE("target mode round-trips through its token", "[mode]") {
    CHECK(std::string(denso::mode::to_string(TargetMode::DigitReader)) == "digit_reader");
    CHECK(std::string(denso::mode::to_string(TargetMode::BallLeveler)) == "ball_leveler");
    CHECK(denso::mode::parse_target_mode("digit_reader") == TargetMode::DigitReader);
    CHECK(denso::mode::parse_target_mode("ball_leveler") == TargetMode::BallLeveler);
}

TEST_CASE("unknown, empty, or corrupt tokens resolve to digit_reader", "[mode]") {
    CHECK(denso::mode::parse_target_mode("") == TargetMode::DigitReader);
    CHECK(denso::mode::parse_target_mode("leveler") == TargetMode::DigitReader);
    CHECK(denso::mode::parse_target_mode("DIGIT_READER") == TargetMode::DigitReader);
    CHECK(denso::mode::parse_target_mode("\x01garbage") == TargetMode::DigitReader);
}

TEST_CASE("mode config load/save over the settings table", "[mode]") {
    auto db = denso::db::Db::open_in_memory();
    REQUIRE(db);
    REQUIRE(denso::db::run_migrations(db->handle()));

    CHECK(denso::mode::load(db->handle()) == TargetMode::DigitReader);  // absent → default
    REQUIRE(denso::mode::save(db->handle(), TargetMode::BallLeveler));
    CHECK(denso::mode::load(db->handle()) == TargetMode::BallLeveler);
}

TEST_CASE("a corrupt stored token loads as digit_reader", "[mode]") {
    auto db = denso::db::Db::open_in_memory();
    REQUIRE(db);
    REQUIRE(denso::db::run_migrations(db->handle()));
    QSqlQuery q(db->handle());
    q.prepare(QStringLiteral("INSERT INTO settings (key, value) VALUES ('mode.target', ?)"));
    q.addBindValue(QStringLiteral("floating_ball_v2"));
    REQUIRE(q.exec());
    CHECK(denso::mode::load(db->handle()) == TargetMode::DigitReader);
}

TEST_CASE("Reset to defaults does not disturb mode.target", "[mode]") {
    auto db = denso::db::Db::open_in_memory();
    REQUIRE(db);
    REQUIRE(denso::db::run_migrations(db->handle()));
    REQUIRE(denso::mode::save(db->handle(), TargetMode::BallLeveler));
    // Simulate MainWindow::on_reset_defaults: save a default-constructed Settings.
    denso::settings::save(db->handle(), denso::settings::Settings{});
    CHECK(denso::mode::load(db->handle()) == TargetMode::BallLeveler);
}
```
(Include `<QSqlQuery>` in the test.)

- [ ] **Step 2: Run — expect compile failure** (`mode/mode.h` missing)

Run: `./build/tests/denso_tests "[mode]"`
Expected: FAIL to compile.

- [ ] **Step 3: Implement `mode.h`**

```cpp
#pragma once
#include <string>
namespace denso::mode {
enum class TargetMode { DigitReader, BallLeveler };
inline const char* to_string(TargetMode m) {
    switch (m) {
        case TargetMode::DigitReader: return "digit_reader";
        case TargetMode::BallLeveler: return "ball_leveler";
    }
    return "digit_reader";
}
inline TargetMode parse_target_mode(const std::string& s) {
    if (s == "ball_leveler") return TargetMode::BallLeveler;
    return TargetMode::DigitReader;   // absent/unknown/corrupt → default (never the newer mode)
}
// Validate a UI selector index / any raw int to a real enumerator. An
// out-of-range value is NEVER stored as an invalid enum (which would make
// to_string fall back while current_mode_ held garbage — a DB/UI mismatch).
inline TargetMode from_index(int i) {
    return i == static_cast<int>(TargetMode::BallLeveler) ? TargetMode::BallLeveler
                                                          : TargetMode::DigitReader;
}
}
```
Add a unit case in `tests/test_mode.cpp`: `CHECK(from_index(0)==DigitReader); CHECK(from_index(1)==BallLeveler); CHECK(from_index(99)==DigitReader);`

- [ ] **Step 4: Implement `config.{h,cpp}`** (mirror `brazing/config.cpp`, but checked `save`)

`config.h`:
```cpp
#pragma once
#include "mode/mode.h"
#include <QSqlDatabase>
namespace denso::mode {
TargetMode load(const QSqlDatabase& db);
bool save(const QSqlDatabase& db, TargetMode m);
}
```
`config.cpp`:
```cpp
#include "mode/config.h"
#include <QSqlQuery>
#include <QString>
#include <QVariant>
#include <optional>
namespace denso::mode {
namespace {
std::optional<QString> get(const QSqlDatabase& db, const QString& key) {
    QSqlQuery q(db);
    q.prepare(QStringLiteral("SELECT value FROM settings WHERE key = ?"));
    q.addBindValue(key);
    if (q.exec() && q.next()) return q.value(0).toString();
    return std::nullopt;
}
}
TargetMode load(const QSqlDatabase& db) {
    if (const auto v = get(db, QStringLiteral("mode.target")))
        return parse_target_mode(v->toStdString());
    return TargetMode::DigitReader;
}
bool save(const QSqlDatabase& db, TargetMode m) {
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "INSERT INTO settings (key, value) VALUES ('mode.target', ?) "
        "ON CONFLICT(key) DO UPDATE SET value = excluded.value"));
    q.addBindValue(QString::fromLatin1(to_string(m)));
    return q.exec();
}
}
```

- [ ] **Step 5: Wire CMake** — add `mode/config.cpp` to `src/core/CMakeLists.txt` `add_library(denso_core …)` list; add `test_mode.cpp` to `tests/CMakeLists.txt`.

- [ ] **Step 6: Run — expect PASS**

Run: `cmake -S . -B build -G Ninja && cmake --build build && ./build/tests/denso_tests "[mode]"`
Expected: PASS (all 5 cases).

- [ ] **Step 7: Commit**

```bash
git add src/core/mode/mode.h src/core/mode/config.h src/core/mode/config.cpp \
        src/core/CMakeLists.txt tests/test_mode.cpp tests/CMakeLists.txt
git commit -m "feat(mode): add TargetMode domain + mode.target persistence"
```

**Success criteria:** spec acceptance §12.2 (absent/corrupt ⇒ digit_reader) and §12.3 (Reset-to-defaults leaves mode) both proven. Schema unchanged (no migration added; `run_migrations` still stamps v13).

**Failure/rollback behavior:** pure additive; nothing consumes it yet. Revert if the suite regresses.

**Proposed commit boundary:** one commit.

---

## Slice 2 — Status path consolidation + mode fields

**Goal:** Route all three `status.json` writers through a single `paths::status_file()`, and emit optional `mode` / `mode_setup_required` keys — omitted when the DB cannot be read; `mode_setup_required` distinguishing "no completed camera" from "configured but inactive".

**Dependencies:** Slice 1 merged.

**Files:**
- Modify: `src/core/paths/paths.h:27` (declare `status_file()`), `src/core/paths/paths.cpp` (implement).
- Modify: `src/core/health/status_file.h:15-19` + `src/core/health/status_file.cpp:23-76` (two optional params; emit keys only when set).
- Modify: `src/core/mode/config.h` + `config.cpp` (add `mode_setup_required` predicate).
- Modify: `src/app/main.cpp:197-198` (compose via `paths::status_file()`; DB-blocker site omits mode).
- Modify: `src/app/ui/startup.cpp:134-135` and `src/app/ui/camera/grid/camera_grid.cpp:329-331` (compose via `paths::status_file()`; pass real mode fields).
- Modify: `tests/test_status_file.cpp` (assert presence/omission), `tests/test_mode.cpp` (predicate cases), `tests/test_paths.cpp` (new path).

**Interfaces:**
- Produces (`paths.h`):
```cpp
QString status_file();   ///< <data>/status.json
```
- Produces (`mode/config.h`):
```cpp
std::optional<bool> mode_setup_required(const QSqlDatabase& db, TargetMode mode);
// ball_leveler → ALWAYS true (spec §2.1: "reports mode_setup_required: true permanently" —
//   this release ships no Leveler setup, so it is never "configured");
// digit_reader → true  if NO camera row has setup_complete = 1 (setup required);
//                false if at least one completed camera, even if inactive (NOT runtime()-based, §9);
//                nullopt if the query failed → status.json OMITS the field.
```
Making the predicate mode-aware puts the §2.1 "permanent true" rule in ONE place, so every writer (startup, grid live, grid idle) is consistent and no caller can leak a `false` for `ball_leveler`.
- Changes (`status_file.h`) — **default args keep every existing 5-arg caller and test compiling unchanged**:
```cpp
bool write_status_file(const QString& path,
                       const health::IntegrityVerdict& verdict,
                       const std::map<int64_t, uint32_t>& camera_causes,
                       const std::set<int>& held_zones,
                       const std::set<int>& inhibited_zones,
                       const std::optional<QString>& mode = std::nullopt,
                       std::optional<bool> mode_setup_required = std::nullopt);
```
Passing the **serialized token** (a `QString`) rather than a `TargetMode` keeps `denso_core/health` free of a dependency on `mode/`.

**Single-owner status-composition principle (locked; see Slices 6–7).** `status.json` has exactly three lifecycle-distinct writers, and **the integrity verdict passed is ALWAYS a real one** — a DB-stage blocker verdict, or `health::evaluate_integrity(...)`, or the live grid's cached `verdict_`. **No caller ever passes a default-constructed `IntegrityVerdict{}`** as a stand-in — that would silently overwrite real blockers/issues/causes with "ready/empty". During runtime the **sole** writer is `CameraGrid` (both its live `refresh_status_file()` and, for an intentionally idle mode, `publish_idle_status()` added in Slice 6); the orchestrator never writes `status.json` directly.

**Ownership of the mode fields per writer:**
- `main.cpp` DB-blocker (`report_db_blocker`, `:101-111`): keeps the **5-arg** call → both keys omitted. This is exactly the "DB unopenable/schema-newer/migration-failed" case where the mode is undeterminable. (spec §9, §12.13)
- `startup.cpp launch()` Blocked-integrity (`:132-142`): DB is already open+migrated → `const auto m = mode::load(db);` pass `QString::fromLatin1(mode::to_string(m))` and `mode::mode_setup_required(db, m)` (an `optional<bool>` — omitted if the digit_reader query fails; always `true` for ball_leveler). Verdict is the real Blocked integrity verdict.
- `camera_grid.cpp refresh_status_file()` (`:324-332`): DB open → `const auto m = mode::load(db_);` pass `mode::to_string(m)` + `mode::mode_setup_required(db_, m)`; verdict is the grid's cached real `verdict_`.

- [ ] **Step 1: Write failing tests**

`tests/test_paths.cpp` — add:
```cpp
TEST_CASE("status_file lives in the data dir", "[paths]") {
    CHECK(denso::paths::status_file().endsWith(QStringLiteral("status.json")));
    CHECK(denso::paths::status_file() ==
          QDir(denso::paths::data_dir()).filePath(QStringLiteral("status.json")));
}
```
`tests/test_mode.cpp` — add:
```cpp
using denso::mode::TargetMode;
TEST_CASE("digit_reader mode_setup_required is true with zero completed cameras", "[mode]") {
    auto db = denso::db::Db::open_in_memory();
    REQUIRE(db); REQUIRE(denso::db::run_migrations(db->handle()));
    CHECK(denso::mode::mode_setup_required(db->handle(), TargetMode::DigitReader)
          == std::optional<bool>(true));
}
TEST_CASE("digit_reader mode_setup_required is false when a completed but inactive camera exists", "[mode]") {
    auto db = denso::db::Db::open_in_memory();
    REQUIRE(db); REQUIRE(denso::db::run_migrations(db->handle()));
    denso::camera::Camera c; c.name = "cam"; c.camera_type = "usb"; c.index = 0u;
    c.active = false; c.setup_complete = true;          // configured, disabled
    REQUIRE(denso::camera::insert(db->handle(), c));
    CHECK(denso::mode::mode_setup_required(db->handle(), TargetMode::DigitReader)
          == std::optional<bool>(false));               // NOT runtime()-based
}
TEST_CASE("ball_leveler mode_setup_required is ALWAYS true, even with a completed camera", "[mode]") {
    auto db = denso::db::Db::open_in_memory();
    REQUIRE(db); REQUIRE(denso::db::run_migrations(db->handle()));
    denso::camera::Camera c; c.name = "cam"; c.camera_type = "usb"; c.index = 0u;
    c.active = true; c.setup_complete = true;            // a completed camera exists
    REQUIRE(denso::camera::insert(db->handle(), c));
    CHECK(denso::mode::mode_setup_required(db->handle(), TargetMode::BallLeveler)
          == std::optional<bool>(true));                // spec §2.1 — permanent
}
TEST_CASE("digit_reader mode_setup_required is nullopt when the query cannot run", "[mode]") {
    auto db = denso::db::Db::open_in_memory();
    REQUIRE(db); REQUIRE(denso::db::run_migrations(db->handle()));
    QSqlQuery(db->handle()).exec(QStringLiteral("DROP TABLE camera"));  // force a query failure
    CHECK_FALSE(denso::mode::mode_setup_required(db->handle(), TargetMode::DigitReader).has_value());
}
```
`tests/test_status_file.cpp` — add:
```cpp
TEST_CASE("status.json emits mode fields when provided", "[status_file]") {
    const QString p = QDir(QDir::tempPath()).filePath(QStringLiteral("st_mode.json"));
    denso::health::IntegrityVerdict v;  // Ready
    REQUIRE(denso::health::write_status_file(p, v, {}, {}, {},
            QStringLiteral("ball_leveler"), true));
    QFile f(p); REQUIRE(f.open(QIODevice::ReadOnly));
    const auto o = QJsonDocument::fromJson(f.readAll()).object();
    CHECK(o.value("mode").toString() == QStringLiteral("ball_leveler"));
    CHECK(o.value("mode_setup_required").toBool() == true);
}
TEST_CASE("status.json omits mode fields when not provided", "[status_file]") {
    const QString p = QDir(QDir::tempPath()).filePath(QStringLiteral("st_nomode.json"));
    denso::health::IntegrityVerdict v;
    REQUIRE(denso::health::write_status_file(p, v, {}, {}, {}));  // 5-arg
    QFile f(p); REQUIRE(f.open(QIODevice::ReadOnly));
    const auto o = QJsonDocument::fromJson(f.readAll()).object();
    CHECK_FALSE(o.contains("mode"));
    CHECK_FALSE(o.contains("mode_setup_required"));
}
```

- [ ] **Step 2: Run — expect FAIL** (`status_file()`/`mode_setup_required`/new params missing)

Run: `./build/tests/denso_tests "[paths][mode][status_file]"`
Expected: FAIL to compile.

- [ ] **Step 3: Implement `paths::status_file()`** in `paths.h`/`paths.cpp` — return `QDir(data_dir()).filePath(QStringLiteral("status.json"))`, mirroring `db_file()`.

- [ ] **Step 4: Implement `mode::mode_setup_required`** in `config.cpp` (add `#include <optional>` to `config.h`; it already includes `mode/mode.h` for `TargetMode`):
```cpp
std::optional<bool> mode_setup_required(const QSqlDatabase& db, TargetMode mode) {
    if (mode == TargetMode::BallLeveler) return true;   // §2.1: permanently true this release
    QSqlQuery q(db);
    // digit_reader: independent of `active`; a query failure is UNDETERMINABLE, never guessed.
    if (!q.exec(QStringLiteral("SELECT 1 FROM camera WHERE setup_complete = 1 LIMIT 1")))
        return std::nullopt;                 // cannot determine → status.json omits it
    return !q.next();                        // true = none completed; false = ≥1 completed
}
```
Declare it in `config.h`.

- [ ] **Step 5: Extend `write_status_file`** — add the two optional params to the signature + emit only when set:
```cpp
    if (mode) root["mode"] = *mode;
    if (mode_setup_required) root["mode_setup_required"] = *mode_setup_required;
```
Add `#include <optional>` to `status_file.h`.

- [ ] **Step 6: Route the three writers through `paths::status_file()`**

- `main.cpp`: replace the local `status_path` compose (`:197-198`) with `denso::paths::status_file()`. Leave `report_db_blocker`'s `write_status_file` **5-arg** (omit mode).
- `startup.cpp launch()` (`:134-136`): use `denso::paths::status_file()`; `const auto m = denso::mode::load(db);` pass `QString::fromLatin1(denso::mode::to_string(m))` + `denso::mode::mode_setup_required(db, m)`. Add `#include "mode/config.h"`.
- `camera_grid.cpp refresh_status_file()` (`:329-331`): use `denso::paths::status_file()`; `const auto m = denso::mode::load(db_);` pass `denso::mode::to_string(m)` + `denso::mode::mode_setup_required(db_, m)` (db_ is the member handle). Add `#include "mode/config.h"`.

- [ ] **Step 7: Run — expect PASS**

Run: `cmake --build build && ./build/tests/denso_tests "[paths][mode][status_file]"`
Expected: PASS. Existing `[status_file]` cases still pass (default-arg compat).

- [ ] **Step 8: Commit**

```bash
git add src/core/paths/paths.h src/core/paths/paths.cpp \
        src/core/health/status_file.h src/core/health/status_file.cpp \
        src/core/mode/config.h src/core/mode/config.cpp \
        src/app/main.cpp src/app/ui/startup.cpp src/app/ui/camera/grid/camera_grid.cpp \
        tests/test_paths.cpp tests/test_mode.cpp tests/test_status_file.cpp
git commit -m "feat(status): route writers through paths::status_file() and add mode fields"
```

**Success criteria:** spec §9 — single path helper, three writers consolidated, `mode`/`mode_setup_required` present when the DB is readable and omitted on a DB-stage blocker (§12.13); predicate distinguishes inactive-configured from setup-required.

**Failure/rollback behavior:** additive; the 5-arg default keeps old behavior. A regression is reverted as one commit.

**Explicitly unchanged behavior:** the status.json schema keys `status`/`blockers`/`issues`/`camera_causes`/`held_zones`/`inhibited_zones`; atomic `QSaveFile` write; decimal-string ids.

**Proposed commit boundary:** one commit.

---

## Slice 3 — Atomic reset transaction (core)

**Goal:** `mode::switch_and_reset(db, new_mode)` — one checked transaction that destroys the mode-owned workspace, preserves every camera row + id + connection/capture columns, resets only `setup_complete`/`areas_need_review`, writes `mode.target`, disables `brazing.enabled`, preserves `brazing.base_url`, and rolls back atomically on any failure. Plus `mode::preview_counts(db)` for the confirmation dialog.

**Dependencies:** Slice 1 merged (needs `TargetMode`).

**Files:**
- Create: `src/core/mode/reset.h`, `src/core/mode/reset.cpp`
- Create: `tests/test_mode_reset.cpp`
- Modify: `src/core/CMakeLists.txt` (add `mode/reset.cpp`), `tests/CMakeLists.txt` (add `test_mode_reset.cpp`).

**Interfaces:**
- Produces (`reset.h`):
```cpp
#pragma once
#include "mode/mode.h"
#include <QSqlDatabase>
#include <optional>
#include <string>
#include <vector>
namespace denso::mode {

struct SwitchCounts {         // real counts for the A3 confirmation (spec §7.1)
    int cameras = 0;          // camera rows kept
    int model_bindings = 0;   // camera_model rows
    int areas = 0;            // camera_area rows
    std::vector<int> zones;   // distinct non-null, non-zero zone numbers (ascending)
    int readings = 0;         // reading rows
    int receipts = 0;         // model_migration_receipt rows
};
// Returns nullopt if ANY count/zone query fails. A3 requires REAL counts before
// destruction — a query error must abort the confirmation, never render as "0",
// which would let an operator authorise deleting data they were told was empty.
std::optional<SwitchCounts> preview_counts(const QSqlDatabase& db);

struct ResetResult {
    bool ok = false;
    std::string error;        // SQL error verbatim on failure; empty on success
};
// Destroys the mode-owned workspace and switches mode in ONE transaction.
// Pops no UI. Cameras are UPDATEd, never deleted. (spec §6.3)
ResetResult switch_and_reset(const QSqlDatabase& db, TargetMode new_mode);
}
```

**Ownership & transaction body (spec §6.3, verbatim order):**
```
BEGIN
  DELETE FROM camera_model_class          -- UNCONDITIONAL (repairs pre-existing orphans)
  DELETE FROM camera_model
  DELETE FROM camera_area
  DELETE FROM reading
  DELETE FROM model_migration_receipt
  UPDATE camera SET setup_complete = 0, areas_need_review = 0     -- ALL rows
  upsert settings 'mode.target'     = <to_string(new_mode)>
  upsert settings 'brazing.enabled' = '0'
COMMIT   -- else ROLLBACK
```
- `camera_model_class` is deleted **unconditionally**, NOT scoped by `WHERE camera_model_id IN (...)`: `camera::remove` orphans such rows (`repo.cpp:115-149`), so a scoped delete would leave rows the acceptance criteria assert are gone (spec §6.3, §12.16).
- The two settings upserts are issued **on this connection inside this transaction** — NOT via `settings::save`/`brazing::save` (which run their own transaction / are unchecked). `brazing.enabled` is the string `"0"` (matching `brazing/config.cpp:34-49`).
- **Every** statement result AND the commit are checked; any failure → explicit `conn.rollback()` and `error` carries `query.lastError().text().toStdString()`. On a failed commit, call `conn.rollback()` explicitly (SQLite can leave the tx open on a busy commit — same shape as `camera/repo.cpp:142-147`).
- `switch_and_reset` **does not** call `camera::remove` and must not delete any `camera` row.
- **Concurrency assumption (documented in code):** runs from a modal GUI handler with no other writer active, so `preview_counts` taken just before cannot go stale before commit.

**Explicitly unchanged behavior:** `camera` connection/capture columns; `brazing.base_url`; `settings` display keys; `net_config`; the `model` catalog; on-disk engines/sidecars/cache/logs/DB.

- [ ] **Step 1: Write failing tests** (`tests/test_mode_reset.cpp`) — seed a realistic DB, then assert preservation, orphan repair, runtime-empty, reporting-disable, and rollback.

Helper seed (top of file):
```cpp
#include <catch2/catch_test_macros.hpp>
#include "mode/reset.h"
#include "mode/config.h"
#include "camera/repo.h"
#include "brazing/config.h"
#include "db/db.h"
#include <QSqlQuery>
#include <QVariant>

namespace {
int64_t seed_camera(const QSqlDatabase& db, bool active, bool setup) {   // IP camera
    denso::camera::Camera c;
    c.name = "cam"; c.camera_type = "ip"; c.ip = "10.0.0.9";
    c.rtsp = "rtsp://10.0.0.9/s1"; c.username = "u"; c.password = "p";
    c.channel = 1u; c.stream = 0u; c.manufacturer = "Dahua";
    c.width = 1920; c.height = 1080; c.fps = 25;
    c.pitch = 1.5f; c.roll = -2.0f; c.rotation = 90;
    c.active = active; c.setup_complete = setup; c.areas_need_review = true;
    auto id = denso::camera::insert(db, c);
    REQUIRE(id);
    return *id;
}
int64_t seed_camera_usb(const QSqlDatabase& db, bool active, bool setup) {  // USB camera (non-null cam_index)
    denso::camera::Camera c;
    c.name = "usbcam"; c.camera_type = "usb"; c.index = 2u;   // cam_index — the USB-only field
    c.width = 1280; c.height = 720; c.fps = 30;
    c.pitch = 0.0f; c.roll = 0.0f; c.rotation = 180;
    c.active = active; c.setup_complete = setup; c.areas_need_review = false;
    auto id = denso::camera::insert(db, c);
    REQUIRE(id);
    return *id;
}
void exec(const QSqlDatabase& db, const QString& sql) {
    QSqlQuery q(db); REQUIRE(q.exec(sql));
}
int count(const QSqlDatabase& db, const QString& table) {
    QSqlQuery q(db); REQUIRE(q.exec("SELECT COUNT(*) FROM " + table)); REQUIRE(q.next());
    return q.value(0).toInt();
}
}
```
Cases:
```cpp
TEST_CASE("reset preserves every camera row, id, and all 18 connection/capture columns", "[mode_reset]") {
    auto db = denso::db::Db::open_in_memory(); REQUIRE(db);
    REQUIRE(denso::db::run_migrations(db->handle()));
    // Seed BOTH an IP and a USB camera so every shared field — incl. the USB-only
    // cam_index and the IP-only credential/channel fields — is non-null on at least
    // one row (spec §12.4 asserts all 18 columns byte-identically, incl. cam_index).
    const int64_t ip_id  = seed_camera(db->handle(), /*active*/true, /*setup*/true);
    const int64_t usb_id = seed_camera_usb(db->handle(), /*active*/false, /*setup*/true);
    const auto before_ip  = denso::camera::get(db->handle(), ip_id);
    const auto before_usb = denso::camera::get(db->handle(), usb_id);
    REQUIRE(before_ip); REQUIRE(before_usb);

    const auto r = denso::mode::switch_and_reset(db->handle(), denso::mode::TargetMode::BallLeveler);
    REQUIRE(r.ok);

    // Compare EVERY row and EVERY shared field (not just one selected row).
    const auto check_preserved = [](const denso::camera::Camera& b, const denso::camera::Camera& a) {
        CHECK(a.id == b.id);
        CHECK(a.name == b.name);
        CHECK(a.camera_type == b.camera_type);
        CHECK(a.active == b.active);                   // A1: preserved (incl. active=false)
        CHECK(a.index == b.index);                     // cam_index — the previously-missing field
        CHECK(a.ip == b.ip);
        CHECK(a.rtsp == b.rtsp);
        CHECK(a.username == b.username);
        CHECK(a.password == b.password);
        CHECK(a.channel == b.channel);
        CHECK(a.stream == b.stream);
        CHECK(a.manufacturer == b.manufacturer);
        CHECK(a.width == b.width);
        CHECK(a.height == b.height);
        CHECK(a.fps == b.fps);
        CHECK(a.pitch == b.pitch);
        CHECK(a.roll == b.roll);
        CHECK(a.rotation == b.rotation);
        CHECK(a.setup_complete == false);              // reset
        CHECK(a.areas_need_review == false);           // reset
    };
    check_preserved(*before_ip,  *denso::camera::get(db->handle(), ip_id));
    check_preserved(*before_usb, *denso::camera::get(db->handle(), usb_id));
}

TEST_CASE("reset empties the five mode-owned tables and leaves no orphans", "[mode_reset]") {
    auto db = denso::db::Db::open_in_memory(); REQUIRE(db);
    REQUIRE(denso::db::run_migrations(db->handle()));
    const int64_t id = seed_camera(db->handle(), true, true);
    // one area (zone 4), one model + class, one reading, one receipt
    exec(db->handle(), QStringLiteral("INSERT INTO camera_area (camera_id,name,points,zone) "
        "VALUES (%1,'a','[]',4)").arg(id));
    exec(db->handle(), QStringLiteral("INSERT INTO camera_model (camera_id,model_id) "
        "VALUES (%1,1)").arg(id));
    exec(db->handle(), QStringLiteral("INSERT INTO camera_model_class (camera_model_id,class_id,conf) "
        "VALUES (1,0,0.5)"));
    exec(db->handle(), QStringLiteral("INSERT INTO reading (camera_id,ts_ms,value,conf) "
        "VALUES (%1,1,1234,0.9)").arg(id));
    // One model_migration_receipt row (all 12 v13 NOT NULL columns, db.cpp:428-440).
    exec(db->handle(), QStringLiteral(
        "INSERT INTO model_migration_receipt "
        "(created_utc,old_filename,old_model_id,old_name,old_class_names,"
        " new_filename,new_model_id,new_engine_sha256,forward_map,inverse_map,attachments) "
        "VALUES ('2026-07-21T00:00:00Z','old.engine',1,'old','[]',"
        "        'new.engine',2,'deadbeef','{}','{}','[]')"));
    const auto r = denso::mode::switch_and_reset(db->handle(), denso::mode::TargetMode::BallLeveler);
    REQUIRE(r.ok);
    CHECK(count(db->handle(), "camera_model_class") == 0);
    CHECK(count(db->handle(), "camera_model") == 0);
    CHECK(count(db->handle(), "camera_area") == 0);
    CHECK(count(db->handle(), "reading") == 0);
    CHECK(count(db->handle(), "model_migration_receipt") == 0);
    CHECK(count(db->handle(), "camera") == 1);         // camera preserved
}

TEST_CASE("reset deletes camera_model_class rows already orphaned by camera::remove", "[mode_reset]") {
    auto db = denso::db::Db::open_in_memory(); REQUIRE(db);
    REQUIRE(denso::db::run_migrations(db->handle()));
    // Orphan: a class row whose parent camera_model does not exist (the state
    // camera::remove leaves behind). A scoped delete would miss it.
    exec(db->handle(), QStringLiteral(
        "INSERT INTO camera_model_class (camera_model_id,class_id,conf) VALUES (999,0,0.5)"));
    const auto r = denso::mode::switch_and_reset(db->handle(), denso::mode::TargetMode::DigitReader);
    REQUIRE(r.ok);
    CHECK(count(db->handle(), "camera_model_class") == 0);   // spec §12.16
}

TEST_CASE("after reset runtime() is empty even for an active camera", "[mode_reset]") {
    auto db = denso::db::Db::open_in_memory(); REQUIRE(db);
    REQUIRE(denso::db::run_migrations(db->handle()));
    seed_camera(db->handle(), /*active*/true, /*setup*/true);
    REQUIRE(denso::mode::switch_and_reset(db->handle(), denso::mode::TargetMode::BallLeveler).ok);
    CHECK(denso::camera::runtime(db->handle()).empty());     // spec §12.6
    CHECK(denso::camera::all(db->handle()).size() == 1);
}

TEST_CASE("reset writes the new mode and disables brazing but keeps the url", "[mode_reset]") {
    auto db = denso::db::Db::open_in_memory(); REQUIRE(db);
    REQUIRE(denso::db::run_migrations(db->handle()));
    denso::brazing::BrazingConfig b; b.enabled = true; b.base_url = "http://host:8098";
    denso::brazing::save(db->handle(), b);
    REQUIRE(denso::mode::switch_and_reset(db->handle(), denso::mode::TargetMode::BallLeveler).ok);
    CHECK(denso::mode::load(db->handle()) == denso::mode::TargetMode::BallLeveler);
    const auto after = denso::brazing::load(db->handle());
    CHECK_FALSE(after.enabled);                              // A2
    CHECK(after.base_url == "http://host:8098");             // preserved
}
```
Rollback via a `RAISE` trigger (a portable failure-injection seam that needs no prepared-statement mock):
```cpp
TEST_CASE("a failure inside the transaction rolls back mode, reporting, and camera flags", "[mode_reset]") {
    auto db = denso::db::Db::open_in_memory(); REQUIRE(db);
    REQUIRE(denso::db::run_migrations(db->handle()));
    const int64_t id = seed_camera(db->handle(), true, true);
    denso::brazing::BrazingConfig b; b.enabled = true; b.base_url = "http://host";
    denso::brazing::save(db->handle(), b);
    exec(db->handle(), QStringLiteral("INSERT INTO reading (camera_id,ts_ms,value,conf) "
        "VALUES (%1,1,1,0.5)").arg(id));
    // Force the DELETE FROM reading to fail mid-transaction.
    exec(db->handle(), QStringLiteral(
        "CREATE TRIGGER boom BEFORE DELETE ON reading BEGIN "
        "SELECT RAISE(ABORT,'injected'); END"));

    const auto r = denso::mode::switch_and_reset(db->handle(), denso::mode::TargetMode::BallLeveler);
    CHECK_FALSE(r.ok);
    CHECK(!r.error.empty());                                 // SQL error surfaced verbatim
    // Everything reverts together (spec §12.9):
    CHECK(denso::mode::load(db->handle()) == denso::mode::TargetMode::DigitReader);
    CHECK(denso::brazing::load(db->handle()).enabled == true);
    const auto after = denso::camera::get(db->handle(), id);
    REQUIRE(after);
    CHECK(after->setup_complete == true);
    CHECK(after->areas_need_review == true);
    CHECK(count(db->handle(), "reading") == 1);
}
```
**Full failure-injection matrix (spec §13 "inject a failure at each statement in turn").** Each statement has a concrete `BEFORE`-trigger that fires `RAISE(ABORT,'injected')`, run as a `GENERATE`d Catch2 `SECTION` so one `TEST_CASE` covers all of them with the identical post-condition assertions (`switch_and_reset` returns `{ok:false, error non-empty}`; mode, `brazing.enabled`, and every camera's `setup_complete`/`areas_need_review` unchanged; the target table's rows still present). Seed one active+setup camera + one row in each mode-owned table, `brazing.enabled=1`, current mode `digit_reader`, then run exactly ONE section:

| Injected failure | Trigger DDL (created before the switch) |
|---|---|
| `DELETE FROM camera_model_class` | `CREATE TRIGGER t BEFORE DELETE ON camera_model_class BEGIN SELECT RAISE(ABORT,'injected'); END` |
| `DELETE FROM camera_model` | `... BEFORE DELETE ON camera_model ...` |
| `DELETE FROM camera_area` | `... BEFORE DELETE ON camera_area ...` |
| `DELETE FROM reading` | `... BEFORE DELETE ON reading ...` |
| `DELETE FROM model_migration_receipt` | `... BEFORE DELETE ON model_migration_receipt ...` |
| `UPDATE camera SET setup_complete=0,areas_need_review=0` | `CREATE TRIGGER t BEFORE UPDATE ON camera BEGIN SELECT RAISE(ABORT,'injected'); END` |
| `settings` upsert of **`mode.target`** (1st settings statement) | `CREATE TRIGGER t BEFORE UPDATE ON settings WHEN NEW.key='mode.target' BEGIN SELECT RAISE(ABORT,'injected'); END` |
| `settings` upsert of **`brazing.enabled`** (2nd settings statement — proves failure at the SECOND upsert rolls back the first) | `CREATE TRIGGER t BEFORE UPDATE ON settings WHEN NEW.key='brazing.enabled' BEGIN SELECT RAISE(ABORT,'injected'); END` |

**Both settings keys are seeded before the switch** (`mode.target='digit_reader'`, `brazing.enabled='1'`) so each upsert takes the `ON CONFLICT … DO UPDATE` (BEFORE-UPDATE) branch and the key-specific trigger fires. The `mode.target`-key case leaves `mode.target` at the SECOND statement's failure point only if the first succeeded — so the two are genuinely distinct statements. Each section asserts the SAME invariant, giving spec §12.9 (all flags revert together) real per-statement coverage — including a failure at the *last* write before commit.

**Commit-failure path (honest scope note — NOT claimed unit-proven).** A busy/locked `COMMIT` leaving the transaction open is not deterministically reproducible over a private in-memory SQLite DB. The explicit `conn.rollback()` on `!conn.commit()` is defensive code that mirrors the **already-reviewed** `camera::remove` precedent (`camera/repo.cpp:142-147`), and is covered by that analysis rather than a new unit test. The plan states this openly instead of asserting a proof it cannot run; the on-device Jetson step (Slice 8) does not exercise it either. If a deterministic seam is later wanted, it needs a second connection contending on a shared-cache DB — out of scope for this feature.

**`preview_counts` failure test** (`[mode_reset]`):
```cpp
TEST_CASE("preview_counts returns nullopt when a count query fails", "[mode_reset]") {
    auto db = denso::db::Db::open_in_memory(); REQUIRE(db);
    REQUIRE(denso::db::run_migrations(db->handle()));
    exec(db->handle(), QStringLiteral("DROP TABLE reading"));   // force a query failure
    CHECK_FALSE(denso::mode::preview_counts(db->handle()).has_value());
}
```

- [ ] **Step 2: Run — expect FAIL** (`mode/reset.h` missing)

Run: `./build/tests/denso_tests "[mode_reset]"`
Expected: FAIL to compile.

- [ ] **Step 3: Implement `reset.cpp`** — the transaction + counts. Sketch:
```cpp
#include "mode/reset.h"
#include <QSqlError>
#include <QSqlQuery>
#include <QString>
#include <QVariant>
#include <set>
namespace denso::mode {
namespace {
bool checked(QSqlQuery& q, std::string& err) {
    if (q.exec()) return true;
    err = q.lastError().text().toStdString();
    return false;
}
}
std::optional<SwitchCounts> preview_counts(const QSqlDatabase& db) {
    SwitchCounts c;
    // Any query failure → nullopt (abort the confirmation). Never fall back to 0.
    const auto one = [&](const char* sql, int& out) -> bool {
        QSqlQuery q(db);
        if (!q.exec(QString::fromLatin1(sql)) || !q.next()) return false;
        out = q.value(0).toInt();
        return true;
    };
    if (!one("SELECT COUNT(*) FROM camera", c.cameras)) return std::nullopt;
    if (!one("SELECT COUNT(*) FROM camera_model", c.model_bindings)) return std::nullopt;
    if (!one("SELECT COUNT(*) FROM camera_area", c.areas)) return std::nullopt;
    if (!one("SELECT COUNT(*) FROM reading", c.readings)) return std::nullopt;
    if (!one("SELECT COUNT(*) FROM model_migration_receipt", c.receipts)) return std::nullopt;
    QSqlQuery z(db);
    if (!z.exec(QStringLiteral("SELECT DISTINCT zone FROM camera_area "
                               "WHERE zone IS NOT NULL AND zone != 0 ORDER BY zone")))
        return std::nullopt;
    while (z.next()) c.zones.push_back(z.value(0).toInt());
    return c;
}
ResetResult switch_and_reset(const QSqlDatabase& db, TargetMode new_mode) {
    ResetResult r;
    QSqlDatabase conn(db);                       // shares the underlying connection
    if (!conn.transaction()) { r.error = conn.lastError().text().toStdString(); return r; }
    const auto rollback = [&](const std::string& e) { conn.rollback(); r.error = e; return r; };

    for (const char* sql : {
            "DELETE FROM camera_model_class",     // unconditional — repairs orphans
            "DELETE FROM camera_model",
            "DELETE FROM camera_area",
            "DELETE FROM reading",
            "DELETE FROM model_migration_receipt",
            "UPDATE camera SET setup_complete = 0, areas_need_review = 0" }) {
        QSqlQuery q(db); q.prepare(QString::fromLatin1(sql));
        std::string e; if (!checked(q, e)) return rollback(e);
    }
    { QSqlQuery q(db);
      q.prepare(QStringLiteral("INSERT INTO settings (key,value) VALUES ('mode.target',?) "
                               "ON CONFLICT(key) DO UPDATE SET value = excluded.value"));
      q.addBindValue(QString::fromLatin1(to_string(new_mode)));
      std::string e; if (!checked(q, e)) return rollback(e); }
    { QSqlQuery q(db);
      q.prepare(QStringLiteral("INSERT INTO settings (key,value) VALUES ('brazing.enabled','0') "
                               "ON CONFLICT(key) DO UPDATE SET value = excluded.value"));
      std::string e; if (!checked(q, e)) return rollback(e); }

    if (!conn.commit()) return rollback(conn.lastError().text().toStdString());
    r.ok = true;
    return r;
}
}
```

- [ ] **Step 4: Wire CMake** (`mode/reset.cpp` into `denso_core`; `test_mode_reset.cpp` into tests).

- [ ] **Step 5: Run — expect PASS**

Run: `cmake --build build && ./build/tests/denso_tests "[mode_reset]"`
Expected: PASS (all preservation/orphan/runtime/reporting/rollback cases).

- [ ] **Step 6: Commit**

```bash
git add src/core/mode/reset.h src/core/mode/reset.cpp src/core/CMakeLists.txt \
        tests/test_mode_reset.cpp tests/CMakeLists.txt
git commit -m "feat(mode): atomic switch-and-reset transaction + preview counts"
```

**Success criteria:** spec acceptance §12.4, §12.5, §12.6, §12.7, §12.8, §12.9, §12.16 — all provable at the unit level here.

**Failure/rollback behavior:** the function itself is the rollback contract — any statement/commit failure reverts everything and returns `{ok:false, error:<verbatim SQL>}`. No source outside this module changes.

**Proposed commit boundary:** one commit.

---

## Slice 4 — Authoritative teardown-only seam

**Goal:** Expose `CameraGrid`'s single authoritative teardown (`clear()`) as a callable pre-transaction primitive, and a `CameraView` entry that tears down **without** re-querying `runtime()`, so nothing restarts the old pipeline before the reset commits. No duplicate teardown sequence.

**Dependencies:** Slice 0 merged (generation counter). Independent of Slices 1–3, but sequenced here. **Contains two commits: Task 4.0 (the locked `denso_integration_tests` harness) then Task 4.1 (the teardown seam).**

**Files:**
- **Task 4.0 (harness):** `src/app/CMakeLists.txt` (extract the `denso_app` OBJECT lib; `denso` keeps `main.cpp` + `resources.qrc` + POST_BUILD), `tests/CMakeLists.txt` (define `denso_integration_tests` AFTER the Catch2 fetch), create `tests/integration_main.cpp`.
- **Task 4.1 (teardown seam):** Modify `src/app/ui/camera/grid/camera_grid.h:42-57` (add public `teardown()`), `camera_grid.cpp` (thin wrapper); `src/app/ui/camera/camera_view.h:29-33`, `camera_view.cpp:79-90` (add `teardown_for_switch()`); `src/app/camera/camera_stream.h`, `camera_stream.cpp` (add the static construction counter); create `tests/test_mode_teardown.cpp` (in `denso_integration_tests`).

**APIs and ownership:**
- `CameraGrid` gains a public `void teardown();` whose body is exactly `clear();` (the authoritative primitive at `camera_grid.cpp:57-95`). **`clear()` is not reimplemented or copied** — `teardown()` delegates. `reload()` still calls `clear()` internally as today. This satisfies spec §6.2: "the switch must call that authoritative primitive."
- `CameraView` gains `void teardown_for_switch();` → `grid_->teardown();` then `stack_->setCurrentIndex(0)` (neutral empty page) so the view shows no stale grid during the multi-second transaction. It does **NOT** call `grid_->reload()` or `camera::runtime()` (spec §6.2 forbids `CameraView::reload()` here — it re-queries `runtime()` and restarts the old mode's cameras, `camera_view.cpp:79-86`).
- **`CameraStream` construction counter — Slice 4 is its SOLE owner (defined here, once).** Add `static std::atomic<uint64_t> s_constructed;` incremented at the top of every `CameraStream` ctor, plus `static uint64_t constructed_count();`. Contract, stated explicitly: it is **monotonic — increments only, never decrements, never resets** (a process-lifetime construction tally). Slice 6 and Slice 7 **only CONSUME** it (read `constructed_count()`); neither redefines it. Because it never resets, a test must compare its value **at precise lifecycle boundaries** (via the Slice 7 switch-observer), not merely "before vs after a whole switch" — a before/after pair around a `ball_leveler` switch that legitimately builds nothing would pass even if a stream were built and torn down mid-window. Add `#include <atomic>`.
- Warm-up is left running (spec §6.2): `teardown()`/`clear()` do not destroy `WarmupState`; late `on_model_ready`/`on_warmup_finished` are harmless no-ops once `pending_cams_` is empty.

**Explicitly unchanged behavior:** `clear()`'s exact ordering (pending → stop+delete streams → delete tiles → `reporter_.reset()` → `brazing_reporter_.reset()` → `health_.reset()` → state reset); `release_streams()` stays capture-only and is NOT used for the switch (spec §6.2 — it leaves a frame in `pending_`).

### Task 4.0 — build the `denso_integration_tests` harness (LOCKED Option A)

- [ ] **Step 0.1: Extract `denso_app` OBJECT library.** In `src/app/CMakeLists.txt`, per the locked strategy (File Structure → *Integration-test harness*): move every `add_executable(denso …)` source EXCEPT `main.cpp` **and** `resources.qrc` into `add_library(denso_app OBJECT …)` (incl. the platform backend inside `if(WIN32)/else()`); PUBLIC-link the Qt/OpenCV/backend interface + `APP_VERSION`; redefine `denso` as `add_executable(denso WIN32 main.cpp resources.qrc)` linking `denso_app`, keeping ALL POST_BUILD steps on `denso`. `resources.qrc` stays on `denso`.
- [ ] **Step 0.2: Add the target in `tests/CMakeLists.txt`** (AFTER `FetchContent_MakeAvailable(Catch2)`, so `Catch2::Catch2` exists): `add_executable(denso_integration_tests tests/integration_main.cpp)` linking `denso_app Catch2::Catch2`; `catch_discover_tests`; Windows POST_BUILD copies the ORT DLLs beside the test exe (no `models/`). Create `tests/integration_main.cpp` (offscreen `QApplication` + Catch2 session).
- [ ] **Step 0.3: Build both `denso` and `denso_integration_tests`; run `ctest`.** Expected: `denso` builds, links, **launches and shows its window icon** (manually confirm once — a buried-resource regression would drop `:/icon.png`), and runs; the (initially empty) integration target links; `ctest` discovers it; existing suites unchanged (measure via `ctest -N`, no hard-coded total). **Binary identity of `denso` is NOT asserted** — only behavioural equivalence.
- [ ] **Step 0.4: Commit (Task 4.0 — its own commit).**
```bash
git add src/app/CMakeLists.txt tests/integration_main.cpp tests/CMakeLists.txt
git commit -m "build(test): add denso_integration_tests harness (denso_app OBJECT lib)"
```

### Task 4.1 — teardown-only seam + construction counter

- [ ] **Step 1: Write the failing integration test** (`tests/test_mode_teardown.cpp`, in `denso_integration_tests`, offscreen)

The regression guard (spec §12.18, §13): the pre-transaction teardown constructs **no** `CameraStream` and restarts nothing. Use the construction counter so the assertion proves *no construction*, not merely "no live stream":
```cpp
#include <catch2/catch_test_macros.hpp>
#include "ui/camera/camera_view.h"
#include "camera/camera_stream.h"   // constructed_count()
#include "camera/repo.h"
#include "db/db.h"

TEST_CASE("teardown_for_switch stops the grid, shows the empty page, and builds no stream",
          "[mode_teardown]") {
    // Seed one runnable (active+setup, no models) camera; build CameraView over the
    // in-memory DB (ctor runs reload() → one live stream). Then tear down for a switch.
    const uint64_t after_build = denso::ui::CameraStream::constructed_count();
    view.teardown_for_switch();
    CHECK(denso::ui::CameraStream::constructed_count() == after_build);  // no NEW construction
    // and the view is on the neutral empty page, no live stream remains
    // (assert via CameraView's observable state / stack index == 0).
}
```
(The offscreen `QApplication` is provided once by `tests/integration_main.cpp` in `denso_integration_tests`; `denso_tests` stays headless/backend-free. Widget assertions use the real `CameraView` API.)

- [ ] **Step 2: Run — expect FAIL** (`teardown`/`teardown_for_switch` missing)

- [ ] **Step 3: Implement** `CameraGrid::teardown()` (delegates to `clear()`), `CameraView::teardown_for_switch()`.

- [ ] **Step 4: Run — expect PASS.**

- [ ] **Step 5: Commit (Task 4.1)**

```bash
git add src/app/ui/camera/grid/camera_grid.h src/app/ui/camera/grid/camera_grid.cpp \
        src/app/ui/camera/camera_view.h src/app/ui/camera/camera_view.cpp \
        src/app/camera/camera_stream.h src/app/camera/camera_stream.cpp \
        tests/test_mode_teardown.cpp tests/CMakeLists.txt
git commit -m "feat(camera): teardown-only seam + stream-construction counter for the mode switch"
```

**Success criteria:** spec §6.2, §12.11, §12.18 — one authoritative teardown, no duplicate sequence, no `CameraStream` built between confirm and commit (proven at lifecycle boundaries via the construction counter).

**Failure/rollback behavior:** teardown is effectively non-throwing (join has no timeout — a hung capture thread is an unresolved operational failure, spec §6.7, out of scope; a watchdog is separate design work R1). If a regression appears, revert; no data is touched.

**Proposed commit boundary:** two commits — **Task 4.0** (`denso_integration_tests` harness / `denso_app` extraction) then **Task 4.1** (teardown seam + counter).

---

## Slice 5 — Settings mode selector + destructive confirmation

**Goal:** An application-wide mode selector in Settings that emits a `switch_mode_requested` intent, refusing the currently-active mode; plus the destructive `mode_confirm_dialog` with real counts, default Cancel, busy/disabled repeat protection, stating exactly what is kept/destroyed. (The actual switch is orchestrated in Slice 7.)

**Dependencies:** Slice 1 (mode load), Slice 3 (`preview_counts`). Slice 7 consumes this slice's signal.

**Files:**
- Modify: `src/app/ui/settings/settings_dialog.h:48-91`, `settings_dialog.cpp` (add a Mode section + `switch_mode_requested(int)` signal + `set_current_mode`).
- Create: `src/app/ui/settings/mode_confirm_dialog.h`, `mode_confirm_dialog.cpp`.
- Modify: `src/app/CMakeLists.txt` (add the new dialog source), `tests/CMakeLists.txt` if a pure copy-builder is tested.

**APIs and ownership:**
- `SettingsDialog` gains:
  - `void set_current_mode(mode::TargetMode m);` — seeds the selector without emitting.
  - `signals: void switch_mode_requested(int target /* static_cast<int>(TargetMode) */);`
  - A `QComboBox`/two radio buttons for the two modes plus a **Switch and Reset** button that is **disabled when the selected target equals the current mode** (spec §5/§7: refuse switching to the active mode). The button emits `switch_mode_requested(selected)`.
- `ModeConfirmDialog` (own modal, follows the Areas destructive-save precedent `areas_page.cpp:734-751`: real counts, closed question, **default Cancel**, no type-to-confirm):
```cpp
class ModeConfirmDialog : public QDialog {
public:
    // `counts` from mode::preview_counts; `target` names the destination.
    ModeConfirmDialog(mode::TargetMode target, const mode::SwitchCounts& counts,
                      QWidget* parent = nullptr);
};
```
  The dialog **builds its copy from real counts** and states, per spec §7.1:
  - "**N camera connections will be kept** — sources, credentials, resolution and orientation are preserved."
  - "Their <current-mode> setup will be deleted: model bindings, **A detection areas**, **Z reported zones (list)**, **R stored readings**, and **K model-rollback receipts**. Each camera will need processing setup again."
  - "**Server reporting will be turned off.** The server address is kept; you must re-enable reporting yourself."
  - When target is `ball_leveler`: "**Floating Ball Leveler setup is not available in this release.**"
  - "This cannot be undone." Buttons: `[Cancel]` (default) `[Switch and Reset]`.
  - The dialog **must not** say or imply cameras are deleted (spec §7.1).
- The **counts are read immediately before showing the dialog** (Slice 7 calls `preview_counts` then constructs `ModeConfirmDialog`), so they are real.

**Explicitly unchanged behavior:** every existing Settings panel (Appearance/Display/System/Network/Server/About) and its signals; the network/display/theme flows.

- [ ] **Step 1: Write a failing test for the confirmation copy builder**

Extract the count→text formatting into the pure `mode_confirm_text.{h,cpp}` module (LOCKED — compiled into `denso_app` AND `denso_tests`, per the File Structure decision), so it is unit-testable without a widget:
```cpp
// src/app/ui/settings/mode_confirm_text.h (free function, denso::ui — Qt Core only)
QString mode_confirm_body(mode::TargetMode target, const mode::SwitchCounts& c);
```
Test (`tests/test_mode_confirm_text.cpp`, `[mode_confirm]`):
```cpp
TEST_CASE("confirmation body states kept connections and destroyed setup", "[mode_confirm]") {
    denso::mode::SwitchCounts c; c.cameras = 3; c.areas = 7;
    c.zones = {3,4,5,7}; c.readings = 1284; c.receipts = 2; c.model_bindings = 3;
    const QString body = denso::ui::mode_confirm_body(denso::mode::TargetMode::BallLeveler, c);
    CHECK(body.contains(QStringLiteral("3 camera connections")));
    CHECK(body.contains(QStringLiteral("7 detection areas")));
    CHECK(body.contains(QStringLiteral("1,284")));         // grouped thousands
    CHECK(body.contains(QStringLiteral("reporting will be turned off")));
    CHECK(body.contains(QStringLiteral("not available in this release")));  // ball_leveler
    // POSITIVE: it must state connections are KEPT (spec §7.1 — the dialog must not
    // say or imply cameras are deleted).
    CHECK(body.contains(QStringLiteral("camera connections will be kept")));
    // NEGATIVE: no phrasing that implies the cameras/connections themselves are removed.
    // (The valid copy legitimately contains both "camera" and "deleted" — the deletion
    // applies to the PROCESSING setup, so test exact forbidden implications, not a
    // naive "delete" AND "camera" co-occurrence.)
    CHECK_FALSE(body.contains(QStringLiteral("camera connections will be deleted")));
    CHECK_FALSE(body.contains(QStringLiteral("delete cameras")));
    CHECK_FALSE(body.contains(QStringLiteral("remove camera connections")));
    CHECK_FALSE(body.contains(QStringLiteral("cameras will be deleted")));
}
```
(Register `mode_confirm_text.cpp` in BOTH `denso_app` and the `denso_tests` source list — the precedent is `grid_layout.cpp` at `tests/CMakeLists.txt:83`; register `test_mode_confirm_text.cpp` in `denso_tests`.)

- [ ] **Step 2: Run — expect FAIL.**

- [ ] **Step 3: Implement** `mode_confirm_body` (grouped thousands via an explicit English locale — `QLocale(QLocale::English, QLocale::UnitedStates).toString(static_cast<qlonglong>(n))`; the C locale does NOT group, so `QLocale::c()` would yield "1284"), the `ModeConfirmDialog`, and the Settings Mode section + `switch_mode_requested` signal + same-mode disable.

- [ ] **Step 4: Run — expect PASS** for `[mode_confirm]`; build the full app to confirm the dialog compiles/links.

- [ ] **Step 5: Commit**

```bash
git add src/app/ui/settings/settings_dialog.h src/app/ui/settings/settings_dialog.cpp \
        src/app/ui/settings/mode_confirm_dialog.h src/app/ui/settings/mode_confirm_dialog.cpp \
        src/app/ui/settings/mode_confirm_text.h src/app/ui/settings/mode_confirm_text.cpp \
        src/app/CMakeLists.txt tests/test_mode_confirm_text.cpp tests/CMakeLists.txt
git commit -m "feat(settings): mode selector + destructive switch confirmation"
```

**Success criteria:** spec §7.1 (copy content, default Cancel), §5/§7 (refuse the current mode), A3. Busy/disable-repeat is wired in Slice 7 (the button is disabled the instant it is clicked, and re-enabled only after the transaction resolves).

**Failure/rollback behavior:** UI-only; emitting the signal does nothing until Slice 7 handles it. No DB writes here.

**Proposed commit boundary:** one commit.

---

## Slice 6 — Retained-camera setup-required UI (three CameraView states)

**Goal:** Give `CameraView` a third state so a post-switch appliance (`all()` non-empty, `runtime()` empty) shows retained connections and a mode-appropriate action, never the false "No cameras yet".

**Dependencies:** Slice 1 (mode load). Independent of Slices 3–5; sequenced here so Slice 7 can rely on it.

**Files:**
- Modify: `src/app/ui/camera/camera_view.h`, `camera_view.cpp:79-86` (add page 2 + the mode-first state selection).
- Modify: `src/app/ui/camera/grid/camera_grid.h`, `camera_grid.cpp` (add `void publish_idle_status();` — the single-owner idle status write).
- Create: `tests/test_camera_view_states.cpp` (in `denso_integration_tests`, offscreen, model-less cameras).

**APIs and ownership:**
- Add a third stacked page (index 2): a **retained-connections list** showing each `camera::all()` row's name and source (built from `Camera` fields; RTSP shown credential-free — reuse `camera/rtsp_templates.*` conventions / show `ip`+`stream` or `usb index`, never the password).
- **Mode is a HARD admission gate — read the mode FIRST (spec §3.5, §12.14).** `reload()` reads `mode::load(db_)` before anything else. `CameraView` gains `#include "mode/config.h"`. Selection:
  - **`ball_leveler` — ALWAYS the unavailable state, regardless of whether any camera rows exist.** Never page 0 (page 0 exposes **+ Add Camera**, which violates the unavailable-destination contract). `reload()` calls `grid_->teardown()` (Slice 4 — leaves the grid empty, constructs **no** `CameraStream`/processor/reporter) then `grid_->publish_idle_status()` (below), and shows **page 2** in its `ball_leveler` variant:
    - `all()` non-empty → header "**N camera connections kept**", a **read-only** list of retained cameras (name + credential-free source) + "**Floating Ball Leveler setup is not available in this release**".
    - `all()` empty → the same unavailable state with **no** camera list and **no actions** ("Floating Ball Leveler setup is not available in this release").
    - In **both** sub-cases: **no + Add Camera, no Set up cameras, no wizard entry, no stream/processor/reporter.** `grid_->reload()` is never called, so even a hand-repaired/restored DB carrying `mode.target=ball_leveler` **and** a completed active camera cannot start the digit-reader pipeline (closes the `runtime()`-ignores-mode gap, `camera_grid.cpp:97-207`). (The top-bar **Camera** button is also gated off in this mode — Slice 7.)
  - **`digit_reader`, `all()` empty →** page 0 ("No cameras yet" + **+ Add Camera**).
  - **`digit_reader`, `all()` non-empty, `runtime()` empty →** page 2 (digit_reader variant), header "**N camera connections kept — processing setup required**", a **Set up cameras** button that emits `add_camera_requested` (opens the existing camera dialog / wizard).
  - **`digit_reader`, `runtime()` non-empty →** page 1 (live grid), via the existing `grid_->reload()`, unchanged.

  **Spec-table note:** §7.2's table row "`runtime()` non-empty → live grid, unchanged" is written mode-agnostically because within the *switch flow* a committed `ball_leveler` always leaves `runtime()` empty (§6.4). Making mode an explicit gate here does not contradict that flow — it hardens §12.14 into a true invariant for out-of-flow DB states, exactly as §3.5 ("processor is a pure function of mode + camera config") intends. A `digit_reader` appliance is entirely unaffected.
- **`CameraGrid::publish_idle_status()` (added here; keeps the single status owner — Slice 2 principle).** Computes the **real** verdict `verdict_ = health::evaluate_integrity(db_, denso::paths::models_dir())`, then writes `status.json` with empty runtime causes (no streaming cameras) and the mode fields: `const auto m = mode::load(db_); health::write_status_file(denso::paths::status_file(), verdict_, {}, {}, {}, QString::fromLatin1(mode::to_string(m)), mode::mode_setup_required(db_, m))`. In `ball_leveler` this yields `mode_setup_required: true` **permanently** (spec §2.1), even when a completed camera is retained. It **never** passes an empty `IntegrityVerdict{}` — it recomputes the true one. This makes `CameraGrid` the sole runtime writer of `status.json` for both the live and idle cases, so Slice 7 does not write it directly.
- **`CameraGrid::reload_invocations()` (Slice 6 observable; monotonic, increment-only) — proves "no pipeline built" for all three object kinds.** Increment a `static`/member counter at the top of `CameraGrid::reload()`'s body and expose `uint64_t reload_invocations() const`. Because the reporter, `ZoneHealth`, every `DetectionProcessor`, and every `CameraStream` are constructed **only inside `reload()`/`start_one`** (`camera_grid.cpp:97-295`), an unchanged `reload_invocations()` across a `CameraView::reload()` proves **none** of them was built — the single observable that covers §12.14's "no CameraStream, processor OR reporter" without three separate counters. Slice 6 owns this counter (distinct object from Slice 4's `CameraStream::constructed_count()`); tests consume both.
- Because mode is read inside `reload()`, the state always matches the committed mode and self-corrects on the rollback re-read of Slice 7.
- **No production stream is constructed on page 2.** The `digit_reader` wizard's snapshot preview (`open_configure()` → `grab_snapshot()`) is NOT a `CameraStream` (spec §7.2 "No stream means no production CameraStream") and remains available and correct — it works precisely because A1 kept the credentials. For `ball_leveler`, no wizard is exposed, so even the snapshot path is unreachable, and reporting is disabled by Slice 3.

**Explicitly unchanged behavior:** page 0 empty-state and page 1 live grid; `mark_setup_complete` remains the sole writer of `setup_complete=1` (`repo.cpp:191-203`); retained cameras re-enter service only through the existing wizard's terminal action — no new completion path.

- [ ] **Step 1: Write failing offscreen tests** (`[camera_view_states]`, in `denso_integration_tests`):
  - (a) `digit_reader`, `all()` empty → page 0 (**+ Add Camera** present).
  - (b) `digit_reader`, one active+setup_complete camera → runtime non-empty → page 1 (grid).
  - (c) `digit_reader`, one camera with `setup_complete=0` → page 2 **with** the **Set up cameras** action; header text asserted.
  - (d) `ball_leveler`, one camera with `setup_complete=0` → page 2 (unavailable variant): retained list shown, **no** Set up cameras, **no** + Add Camera; unavailable-text asserted; grid stream count unchanged.
  - (e) **Blocker regression (spec §12.14):** `mode.target=ball_leveler` **and** an active `setup_complete=1` camera (an out-of-flow DB state) → page 2 (unavailable variant). Prove **no runtime pipeline is built** via `CameraGrid::reload_invocations()` (Slice 6 observable): its value is **unchanged** across `CameraView::reload()`, so the grid's build path (which is the sole constructor of the reporter, `ZoneHealth`, every `DetectionProcessor`, and every `CameraStream`) never ran — covering all three object kinds, not just `CameraStream`. Corroborate with `CameraStream::constructed_count()` unchanged. No setup action, no + Add Camera. Assert `status.json` reports **`mode_setup_required: true`** despite the completed camera (spec §2.1).
  - (f) **Zero-camera `ball_leveler` (Finding 4):** `mode.target=ball_leveler`, `all()` empty → page 2 unavailable state, **never page 0**; assert the widget tree exposes **no + Add Camera and no Set up cameras** control, `reload_invocations()` unchanged, and `status.json` reports `mode: "ball_leveler"` with `mode_setup_required: true`.

- [ ] **Step 2: Run — expect FAIL.**

- [ ] **Step 3: Implement** page 2 + the three-way `reload()` selection.

- [ ] **Step 4: Run — expect PASS.**

- [ ] **Step 5: Commit**

```bash
git add src/app/ui/camera/camera_view.h src/app/ui/camera/camera_view.cpp \
        src/app/ui/camera/grid/camera_grid.h src/app/ui/camera/grid/camera_grid.cpp \
        tests/test_camera_view_states.cpp tests/CMakeLists.txt
git commit -m "feat(camera): mode-gated view states + single-owner idle status"
```

**Success criteria:** spec §7.2, §12.12, §12.14 — post-switch UI shows retained connections and a setup-required state (never "No cameras yet"); `ball_leveler` exposes no setup action / no stream.

**Failure/rollback behavior:** UI-only. If a regression appears, revert; the two-page behavior is fully preserved by the `all() empty / runtime() non-empty` branches.

**Proposed commit boundary:** one commit.

---

## Slice 7 — Orchestration and rollback recovery

**Goal:** Wire the full sequence in `MainWindow`: confirm → busy → teardown-only → atomic reset → update in-memory mode **only after commit** → reload after commit → on rollback re-read the old mode and rebuild the old pipeline → refresh `status.json`.

**Dependencies:** Slices 3, 4, 5, 6 merged.

**Files:**
- Modify: `src/app/ui/mainwindow.h:40-75` (add `mode::TargetMode current_mode_;`, `bool switch_active_ = false;`, a `QPushButton* camera_btn_` member so the gate can disable it, the `SwitchEvent` enum + observer, and the `on_switch_mode`/`perform_switch`/`current_mode`/`set_switch_observer`/`apply_camera_button_gate` declarations), `mainwindow.cpp` (implement; wire the Settings signal in the ctor `:103-110`; promote the local `camera_btn` at `:64` to the `camera_btn_` member).
- Modify (R4 discard logging): `src/app/brazing/brazing_retry_policy.h`, `brazing_retry_policy.cpp` (add pure `pending_zone_numbers()`); `src/app/brazing/brazing_reporter.h`, `brazing_reporter.cpp` (log on destruction via that API).
- Create: `tests/test_mode_switch_flow.cpp` (in `denso_integration_tests`), `tests/test_brazing_discard.cpp` (the log-capture test, in `denso_integration_tests`); extend `tests/test_brazing_retry_policy.cpp` (in `denso_tests`) with `pending_zone_numbers()` cases.
- Modify: `tests/CMakeLists.txt` (register the new test sources in the right targets).

**APIs and ownership:**
- Add `mode::TargetMode current_mode_;` to `MainWindow`, initialized in the ctor from `mode::load(db_)`. Seed `settings_->set_current_mode(current_mode_)` in `apply_startup()` / `open_settings()`.
- Connect `settings_->switch_mode_requested` → `MainWindow::on_switch_mode(int target)`.
- **Test seam + observables (resolves the placeholder-test and no-construction gaps).** Split the handler so the dialog is separable from the lifecycle:
  - `void MainWindow::on_switch_mode(int target)` — validates + confirms (dialog), then calls `perform_switch`.
  - `mode::TargetMode MainWindow::perform_switch(mode::TargetMode target)` — the pure lifecycle (teardown → reset → rebuild), **no dialog**, returns the mode now in effect. Tests call `perform_switch` directly, so no dialog automation is needed.
  - **Switch-lifecycle observer (owned by the integration harness).** Add a test-only `void MainWindow::set_switch_observer(std::function<void(SwitchEvent)>)` (default: no-op), with `enum class SwitchEvent { TeardownStarted, TeardownCompleted, TransactionStarted, TransactionCommitted, TransactionRolledBack, ReloadStarted };`. `perform_switch` fires each event at its exact boundary. A test installs an observer that records `(event, CameraStream::constructed_count())` into a vector, then asserts the counter value is **identical at `TeardownStarted` and at the terminal transaction event (`TransactionCommitted`/`TransactionRolledBack`)** — i.e. **no `CameraStream` was constructed anywhere in that window** (spec §12.18). This is a boundary comparison, not a whole-switch before/after, so it holds even when the mode legitimately builds streams after reload.
  - **Counter is NOT redefined here** — Slice 4 owns `CameraStream::constructed_count()`; Slice 7 only reads it inside the observer.
  - Read observables for the mode: tests assert via `mode::load(db_)` (DB truth) plus a test-only `MainWindow::current_mode()` accessor for the in-memory value; the two must agree after both success and rollback.
- `on_switch_mode(int target)` (dialog half):
  1. **Refuse** if `display_txn_active_` (spec §6.1 — no switch during a display confirm/revert) or if `switch_active_`.
  2. `const mode::TargetMode want = mode::from_index(target);` (validated — never an invalid enum, Finding 6). **Refuse** if `want == current_mode_` (spec §5/§7 — no switch to the active mode).
  3. `const auto counts = mode::preview_counts(db_);` **If `!counts`** (a count query failed) → log + show an error, **abort with NO teardown** (A3: never confirm on fabricated zeros). Else show `ModeConfirmDialog(want, *counts, this)`. If not Accepted → return.
  4. `perform_switch(want);`
- `perform_switch(mode::TargetMode want)` (lifecycle half; spec §6.1, §6.6, §6.7, §7, §12.10, §12.17). Each numbered step fires the matching `SwitchEvent` (for the harness observer):
  1. **Busy state:** set `switch_active_ = true` and disable the switch action for the whole synchronous teardown so a second trigger cannot queue behind the multi-second join (spec §6.1-R1).
  2. **Teardown-only** → emit `TeardownStarted`; `camera_view_->teardown_for_switch();` (Slice 4 — NOT `reload()`); emit `TeardownCompleted`.
  3. **Atomic reset** → emit `TransactionStarted`; `const auto r = mode::switch_and_reset(db_, want);`.
  4. **On success (`r.ok`)** → emit `TransactionCommitted`: set `current_mode_ = want` **only now** (spec §6.7, §12.17 — never optimistically before commit). `apply_camera_button_gate();` (below). Emit `ReloadStarted`; `camera_view_->reload();` (rebuilds for the new mode — `runtime()` is empty, so page 2 shows; for `ball_leveler` Slice 6 builds no grid and writes idle status). `settings_->set_current_mode(current_mode_)`.
  5. **On failure (`!r.ok`)** → emit `TransactionRolledBack`: **re-read** `current_mode_ = mode::load(db_)` (the DB rolled back → the *old* mode; never keep an optimistic target). `apply_camera_button_gate();`. Emit `ReloadStarted`; `camera_view_->reload();` rebuilds the **old** mode's pipeline; surface `r.error` verbatim via a non-modal message/log (the core function pops no UI; MainWindow does). `settings_->set_current_mode(current_mode_)`.
  6. Clear `switch_active_`; return `current_mode_`.
- **`status.json` is written by `camera_view_->reload()` in BOTH paths — `perform_switch` never writes it directly (single-owner rule, Slice 2/6).** In `digit_reader` the rebuilt grid's `refresh_status_file()` writes the live verdict; in `ball_leveler` `CameraView::reload()` calls `grid_->publish_idle_status()` which writes the **real** `evaluate_integrity` verdict + mode fields. No `IntegrityVerdict{}` placeholder is ever written, so a mode switch cannot erase real blocker/issue/cause state.
- **Post-commit reload does NOT report failure (Finding 9 — resolved by definition).** `CameraView::reload()` and `CameraGrid::start_one` already **firewall** engine-construction exceptions internally — a failed camera is shown Offline and skipped (`camera_grid.cpp:265-272`), and `reload()` returns `void`. For this feature `reload()` is therefore **defined as non-failing**: there is no "rebuild after a successful commit fails" branch to surface, and the plan makes no such untestable claim. (Spec §6.7's row 4 is a defensive statement, not a code path this design can trigger; the DB is already consistent, so nothing is owed the operator beyond the first-run state `reload()` produces.)
- **Top-bar Camera button gate (Finding 4 — `ball_leveler` exposes no wizard).** Add `void MainWindow::apply_camera_button_gate();` → the top-bar **Camera** button (`mainwindow.cpp:64-65`) and `open_camera()` are **disabled/short-circuited when `current_mode_ == BallLeveler`**, so neither the button nor any code path opens the camera wizard in that mode. Called from the ctor (after loading `current_mode_`) and from steps 4/5, so a booted `ball_leveler` appliance also has it gated. `open_camera()` returns early if `current_mode_ == BallLeveler`.
- **Pending-snapshot discard logging (R4, spec §6.6) — concrete owner + API.** Two named pieces:
  - **`BrazingRetryPolicy::pending_zone_numbers() const` (pure, `denso_brazing`, `brazing_retry_policy.{h,cpp}`):** `std::optional<std::vector<int>>` — the ascending **keys** of `pending_` when a snapshot is undelivered (`pending_ != delivered_`, `brazing_retry_policy.h:41-42`), else `nullopt`. Returns **zone numbers only — never values.** Unit-tested in `denso_tests` (links `denso_brazing`).
  - **`~BrazingReporter()` (`denso_app`, `brazing_reporter.cpp`):** on destruction, `if (auto z = policy_.pending_zone_numbers()) qInfo().noquote() << "[brazing] discarding undelivered snapshot for" << z->size() << "zones:" << <join z>;` — count + numbers, no values. (`policy_` is the reporter's `BrazingRetryPolicy` member, `brazing_reporter.h:36`.) This runs exactly when `brazing_reporter_.reset()` fires in `CameraGrid::clear()` (`camera_grid.cpp:81`) during the switch teardown. Log-capture tested in `denso_integration_tests` (links `denso_app`) with a stub `BrazingTransport` that never acks (see Slice 8 Step 5).

**Explicitly unchanged behavior:** display confirm/revert flow; the camera modal open/close reload; theme/network handlers; the reporting-guarantee mechanism (blocked GUI thread + processor-owned snapshot context + reporter-parented timer + destroyed QNAM — spec §6.6). After a successful switch, `brazing.enabled=false` means the rebuilt grid constructs no reporter at all (`camera_grid.cpp:128-148` gates on `bcfg.enabled`).

- [ ] **Step 1: Write failing offscreen orchestration tests** (`[mode_switch_flow]`)

Drive `perform_switch` directly (the dialog-free seam) against a seeded DB + `MainWindow` built offscreen. Observables: `MainWindow::current_mode()`, `mode::load(db)`, `camera::runtime(db)`, `brazing::load(db)`, `CameraStream::constructed_count()`, and the on-disk `status.json`.
```cpp
TEST_CASE("a committed switch updates in-memory mode only after commit and empties runtime",
          "[mode_switch_flow]") {
    // seed one active+setup camera; window built; REQUIRE current_mode()==DigitReader.
    const uint64_t before = denso::ui::CameraStream::constructed_count();
    const auto now = win.perform_switch(denso::mode::TargetMode::BallLeveler);
    CHECK(now == denso::mode::TargetMode::BallLeveler);
    CHECK(win.current_mode() == denso::mode::TargetMode::BallLeveler);
    CHECK(denso::mode::load(db) == denso::mode::TargetMode::BallLeveler);
    CHECK(denso::camera::runtime(db).empty());
    CHECK_FALSE(denso::brazing::load(db).enabled);
    // ball_leveler builds no grid → NO new CameraStream across the whole switch.
    CHECK(denso::ui::CameraStream::constructed_count() == before);
    // status.json now names the new mode.
    QFile f(denso::paths::status_file()); REQUIRE(f.open(QIODevice::ReadOnly));
    CHECK(QJsonDocument::fromJson(f.readAll()).object().value("mode").toString()
          == QStringLiteral("ball_leveler"));
}

TEST_CASE("a rolled-back switch keeps the OLD mode in memory and in the DB (no optimistic value)",
          "[mode_switch_flow]") {
    // seed active+setup camera + brazing.enabled=1; install a BEFORE-DELETE-ON-reading
    // RAISE trigger so switch_and_reset fails and rolls back.
    const auto now = win.perform_switch(denso::mode::TargetMode::BallLeveler);
    CHECK(now == denso::mode::TargetMode::DigitReader);          // re-read from DB
    CHECK(win.current_mode() == denso::mode::load(db));          // memory == DB truth
    CHECK(win.current_mode() == denso::mode::TargetMode::DigitReader);
    CHECK(denso::brazing::load(db).enabled);                     // reporting flag reverted
    CHECK_FALSE(denso::camera::runtime(db).empty());             // old pipeline still admissible
}

TEST_CASE("no CameraStream is constructed between teardown start and transaction end",
          "[mode_switch_flow]") {
    // Record the construction counter at each lifecycle boundary via the observer,
    // then assert it did not move across the [TeardownStarted .. terminal] window
    // (spec §12.18). This is a boundary comparison, NOT a whole-switch before/after.
    std::map<denso::ui::MainWindow::SwitchEvent, uint64_t> at;
    win.set_switch_observer([&](denso::ui::MainWindow::SwitchEvent e) {
        at[e] = denso::ui::CameraStream::constructed_count();
    });
    win.perform_switch(denso::mode::TargetMode::BallLeveler);   // seeded digit_reader → ball_leveler
    using E = denso::ui::MainWindow::SwitchEvent;
    REQUIRE(at.count(E::TeardownStarted));
    REQUIRE(at.count(E::TransactionCommitted));
    CHECK(at[E::TransactionCommitted] == at[E::TeardownStarted]);   // no construction in-window
}

TEST_CASE("a count-query failure aborts confirmation with no teardown", "[mode_switch_flow]") {
    // DROP TABLE reading, then invoke on_switch_mode(BallLeveler): preview_counts→nullopt,
    // so no dialog, no teardown; CHECK current_mode unchanged and constructed_count unchanged.
}
```
The offscreen `QApplication` lives in `tests/integration_main.cpp` (`denso_integration_tests`); a `MainWindow` construction helper builds the window over the seeded in-memory DB. `CameraStream::constructed_count()`, `MainWindow::current_mode()`, `MainWindow::set_switch_observer()`, and `MainWindow::perform_switch()` are the only added test surfaces.

- [ ] **Step 2: Run — expect FAIL.**

- [ ] **Step 3: Implement.** (a) R4 discard logging first (its own commit): `BrazingRetryPolicy::pending_zone_numbers()` (pure, `denso_brazing`) with the `denso_tests` unit case, then `~BrazingReporter` logging it with the `denso_integration_tests` `test_brazing_discard.cpp` capture. (b) Then `on_switch_mode` + `perform_switch` + `current_mode_` + `switch_active_` busy guard + `apply_camera_button_gate` + the `SwitchEvent` observer + ctor signal wiring.

- [ ] **Step 4: Run — expect PASS** for `[mode_switch_flow]`; full app build.

- [ ] **Step 5: Commit**

```bash
# Commit 1 — R4 discard logging (separately reviewable; brazing-only)
git add src/app/brazing/brazing_retry_policy.h src/app/brazing/brazing_retry_policy.cpp \
        src/app/brazing/brazing_reporter.h src/app/brazing/brazing_reporter.cpp \
        tests/test_brazing_retry_policy.cpp tests/test_brazing_discard.cpp tests/CMakeLists.txt
git commit -m "feat(brazing): log discarded undelivered snapshot (zone numbers only) on teardown"
# Commit 2 — the orchestration
git add src/app/ui/mainwindow.h src/app/ui/mainwindow.cpp \
        tests/test_mode_switch_flow.cpp tests/CMakeLists.txt
git commit -m "feat(mode): orchestrate switch-and-reset with commit/rollback recovery"
```

**Success criteria:** spec §6.1, §6.6, §6.7, §12.10, §12.17, §12.18 — busy/refuse gating, teardown-before-transaction, in-memory mode updated only after commit, rollback re-reads the DB, no old-mode callback/reporting survives, no optimistic target after failure.

**Failure/rollback behavior:** this slice *is* the recovery orchestration. A failed reset → old pipeline rebuilt, verbatim SQL surfaced, in-memory mode re-read from DB. `reload()` is non-failing (Finding 9), so there is no post-commit-rebuild failure branch to handle — a committed switch always lands on the target's first-run state.

**Proposed commit boundary:** two commits — the R4 brazing discard-logging (brazing-only, separately reviewable) then the MainWindow orchestration.

---

## Slice 8 — Validation and documentation

**Goal:** Prove the whole feature end to end, update docs, and gate on the Jetson `.15` — without touching `.81`.

**Dependencies:** Slices 0–7 merged.

**Files:**
- Modify: `CLAUDE.md` (Layout table: add `src/core/mode/`), `AGENTS.md` (mention the mode key + switch), `docs/ARCHITECTURE.md` (mode concept, teardown-before-transaction rule, reporting guarantee). Packaging docs/tests only if a packaging file changes (it does not — so `tests/packaging/run.sh` is NOT required by this feature).
- Add/confirm the integration tests from Slices 4/6/7 exist and pass together.

**Tasks (each a checkbox; no source logic changes beyond docs + any test gaps):**

- [ ] **Step 1: Full unit + integration suite (Windows/MSYS2)**

Run: `export PATH=/c/msys64/ucrt64/bin:$PATH && cmake -S . -B build -G Ninja && cmake --build build && ctest --test-dir build`
Expected: every non-intentionally-skipped test passes across **both** targets (`denso_tests` + `denso_integration_tests`); the only permitted skip is the known Windows symlink case (#32, rendered Failed by ctest on Windows). Record the actual `ctest -N` total (do not assert a fixed number) and report the two targets separately.

- [ ] **Step 2: Digital Number Reader no-switch regression**

Confirm a `digit_reader` DB that never switches behaves identically: the existing camera/detection/brazing/zone tests are unchanged and green (spec §8, §12.1). No test asserting old behavior was modified except the status.json default-arg additions.

- [ ] **Step 3: Reporting-isolation — concrete observables, not narration** (spec §12.10)

Split into what is deterministically observable without a GPU:
  - **No reporter is rebuilt after a switch:** an offscreen `[mode_switch_flow]` case already asserts `brazing::load(db).enabled == false` post-switch, and the grid gates reporter construction on `bcfg.enabled` (`camera_grid.cpp:128-148`) — so no `ZoneReporter`/`BrazingReporter` is built. Corroborate with `CameraGrid::reload_invocations()` for the `ball_leveler` path (no pipeline at all).
  - **No stale `delivered_` suppresses the next mode's first snapshot:** a **unit** test at the retry-policy level (`brazing_retry_policy`, `denso_brazing`) — a fresh `BrazingRetryPolicy` (built per reload) starts with empty `delivered_` (`brazing_retry_policy.cpp:15`), so its first `submit` sends. Assert directly on a fresh policy.
  - **"No `on_zones` after the inference join" + teardown ordering:** a focused **unit** test at the `frame_processor` level (`denso_camera`) with a recording stub `ZoneSink` — construct a `DetectionProcessor` with the stub, then destroy it and assert the stub records **no** call after destruction (the `~DetectionProcessor` joins the inference worker). The reporter→brazing→health release **order** is the fixed contract in `CameraGrid::clear()` (`camera_grid.cpp:80-84`), reviewed as code; the integration test does not re-observe destructor ordering it cannot instrument.

- [ ] **Step 4: Prerequisite regression (Slice 0) — deterministic, GPU-free**

The full end-to-end (a real inference worker firing `WorkerFailedFn`) needs a detection camera with an engine, which the **model-less** harness deliberately avoids, so it is confirmed on-device (Step 7). Deterministically here:
  - **Unit (Slice 0):** `callback_is_current(old_gen, new_gen)` returns false — the drop logic.
  - **Integration:** expose a test-only `uint64_t CameraGrid::generation() const`; assert it **advances** across `teardown()`/`reload()` (so a handler that captured the old value is dropped by the gate). Together these prove the mechanism (§12.15) without a GPU; the on-device step exercises a live stale callback.

- [ ] **Step 5: Pending-snapshot discard logging — correct targets + named API** (spec §6.6-R4). Two tests (implemented in Slice 7, confirmed here):
  - **`denso_tests` unit test** (`test_brazing_retry_policy.cpp`, links `denso_brazing`): submit `{3:120, 4:35}` to a `BrazingRetryPolicy` with no `on_result(true)`, assert `pending_zone_numbers()` returns `{3,4}` (**numbers only**); after `on_result(true)` (delivered==pending) it returns `nullopt`. This is the pure API that exposes zone keys without values.
  - **`denso_integration_tests` test** (`test_brazing_discard.cpp`, links `denso_app` — because `BrazingReporter` lives in `denso_app`, not `denso_brazing`): construct a `BrazingReporter` with a stub `BrazingTransport` whose `post(zones, done)` (`brazing_transport.h:17`) **retains/ignores `done` and never invokes it** (never acks), `submit` `{3:120, 4:35}`, then **destroy the reporter**. A scoped `qInstallMessageHandler` captures `~BrazingReporter`'s line; assert it contains the count and zone numbers (`3`, `4`) and does **NOT** contain the values (`120`/`35`).

- [ ] **Step 6: Documentation updates** — land the `CLAUDE.md`/`AGENTS.md`/`docs/ARCHITECTURE.md` edits above in this slice's commit.

- [ ] **Step 7: Native Jetson validation — `192.168.1.15` ONLY**

On `.15` (aarch64, JP6.2, TRT 10.3): `cmake -S . -B build && cmake --build build -j4 && ctest --test-dir build` → every non-intentionally-skipped test passes (the Windows symlink case runs+passes here); record the actual `ctest -N` total per target, do not assert a fixed number. Then, with an isolated `DENSO_DATA_DIR`:
  - Real switch with live RTSP cameras; **measure** the GUI stall during teardown (R1) and record it.
  - Confirm no POST reaches a listening server after the confirm is accepted (spec §6.6, §13).
  - Confirm retained cameras stream correctly after re-completing setup through the wizard — the payoff of A1 (spec §13).
  Restore the device to its prior state afterward (branch/data cleaned), exactly as the slice-b Jetson gate did.
  **`192.168.1.81` is NOT touched by any step here or in CI.**

- [ ] **Step 8: Commit (docs + any final test wiring)**

```bash
git add CLAUDE.md AGENTS.md docs/ARCHITECTURE.md tests/CMakeLists.txt tests/
git commit -m "docs+test(mode): document operating modes; full validation"
```

**Success criteria:** every spec §12 acceptance criterion has a passing test; `.15` gate green; docs updated; `.81` untouched.

**Failure/rollback behavior:** validation-only. A failure here sends the specific slice back for a fix commit; it does not roll back merged slices.

**Explicitly unchanged behavior:** packaging pipeline (no packaging file changes ⇒ no `.deb`/preflight/bundle work and no `tests/packaging/run.sh` requirement).

**Proposed commit boundary:** one commit (docs + test wiring); the Jetson gate is a validation step recorded in the progress ledger, not a code commit.

---

## Cross-slice invariants (review these against every slice)

1. **Shared camera fields are never deleted or overwritten** — only `setup_complete`/`areas_need_review` reset; the reset UPDATEs, never deletes, `camera`; the 18 connection/capture columns are asserted field-by-field (Slice 3).
2. **No production stream before setup completion** — for `digit_reader`, `runtime()` (`active AND setup_complete`) is the admission gate; Slice 3 zeroes `setup_complete`; Slice 4 tears down without re-querying `runtime()`; the wizard snapshot is not a `CameraStream`. **`ball_leveler` is an additional HARD gate (Slice 6): `CameraView::reload()` never builds the grid in that mode regardless of `runtime()`**, proven by the construction-counter regression (§12.14). Mode + camera config together decide the pipeline (spec §3.5).
3. **No stale callback crosses a grid generation** — Slice 0 generation guard on both `WorkerFailedFn` and `status_changed`.
4. **Reporting never survives or auto-re-enables** — `brazing.enabled='0'` written in-transaction (Slice 3); rebuilt grid gates on `bcfg.enabled` and builds no reporter; re-enable is an explicit operator action.
5. **UI mode never changes before the DB commit** — Slice 7 updates `current_mode_` only on `r.ok`; on failure it re-reads `mode::load(db_)`.
6. **Rollback coverage is total** — every statement + commit checked; failure-injection tests at each statement assert mode + reporting flag + camera flags revert together (Slice 3).
7. **Slices stay small and single-purpose** — one lifecycle concern per slice; Slice 0 is isolated and merge-gated.
8. **One teardown implementation** — Slice 4 delegates to `CameraGrid::clear()`; nothing re-implements the sequence.
9. **No Floating Ball algorithm** — `ball_leveler` only persists the mode + lands on the unavailable state; no processor/reporter/wizard is built for it.
10. **`192.168.1.81` is never accessed** — validation is `.15` only.

## Self-review (spec coverage map)

| Spec acceptance (§12) | Slice(s) |
|---|---|
| 1 identical no-switch behavior; ctest unchanged; v13 | 1, 8 |
| 2 absent/corrupt ⇒ digit_reader | 1 |
| 3 Reset-to-defaults leaves mode | 1 |
| 4 camera rows/id/columns preserved | 3 |
| 5 five tables destroyed; two flags reset | 3 |
| 6 runtime() empty incl. active=1 | 3 |
| 7 brazing off, url kept, no reporter built | 3, 7 |
| 8 zero orphans | 3 |
| 9 all flags revert on failure | 3 |
| 10 no transport after teardown; no reporter state survives | 4, 7, 8 |
| 11 threads joined before reporter destroyed | 4, 8 |
| 12 retained-connection state + status.json mode | 2, 6, 7 |
| 13 DB-blocker omits mode fields | 2 |
| 14 ball_leveler builds nothing, no setup action | 6 |
| 15 stale worker failure does not inhibit new gen | 0, 8 |
| 16 orphan class rows removed | 3 |
| 17 rollback re-reads in-memory mode | 7 |
| 18 no CameraStream between confirm and commit | 4, 7 |

No spec section is left without a task. Risks R1 (measured, §8), R3/R5/R6 (documented limitations), R4 (discard logging, Slice 7/8), R7/R8 (Slice 0) are all accounted for.

---

**Status:** APPROVED — Stage-2 implementation plan. Ready for sliced implementation; source not yet implemented.
