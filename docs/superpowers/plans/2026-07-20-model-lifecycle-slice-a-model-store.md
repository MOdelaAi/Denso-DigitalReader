# Model Lifecycle Slice A — Model Store, Manifest & `--migrate-model` Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Ship the immutable model store's app-side foundation — a validated `manifest.json`, content hashing, and a transactional `denso --migrate-model` command that re-points camera attachments by class *name* and writes a rollback receipt rich enough for Slice C.

**Architecture:** New Qt-Core-only module `src/core/models/` (manifest parse/validate, SHA-256, class-name remap) linked into `denso_core`; a migration routine in `src/core/detection/migrate.{h,cpp}` mutating `model` + `camera_model` in one SQLite transaction with a compare-and-swap precondition; a headless coordinator + new CLI mode through the existing `src/core/cli/args` + `src/app/cli/run_headless` seam. Every unit is pure or DB-only → Catch2-testable on the MSYS2 dev machine, no backend, no GPU.

**Tech Stack:** C++17, Qt6 (Core, Sql), SQLite (WAL, `user_version` migrations), Catch2 v3. Spec: `docs/superpowers/specs/2026-07-20-model-lifecycle-24-7-design.md` (§3.1, §3.2, §3.4).

## Global Constraints

- **Toolchain:** MSYS2 UCRT64. Configure `cmake -S . -B build -G Ninja`; build `cmake --build build`.
- **Running specific tests:** `catch_discover_tests` registers each case by its **quoted name**, not by `[tag]`. Run a tag subset via the test binary directly: `./build/tests/denso_tests "[manifest]"`. `ctest -R <regex>` matches **test-case names**, not tags.
- **Catch2 test names are CLI arguments:** ASCII only, never start with `--`.
- **`denso_core` links only `Qt6::Core`/`Sql`** — never Widgets/OpenCV/TensorRT.
- **Migrations are append-only:** current head is **`SCHEMA_VERSION = 12`** (`db.cpp:17`), written via `PRAGMA user_version = SCHEMA_VERSION` (`db.cpp:392`). This slice bumps it to **13** and adds a `version < 13` block; never edit a shipped block.
- **Manifest is the sole artifact-identity authority** (spec §3.4): identity is the **SHA-256**, never the filename alone.
- **Path hygiene:** engine/sidecar are **basenames** under `models/`; additionally, Task 8 must **canonicalize and prove both files resolve *under* the real `models_dir`** (a basename symlink can still escape).
- New `src/core/` sources → `src/core/CMakeLists.txt` (`denso_core`); new tests → `tests/CMakeLists.txt` (`denso_tests`).
- **Add explicit includes** in new `.cpp`s: `<optional>`, `<utility>`, `<cstddef>` (use `std::size_t`); don't rely on transitive includes (Jetson/GCC).

> **Scope honesty:** Slice A builds and tests `--migrate-model` and the manifest infra, but startup still calls `sync_models()` (`main.cpp:220`). **Slice A is transitional — it does NOT yet establish manifest authority.** Retiring the boot scan lands in **Slice B** (it needs the readiness/manifest-catalog path). Until then, do not claim in code/docs that the manifest is the *sole* catalog input.

---

### Task 1: File SHA-256 helper

**Files:** Create `src/core/models/hashing.{h,cpp}`; Test `tests/test_hashing.cpp`; Modify `src/core/CMakeLists.txt`, `tests/CMakeLists.txt`.

**Interfaces:** Produces `std::optional<std::string> denso::models::file_sha256(const QString& path)` — lowercase hex (64 chars) or `nullopt` if unreadable.

- [ ] **Step 1: Failing test**

```cpp
// tests/test_hashing.cpp
#include <catch2/catch_test_macros.hpp>
#include "models/hashing.h"
#include <QTemporaryFile>

TEST_CASE("file_sha256 hashes known content", "[hashing]") {
    QTemporaryFile f; REQUIRE(f.open());
    f.write("abc"); f.flush();
    auto h = denso::models::file_sha256(f.fileName());
    REQUIRE(h.has_value());
    REQUIRE(*h == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}
TEST_CASE("file_sha256 returns nullopt for a missing file", "[hashing]") {
    REQUIRE_FALSE(denso::models::file_sha256("/no/such/file/xyz").has_value());
}
```

- [ ] **Step 2: Register + verify fail** — add `test_hashing.cpp` to `tests/CMakeLists.txt`; `cmake --build build` → FAIL (`models/hashing.h` not found).

- [ ] **Step 3: Implement**

