# Per-camera ONNX Detection Overlay — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Run per-camera YOLOv8 detection on the live grid and draw labeled boxes on each tile, where a camera attaches 1..N models and picks per-class confidence thresholds.

**Architecture:** Detection config is domain data in `denso_core` (`src/core/detection/`, Qt/OpenCV-free, unit-tested). The inference runtime (ONNX Runtime + OpenCV) lives in the app layer under `src/app/ui/camera/shared/detection/` and plugs into the existing `FrameProcessor` seam as a new `DetectionProcessor`, selected per camera at the one construction point in `camera_grid.cpp`. Inference uses ONNX Runtime with an execution-provider fallback chain (TensorRT → CUDA → CPU).

**Tech Stack:** C++20, Qt6 (Core/Sql/Widgets), OpenCV (core + dnn), ONNX Runtime 1.27 GPU (`third_party/onnxruntime/`), Catch2 v3, CMake + Ninja, MSYS2 UCRT64 (MinGW GCC 15.2).

## Global Constraints

- Spec: `docs/superpowers/specs/2026-07-01-per-camera-detection-design.md`.
- Build/test on MSYS2 UCRT64: `export PATH=/c/msys64/ucrt64/bin:$PATH` before every cmake/ctest call.
- Configure: `cmake -S . -B build -G Ninja` · Build: `cmake --build build` · Test: `ctest --test-dir build`.
- `denso_core` links **only** `Qt6::Core`/`Qt6::Sql` — never OpenCV, ORT, or Qt6::Widgets. Detection *domain* goes in core; detection *runtime* (OpenCV/ORT) goes in the app target.
- Core domain type file naming rule `<module>/<module>.h` is already in place: `src/core/detection/detection.h` (namespace `denso::detection`) exists with the structs.
- Persistence = one SQLite file, version-gated migrations in `db::run_migrations`; **add** a migration, never edit a shipped one. Current `SCHEMA_VERSION = 7`.
- `camera` table's USB index column is `cam_index` (SQL keyword clash); FK target is `camera(id)`.
- Model file: `models/denso.onnx` (input `images` 1×3×640×640, output `output0` 1×14×8400 = 4 box + 10 classes, `nms=False`, class names `0`–`9` in ONNX metadata key `names`). Commit `denso.onnx`; git-ignore `third_party/onnxruntime/`.
- ORT DLLs must sit beside the exe at runtime (POST_BUILD copy).
- The file renames (`camera/model.h`→`camera/camera.h`, `processor/`→`detection/`) are already DONE and committed-green (96 tests). This plan starts at the domain layer.

---

## File Structure

**Core (denso_core) — new:**
- `src/core/detection/detection.h` — types (EXISTS; verified in Task 1).
- `src/core/detection/class_names.{h,cpp}` — serialize `vector<string>` ↔ TEXT (JSON array).
- `src/core/detection/repo.{h,cpp}` — persistence + the resolve query.
- `src/core/db/db.cpp` — migration v8 (MODIFY).
- `src/core/CMakeLists.txt` — add sources (MODIFY).

**App (denso) — new under `src/app/ui/camera/shared/detection/`:**
- `letterbox.{h,cpp}` — pure resize+pad to 640 and inverse box map.
- `yolo_decode.{h,cpp}` — pure decode of `output0` → `Detection[]` (argmax + conf floor + NMS).
- `names_metadata.{h,cpp}` — pure parse of the ORT `names` dict string → `vector<string>`.
- `inference_engine.h` — `InferenceEngine` interface + `Detection` struct.
- `ort_engine.{h,cpp}` — ORT session + EP fallback + pre/post-process.
- `engine_registry.{h,cpp}` — one shared engine per model filename.
- `model_sync.{h,cpp}` — scan `models/*.onnx`, read names, upsert catalog.

**App — modified:**
- `src/app/ui/camera/shared/frame_convert.h` (+ `frame_convert.cpp` new) — add `qimage_to_mat`.
- `src/app/ui/camera/grid/frame_processor.{h,cpp}` — add `DetectionProcessor`.
- `src/app/ui/camera/grid/camera_grid.cpp` — branch to `DetectionProcessor`.
- `src/app/ui/camera/dialog/models_page.{h,cpp}` (new) — "Models" wizard page.
- `src/app/ui/camera/dialog/wizard_stepper.cpp` — add the Models step.
- `src/app/ui/camera/camera_dialog.{h,cpp}` — wizard nav + persistence.
- `src/app/main.cpp` — call `model_sync` after migrations.
- `src/app/CMakeLists.txt`, top-level `CMakeLists.txt`, `.gitignore` — ORT wiring + DLL/model copy.

**Tests — new files added to `tests/CMakeLists.txt`:**
- `test_class_names.cpp`, `test_detection_repo.cpp`, `test_letterbox.cpp`, `test_yolo_decode.cpp`, `test_names_metadata.cpp`, `test_frame_convert.cpp`.

---

## Task 1: Verify `detection.h` and add the DB migration (v8)

**Files:**
- Verify: `src/core/detection/detection.h`
- Modify: `src/core/db/db.cpp` (bump `SCHEMA_VERSION`, add `version < 8` block)
- Test: `tests/test_detection_repo.cpp` (first test just proves the tables exist via a raw query)
- Modify: `tests/CMakeLists.txt` (add the test file)

**Interfaces:**
- Produces: three tables — `model(id, name, filename UNIQUE, class_names)`, `camera_model(id, camera_id, model_id)`, `camera_model_class(id, camera_model_id, class_id, conf)`.

- [ ] **Step 1: Confirm `detection.h` content**

Open `src/core/detection/detection.h` and confirm it declares, in `namespace denso::detection`: `DetectionModel{int64_t id; std::string name; std::string filename; std::vector<std::string> class_names;}`, `ModelClassSelection{int class_id; float conf;}`, `CameraModel{int64_t id; int64_t camera_id; int64_t model_id; std::vector<ModelClassSelection> classes;}`, `ResolvedModel{std::string filename; std::vector<std::string> class_names; std::vector<ModelClassSelection> classes;}`, `CameraDetection{int64_t camera_id; std::vector<ResolvedModel> models;}`. If any is missing, add it. (It was created during the rename step.)

- [ ] **Step 2: Write the failing test**

Create `tests/test_detection_repo.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>

#include "db/db.h"

#include <QSqlDatabase>
#include <QSqlQuery>
#include <QString>

using denso::db::Db;
using denso::db::run_migrations;

namespace {
Db mem() {
    auto d = Db::open_in_memory();
    REQUIRE(d.has_value());
    REQUIRE(run_migrations(d->handle()));
    return std::move(*d);
}
bool has_table(const QSqlDatabase& db, const char* name) {
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?"));
    q.addBindValue(QString::fromLatin1(name));
    return q.exec() && q.next();
}
} // namespace

TEST_CASE("migration v8 creates the detection tables") {
    auto d = mem();
    REQUIRE(has_table(d.handle(), "model"));
    REQUIRE(has_table(d.handle(), "camera_model"));
    REQUIRE(has_table(d.handle(), "camera_model_class"));
}
```

Add `test_detection_repo.cpp` to the `add_executable(denso_tests ...)` list in `tests/CMakeLists.txt` (after `test_camera_repo.cpp`).

- [ ] **Step 3: Run test to verify it fails**

Run: `export PATH=/c/msys64/ucrt64/bin:$PATH && cmake --build build && ctest --test-dir build -R "detection tables" -V`
Expected: FAIL — tables `model`/`camera_model`/`camera_model_class` do not exist.

- [ ] **Step 4: Add the migration**

In `src/core/db/db.cpp`: change `constexpr int SCHEMA_VERSION = 7;` to `= 8;`. Then, immediately before the `// PRAGMA can't be parameterized` comment near the end of `run_migrations`, add:

```cpp
    if (version < 8) {
        // Per-camera detection config. `model` is the catalog of .onnx files
        // discovered under models/ (class_names cached from ONNX metadata as a
        // JSON array). A camera attaches 1..N models via `camera_model`; each
        // attachment keeps a subset of classes with a per-class confidence in
        // `camera_model_class`. Detection runs for a camera iff it has ≥1 row
        // in camera_model.
        if (!run("CREATE TABLE IF NOT EXISTS model ("
                 "    id          INTEGER PRIMARY KEY,"
                 "    name        TEXT    NOT NULL,"
                 "    filename    TEXT    NOT NULL UNIQUE,"
                 "    class_names TEXT    NOT NULL"
                 ")")) {
            return false;
        }
        if (!run("CREATE TABLE IF NOT EXISTS camera_model ("
                 "    id        INTEGER PRIMARY KEY,"
                 "    camera_id INTEGER NOT NULL REFERENCES camera(id),"
                 "    model_id  INTEGER NOT NULL REFERENCES model(id)"
                 ")")) {
            return false;
        }
        if (!run("CREATE INDEX IF NOT EXISTS idx_camera_model_camera "
                 "ON camera_model(camera_id)")) {
            return false;
        }
        if (!run("CREATE TABLE IF NOT EXISTS camera_model_class ("
                 "    id              INTEGER PRIMARY KEY,"
                 "    camera_model_id INTEGER NOT NULL REFERENCES camera_model(id),"
                 "    class_id        INTEGER NOT NULL,"
                 "    conf            REAL    NOT NULL"
                 ")")) {
            return false;
        }
        if (!run("CREATE INDEX IF NOT EXISTS idx_cmc_camera_model "
                 "ON camera_model_class(camera_model_id)")) {
            return false;
        }
    }
```

- [ ] **Step 5: Run test to verify it passes**

Run: `cmake --build build && ctest --test-dir build -R "detection tables" -V`
Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add src/core/db/db.cpp tests/test_detection_repo.cpp tests/CMakeLists.txt
git commit -m "feat(detection): add model/camera_model/camera_model_class schema (migration v8)"
```

---

## Task 2: `class_names` serialization helper

**Files:**
- Create: `src/core/detection/class_names.{h,cpp}`
- Modify: `src/core/CMakeLists.txt` (add `detection/class_names.cpp`)
- Test: `tests/test_class_names.cpp` + add to `tests/CMakeLists.txt`

**Interfaces:**
- Produces: `std::string denso::detection::serialize_class_names(const std::vector<std::string>&)`; `std::vector<std::string> denso::detection::parse_class_names(const std::string&)`. Round-trips via a JSON array (handles names with commas/spaces safely).

- [ ] **Step 1: Write the failing test**

Create `tests/test_class_names.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>

#include "detection/class_names.h"

using denso::detection::parse_class_names;
using denso::detection::serialize_class_names;

TEST_CASE("class names round-trip through serialize/parse") {
    std::vector<std::string> names = {"0", "1", "person", "hard hat"};
    const std::string text = serialize_class_names(names);
    REQUIRE(parse_class_names(text) == names);
}

TEST_CASE("empty class names round-trip to empty") {
    REQUIRE(serialize_class_names({}).size() >= 0);
    REQUIRE(parse_class_names(serialize_class_names({})).empty());
}

