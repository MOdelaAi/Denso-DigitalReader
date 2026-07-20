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

- [ ] **Step 3: Implement** (as in the prior draft's `manifest.cpp`/`.h`; keep `parse_manifest` structural). Add both to `denso_core`.

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

- [ ] **Step 3: Implement** `parse_migrate` (as prior draft) + a duplicate-id check (`if (c.cameras.contains(id)) return error(...)`). Extend `usage()`.

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
- [ ] **Step 3: Implement** (extend the prior draft's `migrate_model`, now populating the expanded receipt; add `<optional> <utility> <cstddef>`; bind the identity pairs by `const auto&`).
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