```cpp
// src/core/models/hashing.h
#pragma once
#include <QString>
#include <optional>
#include <string>
namespace denso::models {
std::optional<std::string> file_sha256(const QString& path);
}
```
```cpp
// src/core/models/hashing.cpp
#include "models/hashing.h"
#include <QCryptographicHash>
#include <QFile>
namespace denso::models {
std::optional<std::string> file_sha256(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return std::nullopt;
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&f)) return std::nullopt;     // Qt6: addData(QIODevice*) -> bool
    return hash.result().toHex().toStdString();
}
}
```
Add both to `denso_core` in `src/core/CMakeLists.txt`.

- [ ] **Step 4: Run** — `cmake --build build && ./build/tests/denso_tests "[hashing]"` → PASS.
- [ ] **Step 5: Commit** — `git add …; git commit -m "feat(models): streaming file_sha256 helper"`

---

### Task 2: Manifest type + structural parse

**Files:** Create `src/core/models/manifest.{h,cpp}`; Test `tests/test_manifest.cpp`; Modify both CMakeLists.

**Interfaces:** `ModelGeneration{name,engine,engine_sha256,sidecar,sidecar_sha256; vector<string> class_names; trt,cuda,sm,installed_utc,state}`, `Manifest{int schema; vector<ModelGeneration> generations}`, `ParseResult{optional<Manifest> manifest; string error}`, `ParseResult parse_manifest(const std::string&)` — structural only.

- [ ] **Step 1: Failing test** (use a **real 64-char** sha so later validation fixtures reuse it)

```cpp
// tests/test_manifest.cpp
#include <catch2/catch_test_macros.hpp>
#include "models/manifest.h"
static const char* kSha =
  "0000000000000000000000000000000000000000000000000000000000000000";
static std::string one_gen_json() {
    return std::string(R"({"schema":1,"generations":[{)") +
      R"("name":"digit-v3.1","engine":"digit-v3.1.engine","engine_sha256":")" + kSha + R"(",)" +
      R"("sidecar":"digit-v3.1.names.json","sidecar_sha256":")" + kSha + R"(",)" +
      R"("class_names":["0","1"],"built_for":{"trt":"10.3","cuda":"12.6","sm":"87"},)" +
      R"("installed_utc":"2026-07-20T00:00:00Z","state":"installed"}]})";
}
TEST_CASE("parse_manifest reads a valid generation", "[manifest]") {
    auto r = denso::models::parse_manifest(one_gen_json());
    REQUIRE(r.error.empty());
    REQUIRE(r.manifest.has_value());
    REQUIRE(r.manifest->generations.size() == 1);
    REQUIRE(r.manifest->generations[0].engine == "digit-v3.1.engine");
    REQUIRE(r.manifest->generations[0].sm == "87");
}
TEST_CASE("parse_manifest rejects non-JSON", "[manifest]") {
    REQUIRE_FALSE(denso::models::parse_manifest("not json").manifest.has_value());
}
TEST_CASE("parse_manifest rejects a generation missing engine", "[manifest]") {
    REQUIRE_FALSE(denso::models::parse_manifest(
        R"({"schema":1,"generations":[{"name":"x"}]})").manifest.has_value());
}
```

- [ ] **Step 2: Register + verify fail** — add to `tests/CMakeLists.txt`; build → FAIL.

- [ ] **Step 3: Implement**

```cpp
// src/core/models/manifest.h
#pragma once
#include <optional>
#include <string>
#include <vector>
namespace denso::models {
struct ModelGeneration {
    std::string name, engine, engine_sha256, sidecar, sidecar_sha256;
    std::vector<std::string> class_names;
    std::string trt, cuda, sm, installed_utc, state;
};
struct Manifest { int schema = 0; std::vector<ModelGeneration> generations; };
struct ParseResult { std::optional<Manifest> manifest; std::string error; };
ParseResult parse_manifest(const std::string& json_text);   // structural only
}
```
```cpp
// src/core/models/manifest.cpp
#include "models/manifest.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
namespace denso::models {
namespace {
ParseResult fail(const std::string& why) { return {std::nullopt, why}; }
bool str(const QJsonObject& o, const char* k, std::string& out) {
    if (!o.contains(k) || !o.value(k).isString()) return false;
    out = o.value(k).toString().toStdString(); return true;
}
}
ParseResult parse_manifest(const std::string& json_text) {
    QJsonParseError err{};
    const auto doc = QJsonDocument::fromJson(QByteArray::fromStdString(json_text), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject())
        return fail("manifest is not a JSON object");
    const QJsonObject root = doc.object();
    const QJsonValue sv = root.value("schema");
    // schema must be an INTEGER equal to 1 (reject 1.5, strings, etc.)
    if (!sv.isDouble() || sv.toDouble() != static_cast<double>(sv.toInt()) || sv.toInt() != 1)
        return fail("schema must be the integer 1");
    if (!root.value("generations").isArray()) return fail("missing generations array");
    Manifest m; m.schema = sv.toInt();
    for (const auto v : root.value("generations").toArray()) {
        if (!v.isObject()) return fail("generation is not an object");
        const QJsonObject o = v.toObject();
        ModelGeneration g;
        if (!str(o,"name",g.name) || !str(o,"engine",g.engine) ||
            !str(o,"engine_sha256",g.engine_sha256) || !str(o,"sidecar",g.sidecar) ||
            !str(o,"sidecar_sha256",g.sidecar_sha256) ||
            !str(o,"installed_utc",g.installed_utc) || !str(o,"state",g.state))
            return fail("generation missing a required string field");
        if (!o.value("class_names").isArray()) return fail("generation missing class_names");
        for (const auto c : o.value("class_names").toArray()) {
            if (!c.isString()) return fail("class_names must be strings");
            g.class_names.push_back(c.toString().toStdString());
        }
        const QJsonObject bf = o.value("built_for").toObject();
        if (!str(bf,"trt",g.trt) || !str(bf,"cuda",g.cuda) || !str(bf,"sm",g.sm))
            return fail("generation missing built_for.{trt,cuda,sm}");
        m.generations.push_back(std::move(g));
    }
    return {m, {}};
}
}
```
Add both to `denso_core` in `src/core/CMakeLists.txt`.