TEST_CASE("parse tolerates blank text") {
    REQUIRE(parse_class_names("").empty());
    REQUIRE(parse_class_names("not json").empty());
}
```

Add `test_class_names.cpp` to `tests/CMakeLists.txt`.

- [ ] **Step 2: Run to verify it fails**

Run: `cmake -S . -B build -G Ninja && cmake --build build`
Expected: FAIL to compile — `detection/class_names.h` not found.

- [ ] **Step 3: Implement**

Create `src/core/detection/class_names.h`:

```cpp
// Serialize a model's class-name list to / from the single TEXT column
// `model.class_names`. Stored as a JSON array so names containing commas,
// spaces, or other delimiters round-trip safely. Parsing is tolerant: blank or
// malformed text yields an empty list rather than throwing.
#pragma once

#include <string>
#include <vector>

namespace denso::detection {

std::string serialize_class_names(const std::vector<std::string>& names);
std::vector<std::string> parse_class_names(const std::string& text);

} // namespace denso::detection
```

Create `src/core/detection/class_names.cpp`:

```cpp
#include "detection/class_names.h"

#include <QByteArray>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>
#include <QString>

namespace denso::detection {

std::string serialize_class_names(const std::vector<std::string>& names) {
    QJsonArray arr;
    for (const std::string& n : names) {
        arr.append(QString::fromStdString(n));
    }
    return QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact))
        .toStdString();
}

std::vector<std::string> parse_class_names(const std::string& text) {
    std::vector<std::string> out;
    const QJsonDocument doc =
        QJsonDocument::fromJson(QByteArray::fromStdString(text));
    if (!doc.isArray()) {
        return out;
    }
    const QJsonArray arr = doc.array();
    out.reserve(static_cast<size_t>(arr.size()));
    for (const QJsonValue& v : arr) {
        out.push_back(v.toString().toStdString());
    }
    return out;
}

} // namespace denso::detection
```

Add `detection/class_names.cpp` to the `add_library(denso_core STATIC ...)` list in `src/core/CMakeLists.txt`.

- [ ] **Step 4: Run to verify it passes**

Run: `cmake -S . -B build -G Ninja && cmake --build build && ctest --test-dir build -R "class names"`
Expected: PASS (3 tests).

- [ ] **Step 5: Commit**

```bash
git add src/core/detection/class_names.h src/core/detection/class_names.cpp src/core/CMakeLists.txt tests/test_class_names.cpp tests/CMakeLists.txt
git commit -m "feat(detection): class_names JSON serialization helper"
```

---

## Task 3: `detection::repo` — model catalog (`upsert_model`, `list_models`)

**Files:**
- Create: `src/core/detection/repo.h`, `src/core/detection/repo.cpp`
- Modify: `src/core/CMakeLists.txt` (add `detection/repo.cpp`)
- Test: `tests/test_detection_repo.cpp` (append)

**Interfaces:**
- Consumes: `detection::DetectionModel`, `serialize_class_names`/`parse_class_names`.
- Produces:
  - `std::optional<int64_t> denso::detection::upsert_model(const QSqlDatabase&, const DetectionModel&)` — insert by unique `filename`, or update `name`+`class_names` if the filename already exists; returns the row id.
  - `std::vector<DetectionModel> denso::detection::list_models(const QSqlDatabase&)` — ordered by id.

- [ ] **Step 1: Write the failing test** (append to `tests/test_detection_repo.cpp`)

```cpp
#include "detection/detection.h"
#include "detection/repo.h"

using denso::detection::DetectionModel;
using denso::detection::list_models;
using denso::detection::upsert_model;

TEST_CASE("upsert_model inserts and list_models returns it") {
    auto d = mem();
    DetectionModel m;
    m.name = "denso";
    m.filename = "denso.onnx";
    m.class_names = {"0", "1", "2"};
    const auto id = upsert_model(d.handle(), m);
    REQUIRE(id.has_value());

    const auto models = list_models(d.handle());
    REQUIRE(models.size() == 1);
    REQUIRE(models[0].id == *id);
    REQUIRE(models[0].name == "denso");
    REQUIRE(models[0].filename == "denso.onnx");
    REQUIRE(models[0].class_names == std::vector<std::string>{"0", "1", "2"});
}

TEST_CASE("upsert_model updates by filename without adding a row") {
    auto d = mem();
    DetectionModel m;
    m.name = "old";
    m.filename = "denso.onnx";
    m.class_names = {"0"};
    const auto id1 = upsert_model(d.handle(), m);
    m.name = "new";
    m.class_names = {"0", "1"};
    const auto id2 = upsert_model(d.handle(), m);
    REQUIRE(id1 == id2);
    const auto models = list_models(d.handle());
    REQUIRE(models.size() == 1);
    REQUIRE(models[0].name == "new");
    REQUIRE(models[0].class_names.size() == 2);
}
```

- [ ] **Step 2: Run to verify it fails**

Run: `cmake --build build`
Expected: FAIL to compile — `detection/repo.h` not found.

- [ ] **Step 3: Implement `repo.h`**

Create `src/core/detection/repo.h`:

```cpp
// Persistence for the detection model catalog and each camera's attached
// models. Mirrors camera/repo: typed structs in C++, rows in SQLite; write
// failures surface as bool / nullopt. `detection_for` resolves the runtime
// bundle (CameraDetection) a DetectionProcessor consumes.
#pragma once

#include "detection/detection.h"

#include <QSqlDatabase>

#include <cstdint>
#include <optional>
#include <vector>

namespace denso::detection {

// ─── model catalog ───────────────────────────────────────────────────────────

/// Insert a model by unique filename, or update its name + class_names if the
/// filename already exists. Returns the row id, or nullopt on a write error.
std::optional<int64_t> upsert_model(const QSqlDatabase& db, const DetectionModel& m);

/// Every catalog model, ordered by id.
std::vector<DetectionModel> list_models(const QSqlDatabase& db);

// ─── per-camera attachments ──────────────────────────────────────────────────

/// The models attached to a camera with their class selections, ordered by id.
std::vector<CameraModel> models_for(const QSqlDatabase& db, int64_t camera_id);

/// Replace a camera's entire attached-model set (+ class selections) in one
/// transaction. Empty clears it. Returns false on error (rolled back).
bool set_camera_models(const QSqlDatabase& db, int64_t camera_id,
                       const std::vector<CameraModel>& models);

/// Resolve a camera's detection config for the runtime: each attached model
/// joined with its filename + class_names. Empty `models` == no detection.
CameraDetection detection_for(const QSqlDatabase& db, int64_t camera_id);

} // namespace denso::detection
```

- [ ] **Step 4: Implement `repo.cpp` (catalog half)**

Create `src/core/detection/repo.cpp`:

```cpp
#include "detection/repo.h"

#include "detection/class_names.h"

#include <QSqlQuery>
#include <QString>
#include <QVariant>

#include <utility>

namespace denso::detection {

std::optional<int64_t> upsert_model(const QSqlDatabase& db, const DetectionModel& m) {
    QSqlQuery q(db);
    // UPSERT on the unique filename; RETURNING gives the row id for both paths.
    q.prepare(QStringLiteral(
        "INSERT INTO model (name, filename, class_names) VALUES (?, ?, ?) "
        "ON CONFLICT(filename) DO UPDATE SET name=excluded.name, "
        "class_names=excluded.class_names "
        "RETURNING id"));
    q.addBindValue(QString::fromStdString(m.name));
    q.addBindValue(QString::fromStdString(m.filename));
    q.addBindValue(QString::fromStdString(serialize_class_names(m.class_names)));
    if (!q.exec() || !q.next()) {
        return std::nullopt;
    }
    return q.value(0).toLongLong();
}

std::vector<DetectionModel> list_models(const QSqlDatabase& db) {
    std::vector<DetectionModel> out;
    QSqlQuery q(db);
    if (!q.exec(QStringLiteral(
            "SELECT id, name, filename, class_names FROM model ORDER BY id"))) {
        return out;
    }
    while (q.next()) {
        DetectionModel m;
        m.id = q.value(0).toLongLong();
        m.name = q.value(1).toString().toStdString();
        m.filename = q.value(2).toString().toStdString();
        m.class_names = parse_class_names(q.value(3).toString().toStdString());
        out.push_back(std::move(m));
    }
    return out;
}

} // namespace denso::detection
```

Add `detection/repo.cpp` to `add_library(denso_core STATIC ...)` in `src/core/CMakeLists.txt`.

*(Note: SQLite `ON CONFLICT ... RETURNING` requires SQLite ≥ 3.35; QSQLITE bundled with Qt6 is newer. If `RETURNING` fails at runtime, fall back to a `SELECT id FROM model WHERE filename=?` after the upsert.)*

- [ ] **Step 5: Run to verify it passes**

Run: `cmake -S . -B build -G Ninja && cmake --build build && ctest --test-dir build -R "upsert_model|list_models"`
Expected: PASS (2 tests).

- [ ] **Step 6: Commit**

```bash
git add src/core/detection/repo.h src/core/detection/repo.cpp src/core/CMakeLists.txt tests/test_detection_repo.cpp
git commit -m "feat(detection): repo model catalog (upsert_model, list_models)"
```

---

## Task 4: `detection::repo` — attachments (`models_for`, `set_camera_models`, `detection_for`)

**Files:**
- Modify: `src/core/detection/repo.cpp` (append the three functions)
- Test: `tests/test_detection_repo.cpp` (append)

**Interfaces:**
- Consumes: `upsert_model` (to seed a model), `camera::insert` (to seed a camera).
- Produces: `models_for`, `set_camera_models`, `detection_for` as declared in `repo.h`.

- [ ] **Step 1: Write the failing test** (append)

```cpp
#include "camera/camera.h"
#include "camera/repo.h"
#include "detection/repo.h"

using denso::detection::CameraModel;
using denso::detection::detection_for;
using denso::detection::models_for;
using denso::detection::ModelClassSelection;
using denso::detection::set_camera_models;

namespace {
int64_t seed_camera(const QSqlDatabase& db) {
    denso::camera::Camera c;
    c.name = "Cam";
    c.camera_type = "usb";
    c.active = true;
    c.index = 0;
    c.width = 640; c.height = 480; c.fps = 30;
    return *denso::camera::insert(db, c);
}
int64_t seed_model(const QSqlDatabase& db) {
    denso::detection::DetectionModel m;
    m.name = "denso"; m.filename = "denso.onnx";
    m.class_names = {"0", "1", "2", "3"};
    return *upsert_model(db, m);
}
} // namespace

TEST_CASE("set_camera_models + models_for round-trip attachments and classes") {
    auto d = mem();
    const int64_t cam = seed_camera(d.handle());
    const int64_t model = seed_model(d.handle());

    CameraModel cm;
    cm.camera_id = cam;
    cm.model_id = model;
    cm.classes = {ModelClassSelection{1, 0.6f}, ModelClassSelection{3, 0.4f}};
    REQUIRE(set_camera_models(d.handle(), cam, {cm}));

    const auto got = models_for(d.handle(), cam);
    REQUIRE(got.size() == 1);
    REQUIRE(got[0].model_id == model);
    REQUIRE(got[0].classes.size() == 2);
    REQUIRE(got[0].classes[0].class_id == 1);
    REQUIRE(got[0].classes[0].conf == 0.6f);
    REQUIRE(got[0].classes[1].class_id == 3);
}