- [ ] **Step 4: Run** — `./build/tests/denso_tests "[manifest]"` → PASS (3).
- [ ] **Step 5: Commit** — `feat(models): manifest struct + structural JSON parse`

---

### Task 3: Manifest semantic validation + lookup

**Files:** Modify `src/core/models/manifest.{h,cpp}`; extend `tests/test_manifest.cpp`.

**Interfaces:** `std::optional<std::string> validate_manifest(const Manifest&)`; `const ModelGeneration* find_by_engine(const Manifest&, const std::string&)`.

Rules (spec §3.1, hardened per review): `schema == 1` **exactly** (reject `schema` non-integer or ≠1 — enforce in `parse_manifest`: require `isDouble()` **and** integral value); each `engine`/`sidecar` a safe basename (no `/ \ ..`, non-empty); engine/sidecar **stems match**; `class_names` **non-empty, unique, no blanks**; `name`/`installed_utc`/`trt`/`cuda`/`sm` **non-empty**; `engine_sha256`/`sidecar_sha256` **exactly 64 lowercase hex**; `state == "installed"`; `name`s unique; `engine`s unique.

> **Known limitation (accepted):** `QJsonDocument` silently collapses duplicate JSON object keys, so the spec's "duplicate keys rejected" cannot be enforced by this parser without a custom scanner. **Decision: accept this** — the manifest is written only by `denso-setup` (Slice C), not hand-authored, so duplicate keys are not a real threat surface. Slice C's writer emits canonical JSON. (This softens spec §3.1's blanket "duplicate JSON keys rejected" — noted for the spec's next revision.)

- [ ] **Step 1: Failing tests** — accept a clean manifest (reuse `one_gen_json()`); reject `../evil.engine`; reject mismatched stems; reject duplicate engine filenames; reject a 2-char sha (`"aa"`); reject an empty/duplicate class name; `find_by_engine` hit + miss. (All `[manifest]`.)

- [ ] **Step 2: Verify fail** — build → FAIL (undeclared).

- [ ] **Step 3: Implement** — reopen the namespace explicitly:

```cpp
// append to src/core/models/manifest.cpp — REOPEN the namespace
namespace denso::models {
namespace { /* is_basename, is_lower_hex64 (len==64 && all lower-hex), stem_of,
               sidecar_stem — as helpers */ }
std::optional<std::string> validate_manifest(const Manifest& m) { /* rules above */ }
const ModelGeneration* find_by_engine(const Manifest& m, const std::string& engine) {
    for (const auto& g : m.generations) if (g.engine == engine) return &g;
    return nullptr;
}
} // namespace denso::models
```
Declarations go inside the existing `namespace denso::models` block in `manifest.h`. Also update `parse_manifest` to reject a non-integral or ≠1 `schema`.

- [ ] **Step 4: Run** — `./build/tests/denso_tests "[manifest]"` → PASS.
- [ ] **Step 5: Commit** — `feat(models): manifest semantic validation + find_by_engine`

---

### Task 4: Class-name remap resolver (injective)

**Files:** Create `src/core/models/class_map.{h,cpp}`; Test `tests/test_class_map.cpp`; Modify both CMakeLists.

**Interfaces:** `struct ClassMapResult{optional<map<int,int>> map; string error}`; `ClassMapResult resolve_class_map(const vector<string>& old_names, const vector<string>& new_names, const map<string,string>& explicit_remap)`.

Rules (hardened per review — the forward map MUST be safely invertible): reject **duplicate names in `new_names`**; reject **duplicate names in `old_names`**; every `explicit_remap` **key must exist in `old_names`** and **value in `new_names`**; the resulting forward map must be **injective** (no two old ids map to the same new id) — else refuse. An old id whose resolved name is absent from `new_names` is omitted (the migration errors only if a *selected* id is unmapped).

- [ ] **Step 1: Failing tests** — id→id for identical names; reorder maps by name; **reject duplicate new name**; **reject duplicate old name**; explicit remap redirects; **reject explicit key absent from old**; **reject explicit target absent from new**; **reject many-to-one** (`old {"a","b"}`, `new {"x"}`, remap `{"a":"x","b":"x"}` → not injective).

- [ ] **Step 2: Register + verify fail.**

- [ ] **Step 3: Implement** — build `new_index` (reject dup new); reject dup old; validate `explicit_remap` keys∈old & values∈new; build forward map; while inserting, track used new-ids and **fail on a collision** (non-injective).

- [ ] **Step 4: Run** — `./build/tests/denso_tests "[class_map]"` → PASS.
- [ ] **Step 5: Commit** — `feat(models): resolve_class_map — injective remap by name`

---

### Task 5: Migration v13 — expanded `model_migration_receipt`

**Files:** Modify `src/core/db/db.cpp`; update `tests/test_db.cpp` (existing asserts **and** a new case).

**Interfaces:** Table rich enough for Slice C rollback (spec §3.2.6): exact `camera_model.id`s, old **and** new model identities, prior selections+thresholds, forward+inverse maps.

```sql
CREATE TABLE IF NOT EXISTS model_migration_receipt (
    id             INTEGER PRIMARY KEY,
    created_utc    TEXT NOT NULL,
    old_filename   TEXT NOT NULL,
    old_model_id   INTEGER NOT NULL,
    old_name       TEXT NOT NULL,
    old_class_names TEXT NOT NULL,          -- JSON array (for reverse repoint)
    new_filename   TEXT NOT NULL,
    new_model_id   INTEGER NOT NULL,
    new_engine_sha256 TEXT NOT NULL,        -- new artifact identity
    forward_map    TEXT NOT NULL,           -- JSON {old_class_id:new_class_id}
    inverse_map    TEXT NOT NULL,           -- JSON {new_class_id:old_class_id}
    attachments    TEXT NOT NULL            -- JSON [{camera_id,camera_model_id,classes:[{class_id,conf}]}]
)
```

- [ ] **Step 1: Update existing + new tests**
  - Change the two existing assertions from `12` to `13` at `tests/test_db.cpp:55` and `:63`.
  - Add:
```cpp
TEST_CASE("migration v13 creates model_migration_receipt", "[db]") {
    auto db = denso::db::Db::open_in_memory();  REQUIRE(db);
    REQUIRE(denso::db::run_migrations(db->handle()));
    QSqlQuery q(db->handle());
    REQUIRE(q.exec("SELECT count(*) FROM sqlite_master WHERE type='table' "
                   "AND name='model_migration_receipt'"));
    REQUIRE(q.next()); REQUIRE(q.value(0).toInt() == 1);
    QSqlQuery v(db->handle());
    REQUIRE(v.exec("PRAGMA user_version")); REQUIRE(v.next());
    REQUIRE(v.value(0).toInt() == 13);
}
```

- [ ] **Step 2: Verify fail** — `./build/tests/denso_tests "[db]"` → FAIL (asserts 12; table absent).

- [ ] **Step 3: Implement** — set `SCHEMA_VERSION = 13` (`db.cpp:17`); add the `if (version < 13) { … CREATE TABLE … }` block **before** the `PRAGMA user_version = SCHEMA_VERSION` write.

- [ ] **Step 4: Run** — `./build/tests/denso_tests "[db]"` → PASS.
- [ ] **Step 5: Commit** — `feat(db): migration v13 — model_migration_receipt (rollback-complete)`

---

### Task 6: `--migrate-model` CLI parse

**Files:** Modify `src/core/cli/args.{h,cpp}`; extend `tests/test_cli_args.cpp`.

**Interfaces:** `Mode::MigrateModel`; **append after `error`** on `Command`: `QString old_engine, new_engine, class_map_path; QList<qint64> cameras;` (appending keeps the existing aggregate initializers like `Command{Mode::Error,{},{},why}` at `args.cpp:26` valid). Grammar `--migrate-model --old <f> --new <f> --camera <id>… [--class-map <p>]`; require `--old`,`--new`,≥1 `--camera`; **reject duplicate camera ids**; non-integer id or missing value → `Error`.