TEST_CASE("set_camera_models replaces the previous set") {
    auto d = mem();
    const int64_t cam = seed_camera(d.handle());
    const int64_t model = seed_model(d.handle());
    CameraModel cm; cm.camera_id = cam; cm.model_id = model;
    cm.classes = {ModelClassSelection{0, 0.5f}};
    REQUIRE(set_camera_models(d.handle(), cam, {cm, cm}));
    REQUIRE(models_for(d.handle(), cam).size() == 2);
    REQUIRE(set_camera_models(d.handle(), cam, {}));
    REQUIRE(models_for(d.handle(), cam).empty());
}

TEST_CASE("detection_for resolves filename + class_names from the model") {
    auto d = mem();
    const int64_t cam = seed_camera(d.handle());
    const int64_t model = seed_model(d.handle());
    CameraModel cm; cm.camera_id = cam; cm.model_id = model;
    cm.classes = {ModelClassSelection{2, 0.7f}};
    REQUIRE(set_camera_models(d.handle(), cam, {cm}));

    const auto det = detection_for(d.handle(), cam);
    REQUIRE(det.camera_id == cam);
    REQUIRE(det.models.size() == 1);
    REQUIRE(det.models[0].filename == "denso.onnx");
    REQUIRE(det.models[0].class_names.size() == 4);
    REQUIRE(det.models[0].classes.size() == 1);
    REQUIRE(det.models[0].classes[0].class_id == 2);
    REQUIRE(det.models[0].classes[0].conf == 0.7f);
}

TEST_CASE("detection_for is empty for a camera with no models") {
    auto d = mem();
    const int64_t cam = seed_camera(d.handle());
    REQUIRE(detection_for(d.handle(), cam).models.empty());
}
```

- [ ] **Step 2: Run to verify it fails**

Run: `cmake --build build`
Expected: FAIL to compile — `models_for`/`set_camera_models`/`detection_for` undefined.

- [ ] **Step 3: Implement (append to `repo.cpp`, before the closing namespace)**

```cpp
static std::vector<ModelClassSelection> classes_for(const QSqlDatabase& db,
                                                    int64_t camera_model_id) {
    std::vector<ModelClassSelection> out;
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "SELECT class_id, conf FROM camera_model_class "
        "WHERE camera_model_id = ? ORDER BY id"));
    q.addBindValue(static_cast<qlonglong>(camera_model_id));
    if (!q.exec()) {
        return out;
    }
    while (q.next()) {
        out.push_back(ModelClassSelection{q.value(0).toInt(),
                                          q.value(1).toFloat()});
    }
    return out;
}

std::vector<CameraModel> models_for(const QSqlDatabase& db, int64_t camera_id) {
    std::vector<CameraModel> out;
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "SELECT id, camera_id, model_id FROM camera_model "
        "WHERE camera_id = ? ORDER BY id"));
    q.addBindValue(static_cast<qlonglong>(camera_id));
    if (!q.exec()) {
        return out;
    }
    while (q.next()) {
        CameraModel cm;
        cm.id = q.value(0).toLongLong();
        cm.camera_id = q.value(1).toLongLong();
        cm.model_id = q.value(2).toLongLong();
        cm.classes = classes_for(db, cm.id);
        out.push_back(std::move(cm));
    }
    return out;
}

bool set_camera_models(const QSqlDatabase& db, int64_t camera_id,
                       const std::vector<CameraModel>& models) {
    QSqlDatabase conn(db);
    if (!conn.transaction()) {
        return false;
    }
    const auto rollback = [&conn] { conn.rollback(); return false; };

    // Delete children first (class rows for this camera's attachments), then
    // the attachments themselves.
    QSqlQuery delc(db);
    delc.prepare(QStringLiteral(
        "DELETE FROM camera_model_class WHERE camera_model_id IN "
        "(SELECT id FROM camera_model WHERE camera_id = ?)"));
    delc.addBindValue(static_cast<qlonglong>(camera_id));
    if (!delc.exec()) {
        return rollback();
    }
    QSqlQuery delm(db);
    delm.prepare(QStringLiteral("DELETE FROM camera_model WHERE camera_id = ?"));
    delm.addBindValue(static_cast<qlonglong>(camera_id));
    if (!delm.exec()) {
        return rollback();
    }

    for (const CameraModel& cm : models) {
        QSqlQuery ins(db);
        ins.prepare(QStringLiteral(
            "INSERT INTO camera_model (camera_id, model_id) VALUES (?, ?)"));
        ins.addBindValue(static_cast<qlonglong>(camera_id));
        ins.addBindValue(static_cast<qlonglong>(cm.model_id));
        if (!ins.exec()) {
            return rollback();
        }
        const qlonglong cmid = ins.lastInsertId().toLongLong();
        for (const ModelClassSelection& s : cm.classes) {
            QSqlQuery insc(db);
            insc.prepare(QStringLiteral(
                "INSERT INTO camera_model_class (camera_model_id, class_id, conf) "
                "VALUES (?, ?, ?)"));
            insc.addBindValue(cmid);
            insc.addBindValue(s.class_id);
            insc.addBindValue(static_cast<double>(s.conf));
            if (!insc.exec()) {
                return rollback();
            }
        }
    }
    return conn.commit() || rollback();
}

CameraDetection detection_for(const QSqlDatabase& db, int64_t camera_id) {
    CameraDetection out;
    out.camera_id = camera_id;
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "SELECT cm.id, m.filename, m.class_names "
        "FROM camera_model cm JOIN model m ON m.id = cm.model_id "
        "WHERE cm.camera_id = ? ORDER BY cm.id"));
    q.addBindValue(static_cast<qlonglong>(camera_id));
    if (!q.exec()) {
        return out;
    }
    while (q.next()) {
        ResolvedModel rm;
        const int64_t cmid = q.value(0).toLongLong();
        rm.filename = q.value(1).toString().toStdString();
        rm.class_names = parse_class_names(q.value(2).toString().toStdString());
        rm.classes = classes_for(db, cmid);
        out.models.push_back(std::move(rm));
    }
    return out;
}
```

- [ ] **Step 4: Run to verify it passes**

Run: `cmake --build build && ctest --test-dir build -R "set_camera_models|models_for|detection_for"`
Expected: PASS (4 tests).

- [ ] **Step 5: Commit**

```bash
git add src/core/detection/repo.cpp tests/test_detection_repo.cpp
git commit -m "feat(detection): repo attachments + detection_for resolve query"
```

---

## Task 5: `letterbox` (pure)

**Files:**
- Create: `src/app/ui/camera/shared/detection/letterbox.{h,cpp}`
- Test: `tests/test_letterbox.cpp` + add it AND the source to `tests/CMakeLists.txt`

**Interfaces:**
- Produces:
  - `struct denso::ui::LetterboxInfo { float scale; int pad_x; int pad_y; int size; };`
  - `LetterboxInfo denso::ui::letterbox(const cv::Mat& src, cv::Mat& dst, int size = 640, int pad_value = 114);` — resizes `src` preserving aspect into a `size×size` `dst`, centered with gray padding.
  - `cv::Rect denso::ui::undo_letterbox(float cx, float cy, float w, float h, const LetterboxInfo& lb, int orig_w, int orig_h);` — maps a box in letterboxed 640-space (center form) back to a clamped `cv::Rect` in original-image pixels.

- [ ] **Step 1: Write the failing test**

Create `tests/test_letterbox.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>

#include "ui/camera/shared/detection/letterbox.h"

#include <opencv2/core.hpp>

using denso::ui::letterbox;
using denso::ui::LetterboxInfo;
using denso::ui::undo_letterbox;

TEST_CASE("letterbox scales the long side to 640 and pads the short side") {
    cv::Mat src(480, 640, CV_8UC3, cv::Scalar(0, 0, 0));  // 640x480 landscape
    cv::Mat dst;
    const LetterboxInfo lb = letterbox(src, dst, 640);
    REQUIRE(dst.cols == 640);
    REQUIRE(dst.rows == 640);
    REQUIRE(lb.scale == 1.0f);          // 640 long side already fits
    REQUIRE(lb.pad_x == 0);
    REQUIRE(lb.pad_y == 80);            // (640-480)/2
}

TEST_CASE("undo_letterbox inverts the mapping to original pixels") {
    cv::Mat src(480, 640, CV_8UC3, cv::Scalar(0, 0, 0));
    cv::Mat dst;
    const LetterboxInfo lb = letterbox(src, dst, 640);
    // A box centered in the padded image at (320, 320) size 64x64 maps back to
    // original center (320, 240).
    const cv::Rect r = undo_letterbox(320.f, 320.f, 64.f, 64.f, lb, 640, 480);
    REQUIRE(r.x == 288);   // 320 - 32
    REQUIRE(r.y == 208);   // (320-80) - 32 = 208
    REQUIRE(r.width == 64);
    REQUIRE(r.height == 64);
}
```

Add to `tests/CMakeLists.txt`: `test_letterbox.cpp` and `${CMAKE_SOURCE_DIR}/src/app/ui/camera/shared/detection/letterbox.cpp` in the `denso_tests` sources.

- [ ] **Step 2: Run to verify it fails**

Run: `cmake -S . -B build -G Ninja && cmake --build build`
Expected: FAIL to compile — header missing.

- [ ] **Step 3: Implement**

Create `src/app/ui/camera/shared/detection/letterbox.h`:

```cpp
// Letterbox a frame into the square input a YOLO ONNX model expects: resize
// preserving aspect ratio, then pad the short side with gray to size×size. The
// returned LetterboxInfo carries the scale + padding needed to map detection
// boxes back to original-image pixels. Pure (OpenCV only) — unit-tested.
#pragma once

#include <opencv2/core.hpp>

namespace denso::ui {

struct LetterboxInfo {
    float scale = 1.0f;  // original → letterboxed resize factor
    int pad_x = 0;       // left padding in the letterboxed image
    int pad_y = 0;       // top padding
    int size = 640;      // output square size
};

LetterboxInfo letterbox(const cv::Mat& src, cv::Mat& dst, int size = 640,
                        int pad_value = 114);

cv::Rect undo_letterbox(float cx, float cy, float w, float h,
                        const LetterboxInfo& lb, int orig_w, int orig_h);

} // namespace denso::ui
```

Create `src/app/ui/camera/shared/detection/letterbox.cpp`:

```cpp
#include "ui/camera/shared/detection/letterbox.h"

#include <opencv2/imgproc.hpp>

#include <algorithm>

namespace denso::ui {

LetterboxInfo letterbox(const cv::Mat& src, cv::Mat& dst, int size,
                        int pad_value) {
    LetterboxInfo lb;
    lb.size = size;
    const float scale = std::min(static_cast<float>(size) / src.cols,
                                 static_cast<float>(size) / src.rows);
    lb.scale = scale;
    const int new_w = static_cast<int>(std::round(src.cols * scale));
    const int new_h = static_cast<int>(std::round(src.rows * scale));
    lb.pad_x = (size - new_w) / 2;
    lb.pad_y = (size - new_h) / 2;

    cv::Mat resized;
    cv::resize(src, resized, cv::Size(new_w, new_h), 0, 0, cv::INTER_LINEAR);
    dst = cv::Mat(size, size, src.type(), cv::Scalar::all(pad_value));
    resized.copyTo(dst(cv::Rect(lb.pad_x, lb.pad_y, new_w, new_h)));
    return lb;
}

cv::Rect undo_letterbox(float cx, float cy, float w, float h,
                        const LetterboxInfo& lb, int orig_w, int orig_h) {
    // De-pad, then de-scale back to original pixels.
    const float x1 = (cx - w / 2.0f - lb.pad_x) / lb.scale;
    const float y1 = (cy - h / 2.0f - lb.pad_y) / lb.scale;
    const float x2 = (cx + w / 2.0f - lb.pad_x) / lb.scale;
    const float y2 = (cy + h / 2.0f - lb.pad_y) / lb.scale;
    int ix1 = std::clamp(static_cast<int>(std::round(x1)), 0, orig_w);
    int iy1 = std::clamp(static_cast<int>(std::round(y1)), 0, orig_h);
    int ix2 = std::clamp(static_cast<int>(std::round(x2)), 0, orig_w);
    int iy2 = std::clamp(static_cast<int>(std::round(y2)), 0, orig_h);
    return cv::Rect(ix1, iy1, std::max(0, ix2 - ix1), std::max(0, iy2 - iy1));
}

} // namespace denso::ui
```

- [ ] **Step 4: Run to verify it passes**

Run: `cmake -S . -B build -G Ninja && cmake --build build && ctest --test-dir build -R letterbox`
Expected: PASS (2 tests).

- [ ] **Step 5: Commit**

```bash
git add src/app/ui/camera/shared/detection/letterbox.h src/app/ui/camera/shared/detection/letterbox.cpp tests/test_letterbox.cpp tests/CMakeLists.txt
git commit -m "feat(detection): pure letterbox + inverse box mapping"
```

---

## Task 6: `yolo_decode` (pure)

**Files:**
- Create: `src/app/ui/camera/shared/detection/inference_engine.h` (defines `Detection`)
- Create: `src/app/ui/camera/shared/detection/yolo_decode.{h,cpp}`
- Test: `tests/test_yolo_decode.cpp` + add it AND `yolo_decode.cpp` to `tests/CMakeLists.txt`

**Interfaces:**
- Produces:
  - `struct denso::ui::Detection { cv::Rect box; int class_id; float conf; };` (in `inference_engine.h`)
  - `std::vector<Detection> denso::ui::decode_yolo(const float* out, int num_classes, int num_anchors, const LetterboxInfo& lb, int orig_w, int orig_h, float conf_floor, float nms_iou);` — decodes the transposed `[1, 4+nc, na]` buffer (`out[c*na + a]`), argmax over classes, keeps `conf ≥ conf_floor`, runs `cv::dnn::NMSBoxes`, and maps boxes back via `undo_letterbox`.

- [ ] **Step 1: Write the failing test**

Create `tests/test_yolo_decode.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>

#include "ui/camera/shared/detection/letterbox.h"
#include "ui/camera/shared/detection/yolo_decode.h"

#include <vector>

using denso::ui::decode_yolo;
using denso::ui::Detection;
using denso::ui::LetterboxInfo;

TEST_CASE("decode_yolo returns the argmax class above the conf floor") {
    // nc=2 classes, na=2 anchors. Layout is [4+nc, na] row-major: row r, col a
    // at index r*na + a. Anchor 0: box (320,320,64,64), class1=0.9. Anchor 1:
    // all-low scores (filtered out).
    const int nc = 2, na = 2;
    std::vector<float> out((4 + nc) * na, 0.0f);
    auto at = [&](int row, int a) -> float& { return out[row * na + a]; };
    at(0, 0) = 320.f; at(1, 0) = 320.f; at(2, 0) = 64.f; at(3, 0) = 64.f;
    at(4, 0) = 0.10f;  // class 0 score
    at(5, 0) = 0.90f;  // class 1 score
    at(4, 1) = 0.05f; at(5, 1) = 0.02f;  // anchor 1 below floor

    LetterboxInfo lb;  // identity: scale 1, no pad, size 640
    lb.scale = 1.0f; lb.pad_x = 0; lb.pad_y = 0; lb.size = 640;

    const auto dets = decode_yolo(out.data(), nc, na, lb, 640, 640, 0.25f, 0.45f);
    REQUIRE(dets.size() == 1);
    REQUIRE(dets[0].class_id == 1);
    REQUIRE(dets[0].conf == 0.90f);
    REQUIRE(dets[0].box.x == 288);   // 320 - 32
    REQUIRE(dets[0].box.width == 64);
}
```

Add `test_yolo_decode.cpp` and `${CMAKE_SOURCE_DIR}/src/app/ui/camera/shared/detection/yolo_decode.cpp` to `tests/CMakeLists.txt`.

- [ ] **Step 2: Run to verify it fails**

Run: `cmake -S . -B build -G Ninja && cmake --build build`
Expected: FAIL — headers missing.

- [ ] **Step 3: Implement `inference_engine.h`**

Create `src/app/ui/camera/shared/detection/inference_engine.h`:

```cpp
// The inference boundary the DetectionProcessor depends on. An InferenceEngine
// turns one BGR frame into detections; the ONNX Runtime implementation lives in
// ort_engine.{h,cpp}. Keeping this an interface means the runtime backend (ORT
// today, a different one later) is swappable without touching the camera/UI.
#pragma once

#include <opencv2/core.hpp>

#include <vector>

namespace denso::ui {

/// One detection: box in original-frame pixels, class id, confidence.
struct Detection {
    cv::Rect box;
    int class_id = 0;
    float conf = 0.0f;
};

class InferenceEngine {
public:
    virtual ~InferenceEngine() = default;

    /// Run the model on one BGR frame; returns all detections above the
    /// engine's internal conf floor (per-class filtering is the caller's job).
    virtual std::vector<Detection> infer(const cv::Mat& bgr) = 0;

    /// Class names indexed by class id (from the model's metadata).
    virtual const std::vector<std::string>& class_names() const = 0;
};

} // namespace denso::ui
```

- [ ] **Step 4: Implement `yolo_decode.{h,cpp}`**

Create `src/app/ui/camera/shared/detection/yolo_decode.h`:

```cpp
// Decode a YOLOv8-detect ONNX output tensor ([1, 4+nc, na], transposed layout)
// into Detections in original-frame pixels: per-anchor argmax over the class
// scores, confidence-floor filter, class-agnostic NMS, and letterbox inverse
// mapping. Pure (OpenCV only) — unit-tested with a synthetic buffer.
#pragma once

#include "ui/camera/shared/detection/inference_engine.h"
#include "ui/camera/shared/detection/letterbox.h"

#include <vector>

namespace denso::ui {

std::vector<Detection> decode_yolo(const float* out, int num_classes,
                                   int num_anchors, const LetterboxInfo& lb,
                                   int orig_w, int orig_h, float conf_floor,
                                   float nms_iou);

} // namespace denso::ui
```

Create `src/app/ui/camera/shared/detection/yolo_decode.cpp`:

```cpp
#include "ui/camera/shared/detection/yolo_decode.h"

#include <opencv2/dnn.hpp>

namespace denso::ui {

std::vector<Detection> decode_yolo(const float* out, int num_classes,
                                   int num_anchors, const LetterboxInfo& lb,
                                   int orig_w, int orig_h, float conf_floor,
                                   float nms_iou) {
    std::vector<cv::Rect> boxes;
    std::vector<float> scores;
    std::vector<int> class_ids;
    boxes.reserve(64);
    scores.reserve(64);
    class_ids.reserve(64);

    // Transposed layout: value at (row r, anchor a) == out[r * num_anchors + a].
    // Rows 0..3 are cx,cy,w,h; rows 4..4+nc-1 are class scores.
    for (int a = 0; a < num_anchors; ++a) {
        int best = -1;
        float best_score = 0.0f;
        for (int k = 0; k < num_classes; ++k) {
            const float s = out[(4 + k) * num_anchors + a];
            if (s > best_score) {
                best_score = s;
                best = k;
            }
        }
        if (best < 0 || best_score < conf_floor) {
            continue;
        }
        const float cx = out[0 * num_anchors + a];
        const float cy = out[1 * num_anchors + a];
        const float w = out[2 * num_anchors + a];
        const float h = out[3 * num_anchors + a];
        boxes.push_back(undo_letterbox(cx, cy, w, h, lb, orig_w, orig_h));
        scores.push_back(best_score);
        class_ids.push_back(best);
    }

    std::vector<int> keep;
    cv::dnn::NMSBoxes(boxes, scores, conf_floor, nms_iou, keep);

    std::vector<Detection> dets;
    dets.reserve(keep.size());
    for (int i : keep) {
        dets.push_back(Detection{boxes[i], class_ids[i], scores[i]});
    }
    return dets;
}

} // namespace denso::ui
```

- [ ] **Step 5: Run to verify it passes**

Run: `cmake -S . -B build -G Ninja && cmake --build build && ctest --test-dir build -R yolo_decode`
Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add src/app/ui/camera/shared/detection/inference_engine.h src/app/ui/camera/shared/detection/yolo_decode.h src/app/ui/camera/shared/detection/yolo_decode.cpp tests/test_yolo_decode.cpp tests/CMakeLists.txt
git commit -m "feat(detection): pure YOLOv8 output decode (argmax + NMS)"
```

---

## Task 7: `names_metadata` parser (pure)

**Files:**
- Create: `src/app/ui/camera/shared/detection/names_metadata.{h,cpp}`
- Test: `tests/test_names_metadata.cpp` + add it AND the source to `tests/CMakeLists.txt`

**Interfaces:**
- Produces: `std::vector<std::string> denso::ui::parse_names_metadata(const std::string& names);` — parses the ORT `names` custom-metadata value, a Python-dict repr like `{0: '0', 1: '1', ...}`, into an index-ordered list. Extracts the quoted values in key order.

- [ ] **Step 1: Write the failing test**

Create `tests/test_names_metadata.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>

#include "ui/camera/shared/detection/names_metadata.h"

using denso::ui::parse_names_metadata;

TEST_CASE("parse_names_metadata extracts values in key order") {
    const std::string meta = "{0: '0', 1: '1', 2: '2', 3: 'person'}";
    const auto names = parse_names_metadata(meta);
    REQUIRE(names == std::vector<std::string>{"0", "1", "2", "person"});
}

TEST_CASE("parse_names_metadata handles double quotes and blanks") {
    REQUIRE(parse_names_metadata("{0: \"a\", 1: \"b\"}")
            == std::vector<std::string>{"a", "b"});
    REQUIRE(parse_names_metadata("").empty());
}
```

Add `test_names_metadata.cpp` and `${CMAKE_SOURCE_DIR}/src/app/ui/camera/shared/detection/names_metadata.cpp` to `tests/CMakeLists.txt`.

- [ ] **Step 2: Run to verify it fails**

Run: `cmake -S . -B build -G Ninja && cmake --build build`
Expected: FAIL — header missing.

- [ ] **Step 3: Implement**

Create `src/app/ui/camera/shared/detection/names_metadata.h`:

```cpp
// Parse the class-name list from a YOLO ONNX model's `names` custom metadata,
// which Ultralytics stores as a Python-dict repr: "{0: '0', 1: '1', ...}". The
// values are extracted in the order they appear (key order). Pure — unit-tested.
#pragma once

#include <string>
#include <vector>

namespace denso::ui {

std::vector<std::string> parse_names_metadata(const std::string& names);

} // namespace denso::ui
```

Create `src/app/ui/camera/shared/detection/names_metadata.cpp`:

```cpp
#include "ui/camera/shared/detection/names_metadata.h"

namespace denso::ui {

std::vector<std::string> parse_names_metadata(const std::string& names) {
    // Extract each quoted substring in order; keys ascend, so appearance order
    // is class-id order. Handles both ' and " quotes.
    std::vector<std::string> out;
    size_t i = 0;
    while (i < names.size()) {
        const char c = names[i];
        if (c == '\'' || c == '"') {
            const char quote = c;
            const size_t start = i + 1;
            size_t end = names.find(quote, start);
            if (end == std::string::npos) {
                break;
            }
            out.push_back(names.substr(start, end - start));
            i = end + 1;
        } else {
            ++i;
        }
    }
    return out;
}

} // namespace denso::ui
```

- [ ] **Step 4: Run to verify it passes**

Run: `cmake -S . -B build -G Ninja && cmake --build build && ctest --test-dir build -R names_metadata`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add src/app/ui/camera/shared/detection/names_metadata.h src/app/ui/camera/shared/detection/names_metadata.cpp tests/test_names_metadata.cpp tests/CMakeLists.txt
git commit -m "feat(detection): parse class names from ONNX metadata"
```

---

## Task 8: Wire ONNX Runtime into CMake + `qimage_to_mat`

**Files:**
- Modify: top-level `CMakeLists.txt` (imported ORT target)
- Modify: `src/app/CMakeLists.txt` (link ORT, POST_BUILD copy, new sources)
- Modify: `.gitignore`
- Create: `src/app/ui/camera/shared/frame_convert.cpp` (implement existing `mat_to_qimage`'s neighbor `qimage_to_mat`)
- Modify: `src/app/ui/camera/shared/frame_convert.h`
- Test: `tests/test_frame_convert.cpp` + add it AND the sources to `tests/CMakeLists.txt`

**Interfaces:**
- Produces: `cv::Mat denso::ui::qimage_to_mat(const QImage& img);` — RGB/ARGB QImage → BGR `CV_8UC3` `cv::Mat` that owns its bytes.

*Note: `mat_to_qimage` currently lives in `snapshot.cpp`. Leave it there; add `qimage_to_mat` in a new `frame_convert.cpp` compiled into both `denso` and `denso_tests`.*

- [ ] **Step 1: Write the failing test**

Create `tests/test_frame_convert.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>

#include "ui/camera/shared/frame_convert.h"

#include <QImage>
#include <opencv2/core.hpp>

using denso::ui::qimage_to_mat;

TEST_CASE("qimage_to_mat converts RGB to BGR CV_8UC3") {
    QImage img(2, 1, QImage::Format_RGB888);
    img.setPixelColor(0, 0, QColor(255, 0, 0));   // red
    img.setPixelColor(1, 0, QColor(0, 0, 255));   // blue
    const cv::Mat m = qimage_to_mat(img);
    REQUIRE(m.cols == 2);
    REQUIRE(m.rows == 1);
    REQUIRE(m.type() == CV_8UC3);
    // OpenCV is BGR: red pixel → (0,0,255)
    REQUIRE(m.at<cv::Vec3b>(0, 0)[0] == 0);
    REQUIRE(m.at<cv::Vec3b>(0, 0)[2] == 255);
    REQUIRE(m.at<cv::Vec3b>(0, 1)[0] == 255);  // blue pixel → B=255
}
```

Add `test_frame_convert.cpp` and `${CMAKE_SOURCE_DIR}/src/app/ui/camera/shared/frame_convert.cpp` to `tests/CMakeLists.txt`.

- [ ] **Step 2: Run to verify it fails**

Run: `cmake -S . -B build -G Ninja && cmake --build build`
Expected: FAIL — `qimage_to_mat` undefined.

- [ ] **Step 3: Add the declaration + implementation**

In `src/app/ui/camera/shared/frame_convert.h`, add after the `mat_to_qimage` declaration:

```cpp
/// RGB/ARGB QImage → BGR cv::Mat (CV_8UC3) that owns its bytes. Null in → empty.
cv::Mat qimage_to_mat(const QImage& img);
```

Create `src/app/ui/camera/shared/frame_convert.cpp`:

```cpp
#include "ui/camera/shared/frame_convert.h"

#include <opencv2/imgproc.hpp>

namespace denso::ui {

cv::Mat qimage_to_mat(const QImage& img) {
    if (img.isNull()) {
        return cv::Mat();
    }
    const QImage rgb = img.convertToFormat(QImage::Format_RGB888);
    cv::Mat view(rgb.height(), rgb.width(), CV_8UC3,
                 const_cast<uchar*>(rgb.bits()),
                 static_cast<size_t>(rgb.bytesPerLine()));
    cv::Mat bgr;
    cv::cvtColor(view, bgr, cv::COLOR_RGB2BGR);  // deep-copies into bgr
    return bgr;
}

} // namespace denso::ui
```

- [ ] **Step 4: Wire ORT into the build**

In the top-level `CMakeLists.txt`, after `find_package(OpenCV REQUIRED)`:

```cmake
# ONNX Runtime (GPU build, provisioned into third_party/). MinGW links the C
# API directly against the MSVC-built DLL (clean C ABI — verified). The GPU
# provider DLLs (CUDA/TensorRT) are loaded by ORT at runtime, not linked.
set(ORT_DIR ${CMAKE_SOURCE_DIR}/third_party/onnxruntime)
add_library(onnxruntime SHARED IMPORTED GLOBAL)
set_target_properties(onnxruntime PROPERTIES
    IMPORTED_LOCATION ${ORT_DIR}/lib/onnxruntime.dll
    IMPORTED_IMPLIB   ${ORT_DIR}/lib/onnxruntime.dll
    INTERFACE_INCLUDE_DIRECTORIES ${ORT_DIR}/include)
```

In `src/app/CMakeLists.txt`, add the new detection sources to `add_executable(denso ...)`:

```cmake
    ui/camera/shared/frame_convert.cpp
    ui/camera/shared/detection/letterbox.cpp
    ui/camera/shared/detection/yolo_decode.cpp
    ui/camera/shared/detection/names_metadata.cpp
    ui/camera/shared/detection/ort_engine.cpp
    ui/camera/shared/detection/engine_registry.cpp
    ui/camera/shared/detection/model_sync.cpp
```

*(ort_engine/engine_registry/model_sync land in later tasks; adding the lines now is fine only once those files exist — add each line in the task that creates the file. For THIS task add only `frame_convert.cpp`.)*

Then link ORT and copy runtime files, at the end of `src/app/CMakeLists.txt`:

```cmake
target_link_libraries(denso PRIVATE onnxruntime)

# ORT runtime DLLs + the model must sit beside the exe (Windows DLL search +
# the app resolves models/denso.onnx relative to the executable dir).
add_custom_command(TARGET denso POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${ORT_DIR}/lib/onnxruntime.dll
        ${ORT_DIR}/lib/onnxruntime_providers_shared.dll
        ${ORT_DIR}/lib/onnxruntime_providers_cuda.dll
        ${ORT_DIR}/lib/onnxruntime_providers_tensorrt.dll
        $<TARGET_FILE_DIR:denso>
    COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:denso>/models
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_SOURCE_DIR}/models/denso.onnx
        $<TARGET_FILE_DIR:denso>/models/denso.onnx
    VERBATIM)
```

In `tests/CMakeLists.txt`, the tests need OpenCV `imgproc` (already linked via `${OpenCV_LIBS}`). No ORT link for tests (pure units only).

Append `third_party/onnxruntime/` to `.gitignore`.

- [ ] **Step 5: Run to verify it passes**

Run: `cmake -S . -B build -G Ninja && cmake --build build && ctest --test-dir build -R frame_convert`
Expected: PASS. Also confirm `denso.exe` still links.

- [ ] **Step 6: Commit**

```bash
git add CMakeLists.txt src/app/CMakeLists.txt .gitignore src/app/ui/camera/shared/frame_convert.h src/app/ui/camera/shared/frame_convert.cpp tests/test_frame_convert.cpp tests/CMakeLists.txt
git commit -m "build(detection): wire ONNX Runtime + qimage_to_mat"
```

---

## Task 9: `OrtEngine` (ONNX Runtime backend)

**Files:**
- Create: `src/app/ui/camera/shared/detection/ort_engine.{h,cpp}`
- Modify: `src/app/CMakeLists.txt` (add `ort_engine.cpp` to `denso` sources — see Task 8 note)

**Interfaces:**
- Consumes: `InferenceEngine`, `Detection`, `letterbox`, `decode_yolo`, `parse_names_metadata`.
- Produces:
  - `class denso::ui::OrtEngine : public InferenceEngine` with `explicit OrtEngine(const std::string& model_path, const std::string& engine_cache_dir);`, `std::vector<Detection> infer(const cv::Mat& bgr) override;`, `const std::vector<std::string>& class_names() const override;`, `bool ok() const;`.
  - `static std::vector<std::string> OrtEngine::read_names(const std::string& model_path);` — opens a CPU-only session and returns the `names` metadata (used by `model_sync`).

This unit is not unit-tested (needs GPU + the model); it is verified by a build + a runtime smoke in Step 4.

- [ ] **Step 1: Implement `ort_engine.h`**

```cpp
// ONNX Runtime implementation of InferenceEngine. Owns one Ort::Env + Session
// loaded once from a model file, with an execution-provider fallback chain
// (TensorRT → CUDA → CPU): the first tier whose session builds wins, so the
// same binary runs GPU-accelerated where available and CPU otherwise. On a
// TensorRT tier, engines are cached under engine_cache_dir. infer() letterboxes
// the frame, runs the model, and decodes to Detections at a low conf floor;
// per-class filtering is the caller's job.
#pragma once

#include "ui/camera/shared/detection/inference_engine.h"

#include <onnxruntime_cxx_api.h>

#include <memory>
#include <string>
#include <vector>

namespace denso::ui {

class OrtEngine : public InferenceEngine {
public:
    OrtEngine(const std::string& model_path, const std::string& engine_cache_dir);

    std::vector<Detection> infer(const cv::Mat& bgr) override;
    const std::vector<std::string>& class_names() const override { return names_; }
    bool ok() const { return static_cast<bool>(session_); }

    /// Read the `names` metadata from a model without keeping a session (used by
    /// the startup catalog sync). Returns empty on failure.
    static std::vector<std::string> read_names(const std::string& model_path);

private:
    Ort::Env env_;
    std::unique_ptr<Ort::Session> session_;
    Ort::AllocatorWithDefaultOptions alloc_;
    std::string input_name_;
    std::string output_name_;
    std::vector<std::string> names_;