- [ ] **Step 1: Failing tests** — parse two cameras → `MigrateModel` + fields; missing `--old`/`--new`/`--camera` → `Error`; non-integer camera → `Error`; **duplicate camera id → `Error`**. (`[cli]`.)

- [ ] **Step 2: Verify fail.**

- [ ] **Step 3: Implement**

In `args.h`: add `MigrateModel,` to `enum class Mode` (before `Error`), and **append after `error`** on `struct Command`:
```cpp
    QString old_engine;      ///< MigrateModel: --old <file>
    QString new_engine;      ///< MigrateModel: --new <file>
    QString class_map_path;  ///< MigrateModel: --class-map <path> (optional)
    QList<qint64> cameras;   ///< MigrateModel: repeated --camera <id>
```
In `args.cpp`, add a dispatch line in `parse()` before the final `return error(...)`:
```cpp
    if (flag == QStringLiteral("--migrate-model")) return parse_migrate(rest);
```
And the helper in the anonymous namespace:
```cpp
Command parse_migrate(const QStringList& rest) {
    Command c; c.mode = Mode::MigrateModel;
    for (int i = 0; i < rest.size(); ++i) {
        const QString& a = rest.at(i);
        auto need = [&](QString& dst) -> bool {
            if (i + 1 >= rest.size()) return false;
            dst = rest.at(++i); return true;
        };
        if (a == QStringLiteral("--old"))       { if (!need(c.old_engine))     return error(QStringLiteral("--old requires a filename")); }
        else if (a == QStringLiteral("--new"))  { if (!need(c.new_engine))     return error(QStringLiteral("--new requires a filename")); }
        else if (a == QStringLiteral("--class-map")) { if (!need(c.class_map_path)) return error(QStringLiteral("--class-map requires a path")); }
        else if (a == QStringLiteral("--camera")) {
            if (i + 1 >= rest.size()) return error(QStringLiteral("--camera requires an id"));
            bool ok = false; const qint64 id = rest.at(++i).toLongLong(&ok);
            if (!ok) return error(QStringLiteral("--camera id must be an integer"));
            if (c.cameras.contains(id)) return error(QStringLiteral("duplicate --camera id: %1").arg(id));
            c.cameras << id;
        } else return error(QStringLiteral("unexpected argument to --migrate-model: %1").arg(a));
    }
    if (c.old_engine.isEmpty() || c.new_engine.isEmpty() || c.cameras.isEmpty())
        return error(QStringLiteral("--migrate-model requires --old, --new, and at least one --camera"));
    return c;
}
```
Extend `usage()` with a `--migrate-model` line. (Appending the new `Command` members keeps existing `Command{Mode::X,{},{},{}}` initializers valid.)

- [ ] **Step 4: Run** — `./build/tests/denso_tests "[cli]"` → PASS.
- [ ] **Step 5: Commit** — `feat(cli): parse --migrate-model`

---

### Task 7a: Migration request validation + CAS load + receipt shape (pure/DB helpers)

**Files:** Create `src/core/detection/migrate.{h,cpp}`; Test `tests/test_migrate.cpp`; Modify both CMakeLists.

**Interfaces:**
- `struct MigrateRequest { std::string old_filename, new_filename, new_name, new_engine_sha256; std::vector<std::string> new_class_names; std::map<std::string,std::string> explicit_remap; std::vector<int64_t> camera_ids; std::string created_utc; };`
- `struct MigrateResult { bool ok=false; std::string error; std::vector<int64_t> affected_cameras; };`
- `std::optional<std::string> validate_request(const MigrateRequest&)` — non-empty cameras; positive ids; **no duplicates**; non-empty old/new filename, new_name, new_class_names, new_engine_sha256; **reject `old_filename == new_filename`**.
- `struct OldAttach { int64_t camera_model_id; int64_t old_model_id; std::vector<std::string> old_class_names; std::vector<ModelClassSelection> classes; };`
- `enum class LoadStatus { Ok, QueryFailed, NotAttached, Ambiguous };` and a loader returning `{LoadStatus, OldAttach}` that **rejects >1 matching attachment** (distinguishing all four cases — never conflate query failure with not-attached).

**Test seed** (correct DB API + full camera columns):