    static constexpr int kSize = 640;
    static constexpr float kConfFloor = 0.25f;
    static constexpr float kNmsIou = 0.45f;
};

} // namespace denso::ui
```

- [ ] **Step 2: Implement `ort_engine.cpp`**

```cpp
#include "ui/camera/shared/detection/ort_engine.h"

#include "ui/camera/shared/detection/letterbox.h"
#include "ui/camera/shared/detection/names_metadata.h"
#include "ui/camera/shared/detection/yolo_decode.h"

#include <QDebug>

#include <opencv2/imgproc.hpp>

#include <array>

namespace denso::ui {

namespace {

// Build a session for the requested provider tier; returns nullptr on failure.
// tier: 0 = TensorRT+CUDA, 1 = CUDA, 2 = CPU only.
std::unique_ptr<Ort::Session> make_session(Ort::Env& env, const std::wstring& path,
                                           int tier, const std::string& cache_dir) {
    try {
        Ort::SessionOptions opts;
        opts.SetIntraOpNumThreads(1);
        opts.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        if (tier == 0) {
            OrtTensorRTProviderOptions trt{};
            trt.device_id = 0;
            trt.trt_engine_cache_enable = 1;
            trt.trt_engine_cache_path = cache_dir.c_str();
            opts.AppendExecutionProvider_TensorRT(trt);
            OrtCUDAProviderOptions cuda{};
            cuda.device_id = 0;
            opts.AppendExecutionProvider_CUDA(cuda);
        } else if (tier == 1) {
            OrtCUDAProviderOptions cuda{};
            cuda.device_id = 0;
            opts.AppendExecutionProvider_CUDA(cuda);
        }
        return std::make_unique<Ort::Session>(env, path.c_str(), opts);
    } catch (const Ort::Exception& e) {
        qWarning().noquote() << "[ort] tier" << tier << "failed:" << e.what();
        return nullptr;
    }
}

std::wstring widen(const std::string& s) {
    return std::wstring(s.begin(), s.end());  // ASCII paths only (models/*.onnx)
}

} // namespace

OrtEngine::OrtEngine(const std::string& model_path, const std::string& engine_cache_dir)
    : env_(ORT_LOGGING_LEVEL_WARNING, "denso") {
    const std::wstring wpath = widen(model_path);
    for (int tier = 0; tier <= 2 && !session_; ++tier) {
        session_ = make_session(env_, wpath, tier, engine_cache_dir);
        if (session_) {
            qInfo().noquote() << "[ort] loaded" << QString::fromStdString(model_path)
                              << "tier" << tier;
        }
    }
    if (!session_) {
        return;
    }
    input_name_ = session_->GetInputNameAllocated(0, alloc_).get();
    output_name_ = session_->GetOutputNameAllocated(0, alloc_).get();

    Ort::ModelMetadata md = session_->GetModelMetadata();
    Ort::AllocatedStringPtr names = md.LookupCustomMetadataMapAllocated("names", alloc_);
    if (names) {
        names_ = parse_names_metadata(names.get());
    }
}

std::vector<std::string> OrtEngine::read_names(const std::string& model_path) {
    try {
        Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "denso-meta");
        Ort::SessionOptions opts;
        Ort::Session s(env, widen(model_path).c_str(), opts);  // CPU
        Ort::AllocatorWithDefaultOptions alloc;
        Ort::ModelMetadata md = s.GetModelMetadata();
        Ort::AllocatedStringPtr names =
            md.LookupCustomMetadataMapAllocated("names", alloc);
        return names ? parse_names_metadata(names.get()) : std::vector<std::string>{};
    } catch (const Ort::Exception&) {
        return {};
    }
}

std::vector<Detection> OrtEngine::infer(const cv::Mat& bgr) {
    if (!session_ || bgr.empty()) {
        return {};
    }
    // Letterbox → RGB → NCHW float32 [0,1] blob.
    cv::Mat lb_img;
    const LetterboxInfo lb = letterbox(bgr, lb_img, kSize);
    cv::Mat rgb;
    cv::cvtColor(lb_img, rgb, cv::COLOR_BGR2RGB);
    cv::Mat blob;
    cv::dnn::blobFromImage(rgb, blob, 1.0 / 255.0, cv::Size(kSize, kSize),
                           cv::Scalar(), /*swapRB=*/false, /*crop=*/false, CV_32F);

    const std::array<int64_t, 4> in_shape{1, 3, kSize, kSize};
    Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    Ort::Value input = Ort::Value::CreateTensor<float>(
        mem, reinterpret_cast<float*>(blob.data),
        static_cast<size_t>(blob.total()), in_shape.data(), in_shape.size());

    const char* in_names[] = {input_name_.c_str()};
    const char* out_names[] = {output_name_.c_str()};
    std::vector<Ort::Value> outputs;
    try {
        outputs = session_->Run(Ort::RunOptions{nullptr}, in_names, &input, 1,
                                out_names, 1);
    } catch (const Ort::Exception& e) {
        qWarning().noquote() << "[ort] run failed:" << e.what();
        return {};
    }

    // Output shape [1, 4+nc, na].
    const auto info = outputs[0].GetTensorTypeAndShapeInfo();
    const std::vector<int64_t> shape = info.GetShape();  // {1, 4+nc, na}
    const int num_classes = static_cast<int>(shape[1]) - 4;
    const int num_anchors = static_cast<int>(shape[2]);
    const float* out = outputs[0].GetTensorData<float>();

    return decode_yolo(out, num_classes, num_anchors, lb, bgr.cols, bgr.rows,
                       kConfFloor, kNmsIou);
}

} // namespace denso::ui
```

Add `ui/camera/shared/detection/ort_engine.cpp` to `add_executable(denso ...)` in `src/app/CMakeLists.txt`.

- [ ] **Step 3: Build**

Run: `export PATH=/c/msys64/ucrt64/bin:$PATH && cmake -S . -B build -G Ninja && cmake --build build`
Expected: `denso.exe` links (ORT headers compile under GCC; DLL links).

- [ ] **Step 4: Runtime smoke (manual, on this RTX 4070 box)**

Create `tests/smoke_ort.cpp` (temporary, NOT added to CMake) or a throwaway `main` and compile it against ORT + OpenCV, OR add a hidden `--smoke <image>` flag. Simplest: write a tiny standalone in the scratchpad:

```bash
export PATH=/c/msys64/ucrt64/bin:$PATH
SCR="/c/Users/Modela_AICam/AppData/Local/Temp/claude/D--workspace/33d1e957-b59c-444a-9bbe-dba53fdf5447/scratchpad"
cat > "$SCR/smoke_ort.cpp" <<'EOF'
#include "ui/camera/shared/detection/ort_engine.h"
#include <opencv2/imgcodecs.hpp>
#include <iostream>
int main(int argc, char** argv) {
    denso::ui::OrtEngine eng("models/denso.onnx", "models/trt_cache");
    std::cout << "ok=" << eng.ok() << " classes=" << eng.class_names().size() << "\n";
    if (argc > 1) {
        cv::Mat img = cv::imread(argv[1]);
        auto dets = eng.infer(img);
        std::cout << "detections=" << dets.size() << "\n";
        for (auto& d : dets)
            std::cout << "  cls=" << d.class_id << " conf=" << d.conf
                      << " box=" << d.box << "\n";
    }
}
EOF
g++ -std=c++20 "$SCR/smoke_ort.cpp" \
  src/app/ui/camera/shared/detection/ort_engine.cpp \
  src/app/ui/camera/shared/detection/letterbox.cpp \
  src/app/ui/camera/shared/detection/yolo_decode.cpp \
  src/app/ui/camera/shared/detection/names_metadata.cpp \
  -Isrc/app -Ithird_party/onnxruntime/include \
  $(pkg-config --cflags --libs opencv4) \
  third_party/onnxruntime/lib/onnxruntime.dll \
  -o "$SCR/smoke_ort.exe"
cp third_party/onnxruntime/lib/onnxruntime*.dll "$SCR/"
cp -r models "$SCR/models"
(cd "$SCR" && ./smoke_ort.exe models/../"$(ls models/*.onnx | head -1)")
```

Expected: `ok=1 classes=10`, and with a 7-segment test image (`D:\workspace\test_model\images\*`) `detections=N` with `cls` in 0–9. If `ok=1` but detections are 0 on a known-good image, inspect `kConfFloor`/decode. Delete the scratch smoke files after.

- [ ] **Step 5: Commit**

```bash
git add src/app/ui/camera/shared/detection/ort_engine.h src/app/ui/camera/shared/detection/ort_engine.cpp src/app/CMakeLists.txt
git commit -m "feat(detection): OrtEngine with TensorRT/CUDA/CPU fallback"
```

---

## Task 10: `EngineRegistry` (shared engine per model file)

**Files:**
- Create: `src/app/ui/camera/shared/detection/engine_registry.{h,cpp}`
- Modify: `src/app/CMakeLists.txt` (add `engine_registry.cpp`)

**Interfaces:**
- Consumes: `OrtEngine`, `InferenceEngine`.
- Produces: `class denso::ui::EngineRegistry` with `explicit EngineRegistry(std::string models_dir, std::string cache_dir);` and `InferenceEngine* get(const std::string& filename);` — lazily constructs one `OrtEngine` per distinct filename, resolving `models_dir/filename`; returns `nullptr` if the engine failed to load. Owns the engines.

Not unit-tested (constructs real engines). Verified by Task 12's app run.

- [ ] **Step 1: Implement `engine_registry.h`**

```cpp
// One shared inference engine per distinct model file. Cameras that attach the
// same model reuse a single Ort::Session (loaded lazily on first request), so
// N cameras on the same model pay for one load, not N. Owns the engines; hand
// out non-owning pointers. Not thread-safe: build/query it from the UI thread
// (CameraGrid::reload), before the capture threads start.
#pragma once

#include "ui/camera/shared/detection/inference_engine.h"
#include "ui/camera/shared/detection/ort_engine.h"

#include <map>
#include <memory>
#include <string>

namespace denso::ui {

class EngineRegistry {
public:
    EngineRegistry(std::string models_dir, std::string cache_dir)
        : models_dir_(std::move(models_dir)), cache_dir_(std::move(cache_dir)) {}

    /// Engine for `filename` (resolved under models_dir), or nullptr if it
    /// failed to load. Cached across calls.
    InferenceEngine* get(const std::string& filename);

private:
    std::string models_dir_;
    std::string cache_dir_;
    std::map<std::string, std::unique_ptr<OrtEngine>> engines_;
};

} // namespace denso::ui
```

- [ ] **Step 2: Implement `engine_registry.cpp`**

```cpp
#include "ui/camera/shared/detection/engine_registry.h"

namespace denso::ui {

InferenceEngine* EngineRegistry::get(const std::string& filename) {
    auto it = engines_.find(filename);
    if (it == engines_.end()) {
        auto eng = std::make_unique<OrtEngine>(models_dir_ + "/" + filename,
                                               cache_dir_);
        it = engines_.emplace(filename, std::move(eng)).first;
    }
    OrtEngine* e = it->second.get();
    return (e && e->ok()) ? e : nullptr;
}

} // namespace denso::ui
```

Add `ui/camera/shared/detection/engine_registry.cpp` to `add_executable(denso ...)`.

- [ ] **Step 3: Build**

Run: `cmake -S . -B build -G Ninja && cmake --build build`
Expected: links.

- [ ] **Step 4: Commit**

```bash
git add src/app/ui/camera/shared/detection/engine_registry.h src/app/ui/camera/shared/detection/engine_registry.cpp src/app/CMakeLists.txt
git commit -m "feat(detection): EngineRegistry (one shared engine per model file)"
```

---

## Task 11: `DetectionProcessor` + `camera_grid` wiring

**Files:**
- Modify: `src/app/ui/camera/grid/frame_processor.h`, `frame_processor.cpp` (add `DetectionProcessor`)
- Modify: `src/app/ui/camera/grid/camera_grid.h`, `camera_grid.cpp` (own an `EngineRegistry`, branch on `detection_for`)

**Interfaces:**
- Consumes: `InferenceEngine`, `detection::ResolvedModel`/`CameraDetection`, `apply_orientation`, `qimage_to_mat`, `mat_to_qimage`, `detection::detection_for`, `EngineRegistry`.
- Produces: `class DetectionProcessor : public FrameProcessor`.

Verified by the app run in Step 5 (no unit test — needs GPU + display).

- [ ] **Step 1: Declare `DetectionProcessor` in `frame_processor.h`**

Add includes and the class (after `OrientationProcessor`):

```cpp
#include "detection/detection.h"
#include "ui/camera/shared/detection/inference_engine.h"

#include <utility>
#include <vector>
// ... inside namespace denso::ui, after OrientationProcessor:

/// Runs one or more detection models on each frame and draws the results. Each
/// entry pairs a shared engine with the camera's per-class selections (class id
/// → min confidence). Orientation is applied first (so a detection tile matches
/// the others), then detection is drawn on the oriented frame.
class DetectionProcessor : public FrameProcessor {
public:
    struct ModelRun {
        InferenceEngine* engine;                       // shared, non-owning
        std::vector<std::string> class_names;          // for labels
        std::vector<denso::detection::ModelClassSelection> classes;  // id → conf
    };

    DetectionProcessor(int degrees, double pitch, double roll,
                       std::vector<ModelRun> models);
    QImage process(const QImage& frame) override;

private:
    int degrees_;
    double pitch_;
    double roll_;
    std::vector<ModelRun> models_;
};
```

- [ ] **Step 2: Implement in `frame_processor.cpp`**

Add includes at the top:

```cpp
#include "ui/camera/shared/frame_convert.h"  // qimage_to_mat, mat_to_qimage

#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cstdio>
```

Add the implementation:

```cpp
DetectionProcessor::DetectionProcessor(int degrees, double pitch, double roll,
                                       std::vector<ModelRun> models)
    : degrees_(degrees), pitch_(pitch), roll_(roll), models_(std::move(models)) {}

namespace {
// Min confidence a camera keeps for class_id, or nullopt if not selected.
std::optional<float> selected_conf(
    const std::vector<denso::detection::ModelClassSelection>& sel, int class_id) {
    for (const auto& s : sel) {
        if (s.class_id == class_id) return s.conf;
    }
    return std::nullopt;
}
} // namespace

QImage DetectionProcessor::process(const QImage& frame) {
    const QImage oriented = apply_orientation(frame, degrees_, pitch_, roll_);
    cv::Mat bgr = qimage_to_mat(oriented);
    if (bgr.empty()) {
        return oriented;
    }
    for (const ModelRun& run : models_) {
        if (!run.engine) continue;
        for (const Detection& d : run.engine->infer(bgr)) {
            const auto conf = selected_conf(run.classes, d.class_id);
            if (!conf || d.conf < *conf) continue;  // not selected / below thr
            cv::rectangle(bgr, d.box, cv::Scalar(0, 215, 255), 2);
            std::string label =
                (d.class_id < static_cast<int>(run.class_names.size())
                     ? run.class_names[d.class_id]
                     : std::to_string(d.class_id));
            char buf[16];
            std::snprintf(buf, sizeof(buf), " %.0f%%", d.conf * 100.0f);
            label += buf;
            cv::putText(bgr, label, cv::Point(d.box.x, std::max(0, d.box.y - 4)),
                        cv::FONT_HERSHEY_SIMPLEX, 0.5, cv::Scalar(0, 215, 255), 1);
        }
    }
    return mat_to_qimage(bgr);
}
```

*(Add `#include <optional>` to `frame_processor.cpp` if not present.)*

- [ ] **Step 3: Give `CameraGrid` an `EngineRegistry` and branch**

In `camera_grid.h`, add a member and include:

```cpp
#include "ui/camera/shared/detection/engine_registry.h"
#include <memory>
// ... in the class private section:
    std::unique_ptr<EngineRegistry> engines_;
```

In `camera_grid.cpp`, add includes:

```cpp
#include "detection/repo.h"
#include "ui/camera/shared/detection/engine_registry.h"

#include <QCoreApplication>
```

Initialize the registry lazily in `reload()` (top of the function, after `clear()`):

```cpp
    if (!engines_) {
        const std::string dir = QCoreApplication::applicationDirPath().toStdString();
        engines_ = std::make_unique<EngineRegistry>(dir + "/models",
                                                     dir + "/models/trt_cache");
    }
```

Replace the processor construction (currently the `std::make_unique<OrientationProcessor>(...)` in `reload()`) with:

```cpp
        const detection::CameraDetection det = detection::detection_for(db_, cam.id);
        std::unique_ptr<FrameProcessor> proc;
        if (det.models.empty()) {
            proc = std::make_unique<OrientationProcessor>(
                static_cast<int>(cam.rotation), cam.pitch, cam.roll);
        } else {
            std::vector<DetectionProcessor::ModelRun> runs;
            for (const detection::ResolvedModel& rm : det.models) {
                InferenceEngine* eng = engines_->get(rm.filename);
                if (!eng) continue;  // model failed to load — skip it
                runs.push_back({eng, rm.class_names, rm.classes});
            }
            if (runs.empty()) {
                proc = std::make_unique<OrientationProcessor>(
                    static_cast<int>(cam.rotation), cam.pitch, cam.roll);
            } else {
                proc = std::make_unique<DetectionProcessor>(
                    static_cast<int>(cam.rotation), cam.pitch, cam.roll,
                    std::move(runs));
            }
        }
        auto* stream = new CameraStream(cam, std::move(proc));
```

- [ ] **Step 4: Build**

Run: `cmake -S . -B build -G Ninja && cmake --build build`
Expected: `denso.exe` links; `ctest --test-dir build` still fully green (no regressions).

- [ ] **Step 5: Manual app smoke**

With a camera configured to run `denso.onnx` on some classes (set up in Task 13's UI, or temporarily seed the DB), run `./build/src/app/denso.exe` and confirm the tile shows digit boxes. (If Task 13 UI isn't built yet, seed via a throwaway SQL insert into `model`/`camera_model`/`camera_model_class`.)

- [ ] **Step 6: Commit**

```bash
git add src/app/ui/camera/grid/frame_processor.h src/app/ui/camera/grid/frame_processor.cpp src/app/ui/camera/grid/camera_grid.h src/app/ui/camera/grid/camera_grid.cpp
git commit -m "feat(detection): DetectionProcessor + per-camera grid wiring"
```

---

## Task 12: Model registry sync at startup

**Files:**
- Create: `src/app/ui/camera/shared/detection/model_sync.{h,cpp}`
- Modify: `src/app/CMakeLists.txt` (add `model_sync.cpp`)
- Modify: `src/app/main.cpp` (call after migrations)

**Interfaces:**
- Consumes: `OrtEngine::read_names`, `detection::upsert_model`, `detection::DetectionModel`.
- Produces: `void denso::ui::sync_models(const QSqlDatabase& db, const QString& models_dir);` — scans `models_dir/*.onnx`, reads each model's class names, and `upsert_model`s (name = filename stem).

Verified by app run (Task 13) + a log line.

- [ ] **Step 1: Implement `model_sync.h`**

```cpp
// Keep the `model` catalog in sync with the models/ folder: on startup, scan
// for *.onnx, read each one's class names from its ONNX metadata, and upsert a
// catalog row (name defaults to the filename stem). So dropping a new .onnx in
// models/ makes it selectable in the UI next launch; core never touches ONNX.
#pragma once

#include <QSqlDatabase>
#include <QString>

namespace denso::ui {

void sync_models(const QSqlDatabase& db, const QString& models_dir);

} // namespace denso::ui
```

- [ ] **Step 2: Implement `model_sync.cpp`**

```cpp
#include "ui/camera/shared/detection/model_sync.h"

#include "detection/detection.h"
#include "detection/repo.h"
#include "ui/camera/shared/detection/ort_engine.h"

#include <QDebug>
#include <QDir>
#include <QFileInfo>

namespace denso::ui {

void sync_models(const QSqlDatabase& db, const QString& models_dir) {
    QDir dir(models_dir);
    const QStringList files = dir.entryList({QStringLiteral("*.onnx")}, QDir::Files);
    for (const QString& f : files) {
        const QString path = dir.absoluteFilePath(f);
        detection::DetectionModel m;
        m.filename = f.toStdString();
        m.name = QFileInfo(f).completeBaseName().toStdString();
        m.class_names = OrtEngine::read_names(path.toStdString());
        if (m.class_names.empty()) {
            qWarning().noquote() << "[model_sync] no class names in" << f
                                 << "- skipping";
            continue;
        }
        if (!detection::upsert_model(db, m)) {
            qWarning().noquote() << "[model_sync] upsert failed for" << f;
        }
    }
}

} // namespace denso::ui
```

Add `ui/camera/shared/detection/model_sync.cpp` to `add_executable(denso ...)`.

- [ ] **Step 3: Call it from `main.cpp`**

In `src/app/main.cpp`, after migrations succeed and before the window is shown (find the `run_migrations` call and the DB handle), add:

```cpp
#include "ui/camera/shared/detection/model_sync.h"
// ... after run_migrations(...) succeeds:
    denso::ui::sync_models(db_handle,
                           QCoreApplication::applicationDirPath() + "/models");
```

(Use whatever the local DB-handle variable is named at that point in `main.cpp`; ensure `<QCoreApplication>` is included.)

- [ ] **Step 4: Build + run**

Run: `cmake -S . -B build -G Ninja && cmake --build build && ./build/src/app/denso.exe`
Expected: on first launch, the `model` table gets a `denso.onnx` row (verify: stop the app, open `build/src/app/denso.db`, `SELECT * FROM model;` shows one row with 10 class names). `ctest` still green.

- [ ] **Step 5: Commit**

```bash
git add src/app/ui/camera/shared/detection/model_sync.h src/app/ui/camera/shared/detection/model_sync.cpp src/app/CMakeLists.txt src/app/main.cpp
git commit -m "feat(detection): sync model catalog from models/ at startup"
```

---

## Task 13: "Models" wizard page + dialog wiring

**Files:**
- Create: `src/app/ui/camera/dialog/models_page.{h,cpp}`
- Modify: `src/app/ui/camera/dialog/wizard_stepper.cpp` (add the "Models" step label)
- Modify: `src/app/ui/camera/camera_dialog.h`, `camera_dialog.cpp` (insert the page, nav, persistence)
- Modify: `src/app/CMakeLists.txt` (add `models_page.cpp`)