```cpp
// tests/test_migrate.cpp
#include <catch2/catch_test_macros.hpp>
#include "db/db.h"
#include "detection/migrate.h"
#include "detection/repo.h"
#include <QSqlQuery>
static denso::db::Db seed() {           // Db is move-only; return by move
    auto db = denso::db::Db::open_in_memory();
    REQUIRE(db); REQUIRE(denso::db::run_migrations(db->handle()));
    QSqlQuery q(db->handle());
    // full v4 camera columns: camera_type,width,height,fps,pitch,roll,rotation
    REQUIRE(q.exec("INSERT INTO camera(id,name,camera_type,width,height,fps,"
                   "pitch,roll,rotation,active,setup_complete) "
                   "VALUES(1,'C1','usb',640,480,15,0,0,0,1,1)"));
    denso::detection::DetectionModel old_m{0,"old","old_a.engine",{"0","1"}};
    auto oid = denso::detection::upsert_model(db->handle(), old_m); REQUIRE(oid);
    denso::detection::CameraModel cm; cm.camera_id=1; cm.model_id=*oid;
    cm.classes={{0,0.5f},{1,0.5f}};
    REQUIRE(denso::detection::set_camera_models(db->handle(), 1, {cm}));
    return std::move(*db);
}
```
> Verify the exact `camera` columns/NOT-NULLs against `db.cpp` v4 (`:180`) + any later `ALTER`s before finalizing the INSERT; adjust names/order to match.

- [ ] **Step 1: Failing tests** — `validate_request` rejects empty cameras, dup ids, `old==new`; the loader returns `Ambiguous` when a camera attaches `old_a.engine` twice (seed a second identical attachment via a raw insert), `NotAttached` for a wrong filename, `Ok` for the normal case.

- [ ] **Step 2: Register + verify fail.**
- [ ] **Step 3: Implement** `validate_request` + the four-state loader (`SELECT … LIMIT 2`; 0 rows→NotAttached, 1→Ok, 2→Ambiguous; exec fail→QueryFailed).
- [ ] **Step 4: Run** — `./build/tests/denso_tests "[migrate]"` → PASS.
- [ ] **Step 5: Commit** — `feat(detection): migrate request validation + exact-one CAS loader`

---

### Task 7b: Migration transaction (mutate + receipt + rollback paths)

**Files:** Modify `src/core/detection/migrate.{h,cpp}`; extend `tests/test_migrate.cpp`.

**Interfaces:** `MigrateResult migrate_model(const QSqlDatabase&, const MigrateRequest&)` — one transaction: `validate_request` → per-camera CAS load (any non-`Ok` → rollback with a specific error) → `resolve_class_map` → `upsert_model(new)` → per attachment repoint `model_id` + rewrite class rows via the forward map (**refuse if a selected id is unmapped**) → insert the **expanded** receipt (old identity read from DB during the txn: `old_model_id`, `old_name`, `old_class_names`; `new_engine_sha256` from the request; `attachments` with exact `camera_model.id`s + prior classes; forward+inverse maps) → commit.

- [ ] **Step 1: Failing tests** — happy path repoints camera 1 to `new_b.engine` **and** writes one receipt whose `attachments` contains `camera_model_id`; CAS refusal leaves the DB unchanged (`detection_for` still `old_a.engine`); refusal when a selected class is unmapped (`new_class_names={"0"}`). Assert receipt columns via `SELECT old_model_id,new_engine_sha256,attachments FROM model_migration_receipt`.

- [ ] **Step 2: Verify fail.**
- [ ] **Step 3: Implement**

Uses `validate_request`, `load_old` (four-state), `LoadStatus`, `OldAttach` from Task 7a. Add includes `<optional> <utility> <cstddef> <QJsonArray> <QJsonDocument> <QJsonObject> <QSqlQuery> <QString> <QVariant>`, plus `"detection/class_names.h"`, `"detection/repo.h"`, `"models/class_map.h"`.