**Interfaces:**
- Consumes: `detection::list_models`, `detection::models_for`, `detection::set_camera_models`, `detection::DetectionModel`, `detection::CameraModel`, `detection::ModelClassSelection`.
- Produces: `class ModelsPage : public QWidget` with `void set_db(QSqlDatabase);`, `void load_for(int64_t camera_id);` (populate from catalog + current attachments), `std::vector<detection::CameraModel> selections(int64_t camera_id) const;` (read the widgets back), and signals `back_requested()` / `finish_requested()`.

This is UI — verified by building + a manual click-through, not a unit test. Follow the existing page/coordinator pattern (`configure_page` is the closest model to copy).

- [ ] **Step 1: Read the pattern**

Read `src/app/ui/camera/dialog/configure_page.{h,cpp}` and `camera_dialog.cpp`'s `show_page()` / page-stack / footer handling to mirror the exact stepper + Back/Next/Finish wiring. Note how `configure_page` exposes getters/setters and emits `back_requested`/`next_requested`.

- [ ] **Step 2: Implement `models_page.h`**

```cpp
// The wizard's "Models" step: attach 1..N detection models to a camera and, per
// attached model, pick which classes to keep and each class's confidence. Reads
// the catalog + current attachments from detection::repo; the coordinator saves
// via detection::set_camera_models on Finish. Pure widget — owns its controls,
// emits requests, holds no business logic.
#pragma once

#include "detection/detection.h"

#include <QSqlDatabase>
#include <QWidget>

#include <cstdint>
#include <vector>

class QVBoxLayout;

namespace denso::ui {

class ModelsPage : public QWidget {
    Q_OBJECT
public:
    explicit ModelsPage(QWidget* parent = nullptr);

    void set_db(QSqlDatabase db) { db_ = std::move(db); }
    void load_for(int64_t camera_id);
    std::vector<denso::detection::CameraModel> selections(int64_t camera_id) const;

signals:
    void back_requested();
    void finish_requested();

private:
    QSqlDatabase db_;
    QVBoxLayout* list_layout_ = nullptr;
    // One row group per catalog model; see .cpp for the per-model widget bundle.
    struct ModelRowWidgets;
    std::vector<ModelRowWidgets> rows_;
};

} // namespace denso::ui
```

- [ ] **Step 3: Implement `models_page.cpp`**

```cpp
#include "ui/camera/dialog/models_page.h"

#include "detection/repo.h"

#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>
#include <QWidget>

namespace denso::ui {

// Per catalog model: an "attach" checkbox + one (select checkbox, conf spin)
// pair per class. selections() reads these back into CameraModel structs.
struct ModelsPage::ModelRowWidgets {
    int64_t model_id = 0;
    QCheckBox* attach = nullptr;
    std::vector<QCheckBox*> class_on;
    std::vector<QDoubleSpinBox*> class_conf;
};

ModelsPage::ModelsPage(QWidget* parent) : QWidget(parent) {
    auto* root = new QVBoxLayout(this);
    root->addWidget(new QLabel(QStringLiteral("Detection models")));

    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    auto* holder = new QWidget;
    list_layout_ = new QVBoxLayout(holder);
    scroll->setWidget(holder);
    root->addWidget(scroll, 1);

    auto* footer = new QHBoxLayout;
    auto* back = new QPushButton(QStringLiteral("Back"));
    auto* finish = new QPushButton(QStringLiteral("Finish"));
    footer->addWidget(back);
    footer->addStretch(1);
    footer->addWidget(finish);
    root->addLayout(footer);
    connect(back, &QPushButton::clicked, this, &ModelsPage::back_requested);
    connect(finish, &QPushButton::clicked, this, &ModelsPage::finish_requested);
}

void ModelsPage::load_for(int64_t camera_id) {
    // Clear previous rows.
    rows_.clear();
    QLayoutItem* item = nullptr;
    while ((item = list_layout_->takeAt(0)) != nullptr) {
        delete item->widget();
        delete item;
    }

    const auto catalog = denso::detection::list_models(db_);
    const auto attached = denso::detection::models_for(db_, camera_id);

    for (const auto& model : catalog) {
        ModelRowWidgets w;
        w.model_id = model.id;
        auto* box = new QGroupBox(QString::fromStdString(model.name));
        auto* v = new QVBoxLayout(box);
        w.attach = new QCheckBox(QStringLiteral("Run this model"));
        v->addWidget(w.attach);

        // Find this model's current attachment (if any) for pre-fill.
        const denso::detection::CameraModel* cur = nullptr;
        for (const auto& a : attached) {
            if (a.model_id == model.id) { cur = &a; break; }
        }
        w.attach->setChecked(cur != nullptr);

        for (size_t k = 0; k < model.class_names.size(); ++k) {
            auto* row = new QHBoxLayout;
            auto* on = new QCheckBox(QString::fromStdString(model.class_names[k]));
            auto* conf = new QDoubleSpinBox;
            conf->setRange(0.0, 1.0);
            conf->setSingleStep(0.05);
            conf->setValue(0.5);
            // Pre-fill from current selection.
            if (cur) {
                for (const auto& s : cur->classes) {
                    if (s.class_id == static_cast<int>(k)) {
                        on->setChecked(true);
                        conf->setValue(s.conf);
                    }
                }
            }
            row->addWidget(on, 1);
            row->addWidget(conf);
            v->addLayout(row);
            w.class_on.push_back(on);
            w.class_conf.push_back(conf);
        }
        list_layout_->addWidget(box);
        rows_.push_back(std::move(w));
    }
    list_layout_->addStretch(1);
}

std::vector<denso::detection::CameraModel> ModelsPage::selections(
    int64_t camera_id) const {
    std::vector<denso::detection::CameraModel> out;
    for (const ModelRowWidgets& w : rows_) {
        if (!w.attach->isChecked()) continue;
        denso::detection::CameraModel cm;
        cm.camera_id = camera_id;
        cm.model_id = w.model_id;
        for (size_t k = 0; k < w.class_on.size(); ++k) {
            if (w.class_on[k]->isChecked()) {
                cm.classes.push_back(denso::detection::ModelClassSelection{
                    static_cast<int>(k),
                    static_cast<float>(w.class_conf[k]->value())});
            }
        }
        out.push_back(std::move(cm));
    }
    return out;
}

} // namespace denso::ui
```

Add `ui/camera/dialog/models_page.cpp` to `add_executable(denso ...)`.

- [ ] **Step 4: Insert the page into the wizard**

In `wizard_stepper.cpp`, add a "Models" step between "Configure" and "Areas" (follow how the existing three labels are built).

In `camera_dialog.{h,cpp}`: instantiate `ModelsPage`, `set_db(db)`, add it to the page stack after Configure and before Areas; in `show_page()` extend the stepper index handling; on entering the Models step call `models_page_->load_for(current_camera_id_)`; connect `back_requested`→previous page and `finish_requested`→ persist. In the save path (where the camera is inserted/updated on Finish) also call:

```cpp
    denso::detection::set_camera_models(
        db_, camera_id, models_page_->selections(camera_id));
```

Ensure the camera id is known before saving models (for the add flow the camera is inserted first, then its id used for `set_camera_models`). After Finish, the dialog closes and `CameraView::reload()` rebuilds the grid, which now picks up the new detection config.

- [ ] **Step 5: Build + manual click-through**

Run: `cmake -S . -B build -G Ninja && cmake --build build && ./build/src/app/denso.exe`
Expected: Add/Edit a camera → the wizard shows a **Models** step listing `denso` with classes `0`–`9`, each with a checkbox + conf spinner. Attach it, select a few digits, Finish. The live tile then draws boxes for the selected digits. `ctest` still green.

- [ ] **Step 6: Commit**

```bash
git add src/app/ui/camera/dialog/models_page.h src/app/ui/camera/dialog/models_page.cpp src/app/ui/camera/dialog/wizard_stepper.cpp src/app/ui/camera/camera_dialog.h src/app/ui/camera/camera_dialog.cpp src/app/CMakeLists.txt
git commit -m "feat(detection): Models wizard step (attach models, per-class conf)"
```

---

## Task 14: Full gate + docs

**Files:**
- Modify: `CLAUDE.md` (document the detection module + ORT provisioning), `docs/ARCHITECTURE.md` if present.

- [ ] **Step 1: Full suite**

Run: `export PATH=/c/msys64/ucrt64/bin:$PATH && cmake -S . -B build -G Ninja && cmake --build build && ctest --test-dir build`
Expected: all green (96 prior + the new pure tests: class_names 3, detection_repo ≥7, letterbox 2, yolo_decode 1, names_metadata 2, frame_convert 1).

- [ ] **Step 2: Update `CLAUDE.md`**

Add `src/core/detection/` (domain: `detection.h` types + `repo` + `class_names`) to the core table, and `ui/camera/shared/detection/` (ORT inference: engine/registry/letterbox/decode/sync) to the app table. Add a "Detection / ONNX Runtime" note: ORT GPU build lives in `third_party/onnxruntime/` (git-ignored; provision per platform), DLLs are copied beside the exe by a POST_BUILD step, EP fallback is TensorRT→CUDA→CPU, and `models/*.onnx` are synced into the `model` catalog at startup.

- [ ] **Step 3: Commit**

```bash
git add CLAUDE.md docs/ARCHITECTURE.md
git commit -m "docs(detection): document detection module + ORT provisioning"
```

---

## Self-Review

**Spec coverage:**
- Runtime = ORT + EP fallback → Task 9. Overlay only → Task 11. ✔
- Per-camera 1..N models + class selection + per-class conf → domain Tasks 1–4, UI Task 13. ✔
- Domain in core / runtime in app → Tasks 1–4 (core) vs 5–13 (app). ✔
- 3 tables + `SCHEMA_VERSION` bump → Task 1. ✔
- Shared engine per model file → Task 10. ✔
- Model registry sync at startup → Task 12. ✔
- New "Models" wizard step → Task 13. ✔
- Build: ORT imported target, POST_BUILD DLL + model copy, `.gitignore` ORT, commit onnx → Task 8. ✔
- Pure unit tests for repo/letterbox/yolo_decode → Tasks 3–7; manual smoke → Tasks 9, 11, 13. ✔
- Rename stage → already DONE (noted in Global Constraints). ✔

**Placeholder scan:** No TBD/TODO; every code step has full code. Manual-smoke steps are explicit commands, used only where GPU/display/model make a unit test impossible (allowed by the spec).

**Type consistency:** `Detection{cv::Rect box; int class_id; float conf}` defined in Task 6, consumed unchanged in Tasks 9/11. `LetterboxInfo{scale,pad_x,pad_y,size}` Task 5 → used in 6/9. `detection_for`/`models_for`/`set_camera_models`/`upsert_model`/`list_models` signatures defined in Task 3 `repo.h`, used consistently in 4/11/12/13. `InferenceEngine::infer`/`class_names` (Task 6) implemented by `OrtEngine` (Task 9), consumed via `EngineRegistry::get` (Task 10) in Task 11. `DetectionProcessor::ModelRun` fields match what Task 11 Step 3 constructs. Consistent.