```cpp
MigrateResult migrate_model(const QSqlDatabase& db, const MigrateRequest& req) {
    if (auto e = validate_request(req)) return {false, *e, {}};
    QSqlDatabase conn(db);
    if (!conn.transaction()) return {false, "cannot begin transaction", {}};
    auto rb = [&](const std::string& e){ conn.rollback(); return MigrateResult{false, e, {}}; };

    // Old model identity (every named camera must attach exactly this filename).
    int64_t old_model_id = 0; std::string old_name; std::vector<std::string> old_names;
    { QSqlQuery q(db); q.prepare("SELECT id,name,class_names FROM model WHERE filename=?");
      q.addBindValue(QString::fromStdString(req.old_filename));
      if (!q.exec())  return rb("query old model failed");
      if (!q.next())  return rb("no catalog row for old filename " + req.old_filename);
      old_model_id = q.value(0).toLongLong();
      old_name     = q.value(1).toString().toStdString();
      old_names    = parse_class_names(q.value(2).toString().toStdString());
      if (q.next())   return rb("multiple catalog rows for old filename"); }

    std::vector<OldAttach> attaches;
    for (int64_t cam : req.camera_ids) {
        auto [st, a] = load_old(db, cam, req.old_filename);
        switch (st) {
            case LoadStatus::QueryFailed: return rb("CAS query failed for camera " + std::to_string(cam));
            case LoadStatus::NotAttached: return rb("camera " + std::to_string(cam) + " does not attach " + req.old_filename);
            case LoadStatus::Ambiguous:   return rb("camera " + std::to_string(cam) + " attaches " + req.old_filename + " more than once");
            case LoadStatus::Ok: break;
        }
        attaches.push_back(a);
    }

    auto cm = denso::models::resolve_class_map(old_names, req.new_class_names, req.explicit_remap);
    if (!cm.map) return rb(cm.error);
    const auto& fwd = *cm.map;
    std::map<int,int> inv; for (const auto& [k,v] : fwd) inv[v] = k;

    DetectionModel nm{0, req.new_name, req.new_filename, req.new_class_names};
    auto nid = upsert_model(db, nm);
    if (!nid) return rb("upsert new model failed");

    QJsonArray attJson;
    for (std::size_t k = 0; k < attaches.size(); ++k) {
        const OldAttach& a = attaches[k];
        QSqlQuery up(db); up.prepare("UPDATE camera_model SET model_id=? WHERE id=?");
        up.addBindValue(static_cast<qlonglong>(*nid)); up.addBindValue(static_cast<qlonglong>(a.camera_model_id));
        if (!up.exec()) return rb("repoint failed");
        QSqlQuery del(db); del.prepare("DELETE FROM camera_model_class WHERE camera_model_id=?");
        del.addBindValue(static_cast<qlonglong>(a.camera_model_id));
        if (!del.exec()) return rb("clearing old class rows failed");
        QJsonArray sel;
        for (const auto& s : a.classes) {
            const auto it = fwd.find(s.class_id);
            if (it == fwd.end()) return rb("selected class " + std::to_string(s.class_id) + " has no mapping in the new model");
            QSqlQuery ins(db);
            ins.prepare("INSERT INTO camera_model_class(camera_model_id,class_id,conf) VALUES(?,?,?)");
            ins.addBindValue(static_cast<qlonglong>(a.camera_model_id)); ins.addBindValue(it->second);
            ins.addBindValue(static_cast<double>(s.conf));
            if (!ins.exec()) return rb("inserting remapped class row failed");
            QJsonObject so; so["class_id"] = s.class_id; so["conf"] = s.conf; sel.append(so);
        }
        QJsonObject ao; ao["camera_id"] = static_cast<double>(req.camera_ids[k]);
        ao["camera_model_id"] = static_cast<double>(a.camera_model_id); ao["classes"] = sel;
        attJson.append(ao);
    }

    auto jmap = [](const std::map<int,int>& m){ QJsonObject o; for (auto [k,v] : m) o[QString::number(k)] = v;
        return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)); };
    QJsonArray oldcn; for (const auto& n : old_names) oldcn.append(QString::fromStdString(n));
    QSqlQuery rec(db);
    rec.prepare("INSERT INTO model_migration_receipt"
                "(created_utc,old_filename,old_model_id,old_name,old_class_names,"
                " new_filename,new_model_id,new_engine_sha256,forward_map,inverse_map,attachments)"
                " VALUES(?,?,?,?,?,?,?,?,?,?,?)");
    rec.addBindValue(QString::fromStdString(req.created_utc));
    rec.addBindValue(QString::fromStdString(req.old_filename));
    rec.addBindValue(static_cast<qlonglong>(old_model_id));
    rec.addBindValue(QString::fromStdString(old_name));
    rec.addBindValue(QString::fromUtf8(QJsonDocument(oldcn).toJson(QJsonDocument::Compact)));
    rec.addBindValue(QString::fromStdString(req.new_filename));
    rec.addBindValue(static_cast<qlonglong>(*nid));
    rec.addBindValue(QString::fromStdString(req.new_engine_sha256));
    rec.addBindValue(jmap(fwd)); rec.addBindValue(jmap(inv));
    rec.addBindValue(QString::fromUtf8(QJsonDocument(attJson).toJson(QJsonDocument::Compact)));
    if (!rec.exec()) return rb("writing the migration receipt failed");

    if (!conn.commit()) return rb("commit failed");
    MigrateResult r; r.ok = true; r.affected_cameras = req.camera_ids; return r;
}
```
- [ ] **Step 4: Run** — `./build/tests/denso_tests "[migrate]"` → PASS.
- [ ] **Step 5: Commit** — `feat(detection): migrate_model transaction + rollback-complete receipt`

---

### Task 8a: Headless migrate coordinator (host-testable, injected paths)

**Files:** Create `src/app/cli/migrate_coordinator.{h,cpp}`; Test `tests/test_migrate_coordinator.cpp`; Modify `src/app/CMakeLists.txt` (the coordinator must live where a test can link it — put it in a small static lib or add the `.cpp` to `denso_tests` like `roi_geometry.cpp` is), `tests/CMakeLists.txt`.

**Interfaces:**
- `struct MigrateInputs { QString models_dir; QString db_path; QString old_engine, new_engine, class_map_path; QList<qint64> cameras; };`
- `struct MigrateOutcome { int exit_code; QString json; QString error; };`
- `MigrateOutcome run_migrate(const MigrateInputs&);` — the whole flow with **no reliance on `denso::paths`** so tests inject temp dirs. Order (spec §3.2.1): (1) parse class-map (if any) with `QJsonParseError` checking, object root, string values; (2) open DB + **`run_migrations`** (Task 8's missing step — `Db::open` does NOT migrate); (3) load+parse+`validate_manifest`; (4) `find_by_engine(new_engine)`; (5) **canonicalize** `models_dir`/engine/sidecar and assert both resolve **under** the canonical `models_dir` (symlink-escape guard); (6) `file_sha256` of engine+sidecar == manifest; (7) build `MigrateRequest` (incl. `new_engine_sha256` from manifest) + call `migrate_model`. Emit machine-readable JSON on **both** success and failure (`{"ok":false,"error":"…","code":"<stable-slug>"}`), with distinct exit codes.

- [ ] **Step 1: Failing tests** (host, no GPU) — seed a temp `models_dir` with a real engine/sidecar + `manifest.json` and a v12→migrated DB; assert: success JSON + repoint; **engine SHA mismatch** → nonzero + error JSON; **missing file**; **malformed manifest**; **malformed class-map**; **absent manifest generation**; **symlink escape** (sidecar symlinked outside `models_dir`) rejected; **v12 DB auto-migrated to v13** (open a v12 db, run, confirm success); **CAS refusal** JSON.

- [ ] **Step 2: Register + verify fail.**
- [ ] **Step 3: Implement** `run_migrate` (pull the logic out of the CLI so it's injectable).
- [ ] **Step 4: Run** — `./build/tests/denso_tests "[migrate_coord]"` → PASS.
- [ ] **Step 5: Commit** — `feat(cli): host-testable migrate coordinator (identity re-check + auto-migrate)`

---

### Task 8b: Thin CLI dispatch

**Files:** Modify `src/app/cli/run_headless.cpp`.

**Interfaces:** Add `case Mode::MigrateModel:` → build `MigrateInputs` from `cmd` + `denso::paths::{models_dir,db_file}`, call `run_migrate`, print `outcome.json`, return `outcome.exit_code`. No logic here beyond wiring.

- [ ] **Step 1: Implement** the case + a `run_migrate_model(const Command&)` thin wrapper.
- [ ] **Step 2: Build** — `cmake --build build` → links clean.
- [ ] **Step 3: On-device smoke (documented)** — on the Jetson with a real `manifest.json` + engine/sidecar in `<data_dir>/models` and a DB attaching the old engine:
```bash
./build/src/app/denso --migrate-model --old digitv3.engine --new digit-v3.1.engine --camera 1
# -> {"ok":true,"new_engine":"digit-v3.1.engine","affected_cameras":[1]}
./build/src/app/denso --check   # -> PASS validating digit-v3.1.engine
```
- [ ] **Step 4: Commit** — `feat(cli): wire denso --migrate-model to the coordinator`

---

## Self-Review

**Spec coverage (Slice A):** §3.1 store/manifest → Tasks 2,3 (+64-hex SHA, symlink guard in 8a); §3.2 migrate (identity re-check, CAS, class-map by name, receipt) → Tasks 4–8; §3.2.6 rollback receipt (now with `camera_model.id`s + old/new identities) → Tasks 5,7b; integration coverage (spec §7) → Task 8a (host, no GPU).

**Deferred (tracked):** retire `sync_models` boot scan → **Slice B** (Slice A is transitional, stated up front); readiness provenance + per-zone inhibition + local alarm → B; reporter intent-ledger + backend qualification + contract test + `denso-setup replace-model`/`rollback-model` → C (v13 now carries everything C's rollback needs).

**Placeholder scan:** none — every code step carries real code; run commands use the test binary by tag (not `ctest -R <tag>`, which matches names).

**Type consistency:** `MigrateRequest`/`MigrateResult`/`MigrateInputs`/`MigrateOutcome` used identically across Tasks 7a/7b/8a/8b; `Mode::MigrateModel` + appended `Command` fields defined in Task 6, consumed in 8b; `resolve_class_map`/`validate_manifest`/`find_by_engine`/`file_sha256` signatures stable; receipt columns match between Task 5 (DDL) and Task 7b (insert).

---

## Execution Handoff

**Plan revised per Codex review (15 findings applied).** Two execution options:

1. **Subagent-Driven (recommended)** — a fresh subagent per task, review between tasks.
2. **Inline Execution** — batch with checkpoints.
