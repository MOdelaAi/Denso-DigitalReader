# Deployment Slice 1: Application Primitives — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give the app the four primitives a `.deb` install needs — a single source of truth for mutable paths (`$DENSO_DATA_DIR`), a single-instance guard, and four headless CLI modes — so Slice 2's packaging has something correct to call.

**Architecture:** Three new **pure units in `denso_core`** (`paths/`, `cli/`, `instance/`) so Catch2 reaches them off-device; one read-only DB entry point in the existing `db/`; and one app-side runner in `denso` for the modes that need an inference backend. `main.cpp` gains a dispatch **before** `QApplication` is constructed: headless modes get a `QCoreApplication` (no display) and return; the GUI path constructs `QApplication`, then acquires the lock, then logging, then the DB.

**Tech Stack:** C++20, Qt6 (Core/Sql/Widgets), CMake, Catch2 v3. Dev box: MSYS2 UCRT64.

## Global Constraints

Copied verbatim from `docs/superpowers/specs/2026-07-17-build-package-deployment-design.md`:

- `denso::paths` honors `$DENSO_DATA_DIR`, **defaulting to `applicationDirPath()`** so Windows dev and the test suite are unchanged.
- All four modes are **dispatched before `QApplication` is constructed** (`main.cpp:84` constructs it first today — this is a real refactor, not an added branch).
- `--check` must not: construct `QApplication`; initialize the rotating log sink; acquire the production lock; call `EngineRegistry::warm_up()` (creates `trt_cache`, `engine_registry.cpp:42`); call `sync_models`; run migrations; or open the DB via `Db::open()` (sets `journal_mode=WAL`, `db.cpp:76`).
- `--check` treats **a missing DB as an empty configured-model set, and must never create one.**
- `--check-running` **takes the lock by design** — the sole exemption to the no-lock rule.
- `--check-migrations <db-path>` runs the migration chain against **the given path only**.
- "**No persistent mutation**" is the honest term — a temp probe is still a mutation.
- Data-dir writability is probed with a **real create-and-remove** file test; `access(W_OK)` is weaker and doesn't prove creation succeeds under the actual mount/ACL/quota/read-only conditions.
- `TrtEngine` writes nothing (`trt_engine.cpp:87` `(void)cache_dir;`) → constructing it **directly** is the safe validation path. `trtexec --loadEngine` is not an acceptable substitute.
- `denso_core` must not link `Qt6::Widgets`.
- Add a migration, never edit a shipped one. Schema is at **v11**.
- **Never `git add -A`** — `.gitignore` names `models/digitv2.onnx` specifically, not `models/*.onnx`. Use explicit `git add <files>`.

**Build/test commands** (MSYS2 UCRT64):
```sh
export PATH=/c/msys64/ucrt64/bin:$PATH
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build
```

**Before Task 1**, record the green baseline — every later task must not regress it:
```sh
ctest --test-dir build 2>&1 | tail -3    # note the "N tests passed" number
```

## File Structure

| File | Responsibility |
| --- | --- |
| `src/core/paths/paths.{h,cpp}` **(new)** | The only place that decides where mutable state lives. No Qt Widgets, no I/O — pure string composition over one env var. |
| `src/core/cli/args.{h,cpp}` **(new)** | Pure argv → `Command` parse. No side effects, so it is fully unit-testable. |
| `src/core/instance/single_instance.{h,cpp}` **(new)** | `QLockFile` ownership + the `is_running` probe. Qt Core only. |
| `src/core/db/db.{h,cpp}` (modify) | Add `Db::open_read_only`. Point `default_path()` at `paths::db_file()`. |
| `src/app/cli/run_headless.{h,cpp}` **(new)** | Executes the four modes. Lives in `denso` (not core) because `--check` needs the platform inference backend. |
| `src/app/main.cpp` (modify) | Dispatch before `QApplication`; move every path onto `denso::paths`; acquire the lock. |
| `src/app/ui/startup.cpp` (modify) | `models_dir`/`cache_dir` from `denso::paths`. |
| `tests/test_paths.cpp`, `tests/test_cli_args.cpp`, `tests/test_single_instance.cpp` **(new)** | Catch2 coverage for the three core units. |
| `tests/test_db.cpp` (modify) | Coverage for `open_read_only`. |

---

### Task 1: `denso::paths` — one source of truth for mutable paths

**Files:**
- Create: `src/core/paths/paths.h`, `src/core/paths/paths.cpp`
- Modify: `src/core/CMakeLists.txt` (add `paths/paths.cpp` to `denso_core`)
- Test: `tests/test_paths.cpp` (new), `tests/CMakeLists.txt` (add it)

**Interfaces:**
- Consumes: nothing.
- Produces: `namespace denso::paths` — `QString data_dir()`, `db_file()`, `log_file()`, `models_dir()`, `trt_cache_dir()`, `lock_file()`, `legacy_settings_json()`. All absolute-or-relative `QString`, no trailing slash. Tasks 2, 5 and 6 depend on these exact names.

- [ ] **Step 1: Write the failing test**

Create `tests/test_paths.cpp`. Note the RAII env guard — `qputenv` leaks across Catch2 test cases, and this suite has a history of cross-test pollution, so never set an env var without unsetting it on every exit path:

```cpp
#include <catch2/catch_test_macros.hpp>

#include "paths/paths.h"

#include <QCoreApplication>

namespace {

/// Sets DENSO_DATA_DIR for one test case and always restores it. qputenv leaks
/// into every later test case otherwise.
class EnvGuard {
public:
    explicit EnvGuard(const QByteArray& value) {
        had_ = qEnvironmentVariableIsSet("DENSO_DATA_DIR");
        if (had_) prev_ = qgetenv("DENSO_DATA_DIR");
        qputenv("DENSO_DATA_DIR", value);
    }
    ~EnvGuard() {
        if (had_) qputenv("DENSO_DATA_DIR", prev_);
        else qunsetenv("DENSO_DATA_DIR");
    }
private:
    bool had_ = false;
    QByteArray prev_;
};

/// Unsets DENSO_DATA_DIR for one test case and always restores it.
class EnvUnsetGuard {
public:
    EnvUnsetGuard() {
        had_ = qEnvironmentVariableIsSet("DENSO_DATA_DIR");
        if (had_) prev_ = qgetenv("DENSO_DATA_DIR");
        qunsetenv("DENSO_DATA_DIR");
    }
    ~EnvUnsetGuard() { if (had_) qputenv("DENSO_DATA_DIR", prev_); }
private:
    bool had_ = false;
    QByteArray prev_;
};

} // namespace

using namespace denso::paths;

TEST_CASE("data_dir honors DENSO_DATA_DIR", "[paths]") {
    EnvGuard g("/opt/denso/data");
    REQUIRE(data_dir() == QStringLiteral("/opt/denso/data"));
}

TEST_CASE("data_dir falls back to the application dir when unset", "[paths]") {
    EnvUnsetGuard g;
    REQUIRE(data_dir() == QCoreApplication::applicationDirPath());
}

TEST_CASE("an empty DENSO_DATA_DIR falls back, not to an empty path", "[paths]") {
    EnvGuard g("");
    REQUIRE(data_dir() == QCoreApplication::applicationDirPath());
}

TEST_CASE("a relative DENSO_DATA_DIR is cleaned, not rejected", "[paths]") {
    EnvGuard g("foo/../bar");
    REQUIRE(data_dir() == QStringLiteral("bar"));
}

TEST_CASE("a trailing slash does not double up in derived paths", "[paths]") {
    EnvGuard g("/opt/denso/data/");
    REQUIRE(db_file() == QStringLiteral("/opt/denso/data/denso.db"));
}

TEST_CASE("a filesystem root does not double its separator", "[paths]") {
    // cleanPath() keeps the separator on a root, so naive concatenation would
    // produce "//denso.db". This is why the impl uses QDir::filePath.
    EnvGuard g("/");
    REQUIRE(db_file() == QStringLiteral("/denso.db"));
}

TEST_CASE("every derived path hangs off data_dir", "[paths]") {
    EnvGuard g("/opt/denso/data");
    REQUIRE(db_file()               == QStringLiteral("/opt/denso/data/denso.db"));
    REQUIRE(log_file()              == QStringLiteral("/opt/denso/data/denso.log"));
    REQUIRE(models_dir()            == QStringLiteral("/opt/denso/data/models"));
    REQUIRE(trt_cache_dir()         == QStringLiteral("/opt/denso/data/models/trt_cache"));
    REQUIRE(lock_file()             == QStringLiteral("/opt/denso/data/denso.lock"));
    REQUIRE(legacy_settings_json()  == QStringLiteral("/opt/denso/data/settings.json"));
}
```

Register it in `tests/CMakeLists.txt` — add `test_paths.cpp` to the `add_executable(denso_tests ...)` source list, directly after `test_db.cpp`.

- [ ] **Step 2: Run test to verify it fails**

```sh
export PATH=/c/msys64/ucrt64/bin:$PATH
cmake -S . -B build -G Ninja && cmake --build build
```
Expected: **compile error** — `fatal error: paths/paths.h: No such file or directory`.

- [ ] **Step 3: Write minimal implementation**

Create `src/core/paths/paths.h`:

```cpp
// The single source of truth for every MUTABLE path the app owns: the database,
// the rotating log, the model/engine dir, the TensorRT cache, and the instance
// lock. Nothing else may compose these paths.
//
// Resolution: $DENSO_DATA_DIR when set and non-empty, else the directory holding
// the executable (the historical behavior — so Windows dev and the test suite are
// unchanged), else "." when there is no QCoreApplication yet.
//
// Why this exists: an installed build's program dir is root-owned and is
// REPLACED on upgrade, so state kept beside the executable would be unwritable
// at runtime and destroyed on every package upgrade. The launcher points
// DENSO_DATA_DIR at /opt/denso/data.
#pragma once

#include <QString>

namespace denso::paths {

/// The mutable-state root. Never has a trailing slash.
QString data_dir();

QString db_file();               ///< <data>/denso.db
QString log_file();              ///< <data>/denso.log (rotated siblings: .1 … .4)
QString models_dir();            ///< <data>/models
QString trt_cache_dir();         ///< <data>/models/trt_cache
QString lock_file();             ///< <data>/denso.lock (single-instance guard)
QString legacy_settings_json();  ///< <data>/settings.json (one-time pre-SQLite import)

} // namespace denso::paths
```

Create `src/core/paths/paths.cpp`:

```cpp
#include "paths/paths.h"

#include <QCoreApplication>
#include <QDir>

namespace denso::paths {

QString data_dir() {
    // qEnvironmentVariable returns an empty string for BOTH unset and
    // set-but-empty; both must fall back, never yield an empty path.
    const QString env = qEnvironmentVariable("DENSO_DATA_DIR");
    if (!env.isEmpty()) {
        // cleanPath resolves ".."/"." and strips a trailing slash, so the
        // derived paths below can concatenate unconditionally.
        return QDir::cleanPath(env);
    }
    const QString dir = QCoreApplication::applicationDirPath();
    if (dir.isEmpty()) {
        // No QCoreApplication yet (or no argv[0]) — mirror db::default_path()'s
        // historical fallback rather than returning an empty path.
        return QStringLiteral(".");
    }
    return dir;
}

// QDir::filePath, NOT string concatenation: cleanPath() strips a trailing
// separator from every path EXCEPT a filesystem root, so "/" + "/denso.db" would
// yield "//denso.db" (and "C:/" → "C://denso.db").
QString db_file()              { return QDir(data_dir()).filePath(QStringLiteral("denso.db")); }
QString log_file()             { return QDir(data_dir()).filePath(QStringLiteral("denso.log")); }
QString models_dir()           { return QDir(data_dir()).filePath(QStringLiteral("models")); }
QString trt_cache_dir()        { return QDir(models_dir()).filePath(QStringLiteral("trt_cache")); }
QString lock_file()            { return QDir(data_dir()).filePath(QStringLiteral("denso.lock")); }
QString legacy_settings_json() { return QDir(data_dir()).filePath(QStringLiteral("settings.json")); }

} // namespace denso::paths
```

In `src/core/CMakeLists.txt`, add to the `add_library(denso_core STATIC ...)` source list, immediately before `db/db.cpp`:

```cmake
    paths/paths.cpp
```

- [ ] **Step 4: Run tests to verify they pass**

```sh
cmake --build build && ./build/tests/denso_tests "[paths]"
```
Expected: PASS — 6 test cases. Then run the full suite and confirm the baseline number from the header only went **up**:
```sh
ctest --test-dir build 2>&1 | tail -3
```

- [ ] **Step 5: Commit**

```bash
git add src/core/paths/paths.h src/core/paths/paths.cpp src/core/CMakeLists.txt tests/test_paths.cpp tests/CMakeLists.txt
git commit -m "feat(core): add denso::paths — one source of truth for mutable paths

\$DENSO_DATA_DIR, defaulting to applicationDirPath() so Windows dev and the
tests are unchanged. Nothing is wired to it yet (Task 5)."
```

---

### Task 2: Honest DB inspection — `Db::open_read_only` + an error-preserving model query

**Files:**
- Modify: `src/core/db/db.h` (add `open_read_only`; retarget `default_path` docs), `src/core/db/db.cpp`
- Modify: `src/core/detection/repo.h`, `src/core/detection/repo.cpp` (add `try_attached_model_filenames`)
- Test: `tests/test_db.cpp` (append), `tests/test_detection_repo.cpp` (append)

**Interfaces:**
- Consumes: `denso::paths::db_file()` (Task 1).
- Produces (both used by Task 6):
  - `static std::optional<Db> Db::open_read_only(const QString& path);` — `nullopt` when the file is absent or unreadable, **never creates it**.
  - `std::optional<std::vector<std::string>> denso::detection::try_attached_model_filenames(const QSqlDatabase& db);` — `nullopt` on **query failure**, an empty vector for **no attachments**.

**Why the second one:** the existing `attached_model_filenames` (`src/core/detection/repo.cpp:48`) returns an empty vector *both* when the query succeeds with no rows *and* when `exec()` fails (`repo.cpp:53` — `if (!q.exec(...)) return out;`). A present-but-corrupt database would therefore pass `--check` as though it were a fresh install. A validation gate cannot conflate "nothing configured" with "I couldn't read it".

**The `--check` mutation guarantee, stated precisely.** "No persistent mutation" is *not* unconditionally achievable against a WAL database, and the plan must not pretend otherwise:
- `QSQLITE_OPEN_READONLY` maps to `SQLITE_OPEN_READONLY`, which fails rather than creating an absent primary DB. Good.
- But a WAL reader needs the `-shm` shared-memory index. If it doesn't exist, SQLite may need to create it (requiring directory write access), and it can survive abnormal termination.
- `immutable=1` is **not** a safe fix: it lets SQLite ignore WAL state and read a stale image — potentially missing the very camera-model rows `--check` exists to validate.

So the guarantee is: **`--check` does not mutate the primary database and creates no root-owned artifacts; SQLite may create target-user-owned WAL support files.** That is why the run-as-target-user rule is mandatory and not merely tidy — it bounds ownership, not mutation.

- [ ] **Step 1: Write the failing test**

Append to `tests/test_db.cpp` (it already includes `db/db.h`, `QDir`, `QFile`, `QSqlQuery`, `QVariant`). Add `#include <QTemporaryDir>` to the include block at the top:

```cpp
TEST_CASE("open_read_only does not create a missing database", "[db][readonly]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString missing = dir.filePath(QStringLiteral("absent.db"));

    // The --check contract: a missing DB is an empty configured-model set. It
    // must NOT be conjured into existence by the act of checking.
    REQUIRE_FALSE(Db::open_read_only(missing).has_value());
    REQUIRE_FALSE(QFile::exists(missing));
}

TEST_CASE("open_read_only reads but refuses writes", "[db][readonly]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("ro.db"));

    {
        auto rw = Db::open(path);
        REQUIRE(rw.has_value());
        REQUIRE(run_migrations(rw->handle()));
    }

    auto ro = Db::open_read_only(path);
    REQUIRE(ro.has_value());

    QSqlQuery read(ro->handle());
    REQUIRE(read.exec(QStringLiteral("SELECT count(*) FROM camera")));
    REQUIRE(read.next());

    QSqlQuery write(ro->handle());
    REQUIRE_FALSE(write.exec(QStringLiteral(
        "INSERT INTO settings (key, value) VALUES ('k', 'v')")));
}

TEST_CASE("open_read_only leaves the primary database byte-identical", "[db][readonly]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("journal.db"));

    {
        auto rw = Db::open(path);
        REQUIRE(rw.has_value());
        REQUIRE(run_migrations(rw->handle()));
    }

    // Hash the file itself rather than re-opening and asking PRAGMA
    // journal_mode: Db::open() forces WAL again, which would mask exactly the
    // mutation we're trying to detect.
    const auto digest = [&path] {
        QFile f(path);
        REQUIRE(f.open(QIODevice::ReadOnly));
        QCryptographicHash h(QCryptographicHash::Sha256);
        REQUIRE(h.addData(&f));
        return h.result();
    };

    const QByteArray before = digest();
    {
        auto ro = Db::open_read_only(path);
        REQUIRE(ro.has_value());
        QSqlQuery q(ro->handle());
        REQUIRE(q.exec(QStringLiteral("SELECT count(*) FROM camera")));
        REQUIRE(q.next());
    }
    REQUIRE(digest() == before);
}

TEST_CASE("open_read_only sets query_only", "[db][readonly]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("qo.db"));
    {
        auto rw = Db::open(path);
        REQUIRE(rw.has_value());
        REQUIRE(run_migrations(rw->handle()));
    }

    auto ro = Db::open_read_only(path);
    REQUIRE(ro.has_value());
    QSqlQuery q(ro->handle());
    REQUIRE(q.exec(QStringLiteral("PRAGMA query_only")));
    REQUIRE(q.next());
    REQUIRE(q.value(0).toInt() == 1);
}
```

Add `#include <QCryptographicHash>` to the include block of `tests/test_db.cpp`.

Also append to `tests/test_detection_repo.cpp` (match the includes and helpers already at the top of that file; it opens in-memory DBs via `Db::open_in_memory()` + `run_migrations`):

```cpp
TEST_CASE("try_attached_model_filenames distinguishes empty from unreadable",
          "[detection][repo]") {
    auto db = denso::db::Db::open_in_memory();
    REQUIRE(db.has_value());

    SECTION("valid schema with no attachments yields an empty vector, not nullopt") {
        REQUIRE(denso::db::run_migrations(db->handle()));
        const auto got = denso::detection::try_attached_model_filenames(db->handle());
        REQUIRE(got.has_value());
        REQUIRE(got->empty());
    }

    SECTION("a missing schema yields nullopt, NOT an empty vector") {
        // No migrations: camera_model/model do not exist, so the query fails.
        // The old attached_model_filenames() returns {} here, which would let a
        // corrupt database pass --check as if it were a fresh install.
        REQUIRE_FALSE(
            denso::detection::try_attached_model_filenames(db->handle()).has_value());
    }
}
```

- [ ] **Step 2: Run test to verify it fails**

```sh
cmake --build build
```
Expected: **compile error** — `'open_read_only' is not a member of 'denso::db::Db'`.

- [ ] **Step 3: Write minimal implementation**

In `src/core/db/db.h`, add directly below the `open_in_memory` declaration:

```cpp
    /// Open an EXISTING database read-only, for inspection that must not mutate
    /// it. Unlike open(), this does NOT run `PRAGMA journal_mode = WAL` — that
    /// pragma rewrites the file header, which is a mutation. Returns nullopt if
    /// the file is absent or unreadable, and never creates it (the --check
    /// contract: a missing DB is an empty configured-model set).
    ///
    /// Guarantee, stated precisely: this does not mutate the PRIMARY database.
    /// It is NOT unconditionally side-effect-free — a WAL reader needs the `-shm`
    /// index and SQLite may create it (and it can outlive an abnormal exit). So
    /// any root-side caller MUST drop to the target user first: that bounds
    /// OWNERSHIP, not mutation, and a root-owned `-shm` in an operator-owned data
    /// dir breaks the app. Do not "fix" this with `immutable=1` — that lets
    /// SQLite ignore WAL state and read a stale image, which could hide the very
    /// camera-model rows a caller is validating.
    static std::optional<Db> open_read_only(const QString& path);
```

In `src/core/db/db.cpp`, add after `Db::open`:

```cpp
std::optional<Db> Db::open_read_only(const QString& path) {
    const QString name = next_connection_name();
    bool ok = false;
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name);
        db.setDatabaseName(path);
        // QSQLITE_OPEN_READONLY maps to SQLITE_OPEN_READONLY, which fails rather
        // than creating an absent file — exactly the contract we want.
        db.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY"));
        ok = db.open();
        // Deliberately NO journal_mode pragma here (see the header).
        if (ok) {
            // Belt and braces: READONLY already rejects writes, but query_only
            // makes the intent explicit and is what the spec calls for.
            ok = QSqlQuery(db).exec(QStringLiteral("PRAGMA query_only = ON"));
        }
    }
    if (!ok) {
        QSqlDatabase::removeDatabase(name);
        return std::nullopt;
    }
    return Db(name);
}
```

In `src/core/detection/repo.h`, add next to the existing `attached_model_filenames` declaration:

```cpp
/// Like attached_model_filenames, but distinguishes "no attachments" (an empty
/// vector) from "the query failed" (nullopt) — attached_model_filenames returns
/// {} for both, which would let a corrupt database look like a fresh install to
/// a validation gate. Prefer this wherever the difference matters.
std::optional<std::vector<std::string>>
try_attached_model_filenames(const QSqlDatabase& db);
```
and add `#include <optional>` to that header's include block.

In `src/core/detection/repo.cpp`, add beside the existing function, and re-express the old one in terms of it so the query lives in exactly one place:

```cpp
std::optional<std::vector<std::string>>
try_attached_model_filenames(const QSqlDatabase& db) {
    std::vector<std::string> out;
    QSqlQuery q(db);
    if (!q.exec(QStringLiteral(
            "SELECT DISTINCT m.filename FROM camera_model cm "
            "JOIN model m ON m.id = cm.model_id ORDER BY m.filename"))) {
        return std::nullopt;
    }
    while (q.next()) {
        out.push_back(q.value(0).toString().toStdString());
    }
    return out;
}

std::vector<std::string> attached_model_filenames(const QSqlDatabase& db) {
    return try_attached_model_filenames(db).value_or(std::vector<std::string>{});
}
```
(Delete the old body of `attached_model_filenames`; its behavior is unchanged for every existing caller.)

- [ ] **Step 4: Run tests to verify they pass**

```sh
cmake --build build
./build/tests/denso_tests "[db][readonly]"
./build/tests/denso_tests "[detection][repo]"
```
Expected: PASS, including the four new cases. (Run the Catch2 binary with **tags** — `ctest -R "db"` filters on CTest *test names*, which are the `TEST_CASE` strings, so a tag is not a reliable filter.)

If "refuses writes" fails: confirm the connect option string is exactly `QSQLITE_OPEN_READONLY` and that it is set **before** `db.open()`.

- [ ] **Step 5: Commit**

```bash
git add src/core/db/db.h src/core/db/db.cpp src/core/detection/repo.h src/core/detection/repo.cpp tests/test_db.cpp tests/test_detection_repo.cpp
git commit -m "feat(core): honest DB inspection for the --check gate

Db::open_read_only: open() runs PRAGMA journal_mode=WAL (db.cpp:76), which
rewrites the file header -- a mutation, so it cannot serve --check. The
read-only path skips the pragma, sets query_only, and never creates an absent
file (a missing DB is an empty configured-model set).

try_attached_model_filenames: the existing attached_model_filenames returns {}
BOTH for 'no attachments' and for 'the query failed' (repo.cpp:53), so a corrupt
DB would pass --check as a fresh install. The new API preserves the difference;
the old one now delegates to it, unchanged for existing callers."
```

---

### Task 3: `SingleInstance` — the lock guard

**Files:**
- Create: `src/core/instance/single_instance.h`, `src/core/instance/single_instance.cpp`
- Modify: `src/core/CMakeLists.txt`
- Test: `tests/test_single_instance.cpp` (new), `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `denso::paths::lock_file()` (Task 1).
- Produces: `namespace denso::instance` — `class SingleInstance` with `explicit SingleInstance(QString lock_path)`, `bool acquire()`, `bool is_held() const`, and `static bool is_running(const QString& lock_path)`. Tasks 5 and 6 use these exact names.

**Why this is v1-mandatory** (from the spec): autostart plus a clickable menu icon guarantees two processes eventually — duplicate camera opens, competing network config, SQLite write contention, and silent log loss, because rename-based rotation does not move another process's open file descriptor to the new pathname.

- [ ] **Step 1: Write the failing test**

Create `tests/test_single_instance.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>

#include "instance/single_instance.h"

#include <QFile>
#include <QTemporaryDir>

using denso::instance::SingleInstance;

TEST_CASE("a second acquire on the same lock fails", "[instance]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString lock = dir.filePath(QStringLiteral("denso.lock"));

    SingleInstance first(lock);
    REQUIRE(first.acquire());
    REQUIRE(first.is_held());

    SingleInstance second(lock);
    REQUIRE_FALSE(second.acquire());
    REQUIRE_FALSE(second.is_held());
}

TEST_CASE("is_running reports false when nothing holds the lock", "[instance]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString lock = dir.filePath(QStringLiteral("denso.lock"));

    REQUIRE_FALSE(SingleInstance::is_running(lock));
}

TEST_CASE("is_running reports true while the lock is held", "[instance]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString lock = dir.filePath(QStringLiteral("denso.lock"));

    SingleInstance held(lock);
    REQUIRE(held.acquire());
    REQUIRE(SingleInstance::is_running(lock));
}

TEST_CASE("the lock is released on destruction", "[instance]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString lock = dir.filePath(QStringLiteral("denso.lock"));

    {
        SingleInstance scoped(lock);
        REQUIRE(scoped.acquire());
    }
    REQUIRE_FALSE(SingleInstance::is_running(lock));

    SingleInstance again(lock);
    REQUIRE(again.acquire());
}

TEST_CASE("is_running leaves no lock file behind when nothing holds it", "[instance]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString lock = dir.filePath(QStringLiteral("denso.lock"));

    // is_running is the ONE mode allowed to touch the lock (it must tryLock to
    // answer), but it must not leave a corpse that later looks like an owner.
    REQUIRE_FALSE(SingleInstance::is_running(lock));
    REQUIRE_FALSE(QFile::exists(lock));
}
```

Register `test_single_instance.cpp` in `tests/CMakeLists.txt` next to `test_paths.cpp`.

- [ ] **Step 2: Run test to verify it fails**

```sh
cmake --build build
```
Expected: **compile error** — `fatal error: instance/single_instance.h: No such file or directory`.

- [ ] **Step 3: Write minimal implementation**

Create `src/core/instance/single_instance.h`:

```cpp
// Single-instance guard over QLockFile.
//
// Why: the appliance autostarts AND has a clickable menu icon, so a second
// process is inevitable. Two processes would open the same cameras, compete to
// write network config, contend on one SQLite file, and silently eat log data —
// rename-based rotation does not move another process's open fd to the new
// pathname, so the loser keeps writing into denso.log.1 after the winner rotates.
//
// Acquire this BEFORE the DB opens, logging initializes, or cameras start.
#pragma once

#include <QString>

#include <memory>

class QLockFile;

namespace denso::instance {

class SingleInstance {
public:
    explicit SingleInstance(QString lock_path);
    ~SingleInstance();

    SingleInstance(const SingleInstance&) = delete;
    SingleInstance& operator=(const SingleInstance&) = delete;

    /// Try to become the one live instance. Non-blocking. Idempotent: calling it
    /// again while already held returns true without re-locking.
    bool acquire();

    bool is_held() const;

    /// True if some process currently holds `lock_path`.
    ///
    /// This is the SOLE exemption to "checks never take the production lock":
    /// answering the question requires tryLock(), which briefly acquires and
    /// releases when nothing is running. It must therefore run as the target
    /// user — as root it would leave a root-owned lock artifact in an
    /// operator-owned data dir, the exact poisoning the data-dir rules prevent.
    static bool is_running(const QString& lock_path);

private:
    QString path_;
    std::unique_ptr<QLockFile> lock_;
    bool held_ = false;
};

} // namespace denso::instance
```

Create `src/core/instance/single_instance.cpp`:

```cpp
#include "instance/single_instance.h"

#include <QLockFile>

#include <utility>

namespace denso::instance {

SingleInstance::SingleInstance(QString lock_path)
    : path_(std::move(lock_path)), lock_(std::make_unique<QLockFile>(path_)) {
    // QLockFile's default 30s staleness heuristic is what recovers the lock
    // after a hard kill: it reads the recorded pid and reclaims only if that
    // process is gone. Keep the default — a 0 here would mean "never stale".
}

SingleInstance::~SingleInstance() = default;  // ~QLockFile unlocks + removes

bool SingleInstance::acquire() {
    if (held_) return true;
    held_ = lock_->tryLock(0);  // 0 = do not wait
    return held_;
}

bool SingleInstance::is_held() const { return held_; }

bool SingleInstance::is_running(const QString& lock_path) {
    QLockFile probe(lock_path);
    if (probe.tryLock(0)) {
        probe.unlock();  // removes the file we just made — leave no corpse
        return false;
    }
    return true;
}

} // namespace denso::instance
```

In `src/core/CMakeLists.txt`, add to the `denso_core` source list after `paths/paths.cpp`:

```cmake
    instance/single_instance.cpp
```

- [ ] **Step 4: Run tests to verify they pass**

```sh
cmake --build build && ./build/tests/denso_tests "[instance]"
```
Expected: PASS — 5 test cases.

**If "a second acquire on the same lock fails" FAILS:** `QLockFile` is documented for *inter*-process use. The expected behavior is that the second one reads the recorded pid, sees our own live process, and refuses to reclaim — so the test should pass as written.

If it does **not**: do **not** weaken the test. **Stop and report** — the guard is then unsound in-process and the fix is a design decision, not an improvisation. (For reference, the shape would be a file-scope `std::mutex` guarding a `std::set<QString>` of canonical held paths, consulted before `tryLock` and erased on release/destruction — *not* a single `atomic_flag`, which cannot be keyed by path.)

**Coverage note:** these cases prove same-process behavior only. The real contract is *inter*-process and is proven by the Slice 1 exit criteria (launching a second `denso` while one runs) and by Slice 2's `prerm` refusing an upgrade under a live app.

- [ ] **Step 5: Commit**

```bash
git add src/core/instance/single_instance.h src/core/instance/single_instance.cpp src/core/CMakeLists.txt tests/test_single_instance.cpp tests/CMakeLists.txt
git commit -m "feat(core): add SingleInstance lock guard

Autostart + a menu icon guarantees two processes; two writers on one
rename-rotated log silently eat data. Not wired up yet (Task 5)."
```

---

### Task 4: `cli::parse` — pure argument parsing

**Files:**
- Create: `src/core/cli/args.h`, `src/core/cli/args.cpp`
- Modify: `src/core/CMakeLists.txt`
- Test: `tests/test_cli_args.cpp` (new), `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: nothing.
- Produces: `namespace denso::cli` — `enum class Mode { Gui, Version, Check, CheckRunning, CheckMigrations, Error }`, `struct Command { Mode mode = Mode::Gui; QString arg; QStringList engines; QString error; }`, `Command parse(const QStringList& args)` (args **exclude** `argv[0]`), `bool is_headless(Mode m)`, `QString usage()`. Tasks 5 and 6 use these exact names.

**Why `--check` takes repeatable `--engine <filename>`:** on a **fresh install** there is no database, so the configured-model set is empty — and a `--check` that only validates configured models would load **zero engines** and pass, even with a corrupt packaged `digitv2.engine`. The spec makes package engines an activation blocker too. Scanning `models/*.engine` instead is wrong: an unrelated operator engine must not block an upgrade. So the caller names them, and Slice 2's `denso-setup` passes the ones from its tracked package manifest.

- [ ] **Step 1: Write the failing test**

Create `tests/test_cli_args.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>

#include "cli/args.h"

using denso::cli::Command;
using denso::cli::Mode;
using denso::cli::is_headless;
using denso::cli::parse;

TEST_CASE("no arguments means the GUI", "[cli]") {
    const Command c = parse({});
    REQUIRE(c.mode == Mode::Gui);
    REQUIRE_FALSE(is_headless(c.mode));
}

TEST_CASE("each headless flag maps to its mode", "[cli]") {
    REQUIRE(parse({QStringLiteral("--version")}).mode == Mode::Version);
    REQUIRE(parse({QStringLiteral("--check")}).mode == Mode::Check);
    REQUIRE(parse({QStringLiteral("--check-running")}).mode == Mode::CheckRunning);
}

TEST_CASE("every non-GUI mode is headless", "[cli]") {
    REQUIRE(is_headless(Mode::Version));
    REQUIRE(is_headless(Mode::Check));
    REQUIRE(is_headless(Mode::CheckRunning));
    REQUIRE(is_headless(Mode::CheckMigrations));
    // Error prints usage and exits — it must not open a window either.
    REQUIRE(is_headless(Mode::Error));
}

TEST_CASE("parse: --check-migrations carries its db path", "[cli]") {
    const Command c = parse({QStringLiteral("--check-migrations"),
                             QStringLiteral("/tmp/copy.db")});
    REQUIRE(c.mode == Mode::CheckMigrations);
    REQUIRE(c.arg == QStringLiteral("/tmp/copy.db"));
}

TEST_CASE("parse: --check-migrations without a path is an error, not a GUI launch", "[cli]") {
    const Command c = parse({QStringLiteral("--check-migrations")});
    REQUIRE(c.mode == Mode::Error);
    REQUIRE_FALSE(c.error.isEmpty());
}

TEST_CASE("an unknown flag is an error, not a silent GUI launch", "[cli]") {
    const Command c = parse({QStringLiteral("--wat")});
    REQUIRE(c.mode == Mode::Error);
    REQUIRE(c.error.contains(QStringLiteral("--wat")));
}

TEST_CASE("a trailing extra argument is an error", "[cli]") {
    REQUIRE(parse({QStringLiteral("--check"), QStringLiteral("junk")}).mode == Mode::Error);
    REQUIRE(parse({QStringLiteral("--check-migrations"), QStringLiteral("a"),
                   QStringLiteral("b")}).mode == Mode::Error);
}

TEST_CASE("parse: --check takes zero or more --engine names", "[cli]") {
    SECTION("none") {
        const Command c = parse({QStringLiteral("--check")});
        REQUIRE(c.mode == Mode::Check);
        REQUIRE(c.engines.isEmpty());
    }
    SECTION("one") {
        const Command c = parse({QStringLiteral("--check"), QStringLiteral("--engine"),
                                 QStringLiteral("digitv2.engine")});
        REQUIRE(c.mode == Mode::Check);
        REQUIRE(c.engines == QStringList{QStringLiteral("digitv2.engine")});
    }
    SECTION("repeated") {
        const Command c = parse({QStringLiteral("--check"),
                                 QStringLiteral("--engine"), QStringLiteral("a.engine"),
                                 QStringLiteral("--engine"), QStringLiteral("b.engine")});
        REQUIRE(c.mode == Mode::Check);
        REQUIRE(c.engines == QStringList{QStringLiteral("a.engine"), QStringLiteral("b.engine")});
    }
}

TEST_CASE("parse: --engine without a value is an error", "[cli]") {
    REQUIRE(parse({QStringLiteral("--check"), QStringLiteral("--engine")}).mode == Mode::Error);
}

TEST_CASE("parse: --engine only applies to --check", "[cli]") {
    REQUIRE(parse({QStringLiteral("--version"), QStringLiteral("--engine"),
                   QStringLiteral("a.engine")}).mode == Mode::Error);
}
```

Register `test_cli_args.cpp` in `tests/CMakeLists.txt`.

**Test names must not START with `--`.** `catch_discover_tests` registers each
case by its literal name and re-invokes the binary with that name as a positional
selector; Catch2's own CLI parser then eats a leading `--` as an unrecognized
option, and the case reports Failed under `ctest` while the code is fine. An
earlier draft of this plan hit exactly that: 5 correct tests went red. Hence the
`parse: ` prefix.

- [ ] **Step 2: Run test to verify it fails**

```sh
cmake --build build
```
Expected: **compile error** — `fatal error: cli/args.h: No such file or directory`.

- [ ] **Step 3: Write minimal implementation**

Create `src/core/cli/args.h`:

```cpp
// Pure command-line parsing: argv → Command. No side effects, no I/O, no Qt
// application object — so main() can decide BEFORE constructing QApplication
// whether this run needs a GUI at all. A headless mode must never load the xcb
// platform plugin (the installer calls these with no display).
#pragma once

#include <QString>
#include <QStringList>

namespace denso::cli {

enum class Mode {
    Gui,              ///< no flags — the normal application
    Version,          ///< --version
    Check,            ///< --check
    CheckRunning,     ///< --check-running
    CheckMigrations,  ///< --check-migrations <db-path>
    Error,            ///< bad usage; `error` says why
};

struct Command {
    Mode mode = Mode::Gui;
    QString arg;         ///< CheckMigrations: the db path to migrate
    QStringList engines; ///< Check: extra engine filenames to validate (--engine)
    QString error;       ///< Error: the human-readable reason
};

/// `args` EXCLUDES argv[0].
Command parse(const QStringList& args);

/// True for everything except Gui — including Error, which prints usage.
bool is_headless(Mode m);

QString usage();

} // namespace denso::cli
```

Create `src/core/cli/args.cpp`:

```cpp
#include "cli/args.h"

namespace denso::cli {

QString usage() {
    return QStringLiteral(
        "usage: denso [--version | --check [--engine <file>]... |\n"
        "              --check-running | --check-migrations <db-path>]\n"
        "\n"
        "  (no flags)                 run the application\n"
        "  --version                  print the version and exit\n"
        "  --check                    validate the data dir + every engine the DB\n"
        "                             references, plus each --engine named here\n"
        "                             (does not mutate the primary database)\n"
        "  --engine <file>            repeatable; a models/ filename --check must\n"
        "                             validate even when no DB references it\n"
        "  --check-running            exit 0 if an instance holds the lock, 1 if not\n"
        "  --check-migrations <db>    run the migration chain against <db> ONLY\n");
}

bool is_headless(Mode m) { return m != Mode::Gui; }

namespace {

Command error(const QString& why) { return Command{Mode::Error, {}, {}, why}; }

/// --check [--engine <file>]...  — the only mode taking trailing options.
Command parse_check(const QStringList& rest) {
    Command c;
    c.mode = Mode::Check;
    for (int i = 0; i < rest.size(); ++i) {
        if (rest.at(i) != QStringLiteral("--engine")) {
            return error(QStringLiteral("unexpected argument after --check: %1")
                             .arg(rest.at(i)));
        }
        if (i + 1 >= rest.size()) {
            return error(QStringLiteral("--engine requires a models/ filename"));
        }
        c.engines << rest.at(++i);
    }
    return c;
}

} // namespace

Command parse(const QStringList& args) {
    if (args.isEmpty()) return Command{Mode::Gui, {}, {}, {}};

    const QString& flag = args.first();
    const QStringList rest = args.mid(1);

    if (flag == QStringLiteral("--check")) return parse_check(rest);

    if (flag == QStringLiteral("--check-migrations")) {
        if (rest.size() == 1) return Command{Mode::CheckMigrations, rest.first(), {}, {}};
        return error(QStringLiteral("--check-migrations requires exactly one "
                                    "database path"));
    }

    if (rest.isEmpty()) {
        if (flag == QStringLiteral("--version"))       return Command{Mode::Version, {}, {}, {}};
        if (flag == QStringLiteral("--check-running")) return Command{Mode::CheckRunning, {}, {}, {}};
    }

    return error(QStringLiteral("unknown or malformed option: %1").arg(flag));
}

} // namespace denso::cli
```

In `src/core/CMakeLists.txt`, add to the `denso_core` source list after `instance/single_instance.cpp`:

```cmake
    cli/args.cpp
```

- [ ] **Step 4: Run tests to verify they pass**

```sh
cmake --build build && ./build/tests/denso_tests "[cli]"
```
Expected: PASS — 7 test cases.

- [ ] **Step 5: Commit**

```bash
git add src/core/cli/args.h src/core/cli/args.cpp src/core/CMakeLists.txt tests/test_cli_args.cpp tests/CMakeLists.txt
git commit -m "feat(core): add pure cli::parse for the headless modes

Pure argv -> Command so main() can dispatch BEFORE constructing QApplication;
a headless mode must never load xcb. Not wired up yet (Task 5)."
```

---

### Task 5: Wire it up — dispatch before `QApplication`, paths everywhere, lock on the GUI path

**Files:**
- Create: `src/app/cli/run_headless.h`, `src/app/cli/run_headless.cpp`
- Modify: `src/app/main.cpp` (dispatch + paths + lock), `src/app/ui/startup.cpp:111-114` (paths), `src/core/db/db.cpp` (`default_path` → `paths::db_file`), `src/app/CMakeLists.txt`

**Interfaces:**
- Consumes: `denso::cli::{parse,is_headless,usage,Command,Mode}` (Task 4); `denso::paths::*` (Task 1); `denso::instance::SingleInstance` (Task 3); `denso::db::Db::open` + `run_migrations` (existing).
- Produces: `namespace denso::app { int run_headless(const denso::cli::Command& cmd); }` — Task 6 fills in its `Check` branch.

**Exit codes** (Slice 2's `denso-setup` and the maintainer scripts depend on these):

| Code | Meaning |
| --- | --- |
| 0 | success — and for `--check-running`, "an instance IS running" |
| 1 | check failed — and for `--check-running`, "no instance is running" |
| 2 | bad usage |
| 3 | GUI refused to start: another instance already holds the lock |

- [ ] **Step 1: Write the failing test**

This task is main-loop wiring; its gate is behavioral, not a Catch2 case. Write the check as a script `tests/manual/slice1_modes.sh` so it is repeatable:

```sh
#!/usr/bin/env bash
# Slice 1 behavioral gate. Run from the repo root after a build.
set -u
EXE=build/src/app/denso.exe
[ -f "$EXE" ] || EXE=build/src/app/denso
fail=0
chk() { # chk <description> <expected-rc> <actual-rc>
  if [ "$2" = "$3" ]; then echo "ok   - $1"; else echo "FAIL - $1 (want rc=$2, got rc=$3)"; fail=1; fi
}
# BEFORE the CLI dispatch exists, every one of these flags falls through and
# opens the GUI, which blocks forever. Bound each run so the red test FAILS fast
# instead of hanging the worker's terminal (timeout returns 124).
run() { timeout 20 "$EXE" "$@" >/dev/null 2>&1; }

TMP=$(mktemp -d)
export DENSO_DATA_DIR="$TMP"
# Keep backend/tmp caches inside the sandbox so a stray write can't land
# somewhere the assertions below don't look.
export TMPDIR="$TMP/tmp"; mkdir -p "$TMPDIR"
export CUDA_CACHE_PATH="$TMP/cuda-cache"

run --version;            chk "--version exits 0" 0 $?
run --wat;                chk "unknown flag exits 2" 2 $?
run --check-migrations;   chk "--check-migrations without path exits 2" 2 $?
run --check-running;      chk "--check-running with nothing running exits 1" 1 $?
[ -f "$TMP/denso.lock" ] && { echo "FAIL - --check-running left a lock corpse"; fail=1; } || echo "ok   - --check-running left no lock"

# --version must not create ANY state in the data dir.
if [ -z "$(ls -A "$TMP")" ]; then echo "ok   - --version left the data dir empty"; else echo "FAIL - --version created: $(ls -A "$TMP")"; fail=1; fi

# --check-migrations builds the chain in a throwaway db and touches nothing else.
run --check-migrations "$TMP/copy.db"; chk "--check-migrations exits 0" 0 $?
[ -f "$TMP/copy.db" ] && echo "ok   - migration ran against the given path" || { echo "FAIL - copy.db absent"; fail=1; }
for artifact in denso.db denso.log denso.lock models; do
  [ -e "$TMP/$artifact" ] && { echo "FAIL - --check-migrations created $artifact"; fail=1; } \
                          || echo "ok   - --check-migrations created no $artifact"
done

rm -rf "$TMP"
exit $fail
```

- [ ] **Step 2: Run it to verify it fails**

```sh
chmod +x tests/manual/slice1_modes.sh && ./tests/manual/slice1_modes.sh
```
Expected: FAIL — the app ignores the flags today and tries to open a window.

- [ ] **Step 3: Write minimal implementation**

Create `src/app/cli/run_headless.h`:

```cpp
// Executes the headless CLI modes. Lives in `denso` rather than denso_core
// because --check constructs the platform inference backend (Task 6).
//
// Every mode here runs under a QCoreApplication — never QApplication — so no
// display is required.
#pragma once

#include "cli/args.h"

namespace denso::app {

/// Returns the process exit code. See the plan's exit-code table:
/// 0 ok (--check-running: running) / 1 failed (--check-running: not running)
/// / 2 bad usage.
int run_headless(const denso::cli::Command& cmd);

} // namespace denso::app
```

Create `src/app/cli/run_headless.cpp`:

```cpp
#include "cli/run_headless.h"

#include "db/db.h"
#include "instance/single_instance.h"
#include "paths/paths.h"

#include <QString>

#include <cstdio>

namespace denso::app {

namespace {

int run_version() {
    std::printf("%s\n", APP_VERSION);
    return 0;
}

int run_check_running() {
    // 0 = an instance is running, 1 = none. (prerm reads this to refuse an
    // upgrade under a live app.)
    const bool running =
        denso::instance::SingleInstance::is_running(denso::paths::lock_file());
    std::printf("%s\n", running ? "running" : "not running");
    return running ? 0 : 1;
}

int run_check_migrations(const QString& db_path) {
    // Deliberately the NORMAL open() + run_migrations(): the caller hands us a
    // throwaway copy, so mutation is confined there and --check's
    // no-persistent-mutation contract is untouched.
    auto db = denso::db::Db::open(db_path);
    if (!db) {
        std::fprintf(stderr, "check-migrations: cannot open %s\n",
                     qPrintable(db_path));
        return 1;
    }
    if (!denso::db::run_migrations(db->handle())) {
        std::fprintf(stderr, "check-migrations: migration chain FAILED on %s\n",
                     qPrintable(db_path));
        return 1;
    }
    std::printf("check-migrations: ok (%s)\n", qPrintable(db_path));
    return 0;
}

} // namespace

int run_headless(const denso::cli::Command& cmd) {
    using denso::cli::Mode;
    switch (cmd.mode) {
        case Mode::Version:         return run_version();
        case Mode::CheckRunning:    return run_check_running();
        case Mode::CheckMigrations: return run_check_migrations(cmd.arg);
        case Mode::Check:           return run_check();   // Task 6
        case Mode::Error:
            std::fprintf(stderr, "denso: %s\n\n%s", qPrintable(cmd.error),
                         qPrintable(denso::cli::usage()));
            return 2;
        case Mode::Gui:
            std::fprintf(stderr, "denso: internal error: Gui is not headless\n");
            return 2;
    }
    return 2;
}

} // namespace denso::app
```

**Until Task 6 lands**, `run_check()` does not exist. Add it as the **last function inside the existing anonymous namespace above** — i.e. directly after `run_check_migrations` and before the closing `} // namespace` — so it is declared before `run_headless` uses it. Do not open a second anonymous namespace:

```cpp
// Placeholder — Task 6 replaces this with real engine validation.
int run_check() {
    std::fprintf(stderr, "check: not implemented\n");
    return 1;
}
```

In `src/app/main.cpp`, add to the include block:

```cpp
#include "cli/args.h"
#include "cli/run_headless.h"
#include "instance/single_instance.h"
#include "paths/paths.h"
#include <QCoreApplication>
#include <QMessageBox>
```

Replace the opening of `main()` (currently `int main(int argc, char** argv) {` followed immediately by `QApplication app(argc, argv);` at `main.cpp:83-84`) with:

```cpp
int main(int argc, char** argv) {
    // ── Headless dispatch, BEFORE any QApplication exists. QApplication would
    // load the xcb platform plugin and fail with no display — and the installer
    // calls these modes from a root shell that has none.
    QStringList raw;
    raw.reserve(argc - 1);
    for (int i = 1; i < argc; ++i) raw << QString::fromLocal8Bit(argv[i]);
    const denso::cli::Command cmd = denso::cli::parse(raw);
    if (denso::cli::is_headless(cmd.mode)) {
        QCoreApplication app(argc, argv);  // no GUI; gives us applicationDirPath()
        return denso::app::run_headless(cmd);
    }

    QApplication app(argc, argv);

    // ── Single instance, BEFORE the DB, the log sink, or any camera. Two
    // processes would contend on one SQLite file and silently eat log data
    // (rename-based rotation does not follow another process's open fd).
    static denso::instance::SingleInstance instance_guard(denso::paths::lock_file());
    if (!instance_guard.acquire()) {
        // The log sink does not exist yet — stderr is all we have.
        std::fprintf(stderr, "denso: another instance is already running\n");
        QMessageBox::information(nullptr, QStringLiteral("Denso DigitalReader"),
                                 QStringLiteral("Denso DigitalReader is already running."));
        return 3;
    }
```

Then in the same file, retarget the three path call sites:

```cpp
// main.cpp:98 — was: QCoreApplication::applicationDirPath() + QStringLiteral("/denso.log")
static denso::logging::RotatingLogSink sink(denso::paths::log_file());

// main.cpp:118 (the SESSION marker) — was: << " dir=" << QCoreApplication::applicationDirPath();
    << " data=" << denso::paths::data_dir();

// main.cpp — was: const QString db_path = denso::db::default_path();
const QString db_path = denso::paths::db_file();

// main.cpp — was: QFileInfo(db_path).absolutePath() + QStringLiteral("/settings.json")
const QString legacy_json = denso::paths::legacy_settings_json();

// main.cpp:154 — was: QCoreApplication::applicationDirPath() + QStringLiteral("/models")
denso::ui::sync_models(conn, denso::paths::models_dir());
```

In `src/app/ui/startup.cpp`, replace lines 111-114:

```cpp
// was:
//   const std::string dir = QCoreApplication::applicationDirPath().toStdString();
//   const std::string models_dir = dir + "/models";
//   const std::string cache_dir = dir + "/models/trt_cache";
const std::string models_dir = denso::paths::models_dir().toStdString();
const std::string cache_dir = denso::paths::trt_cache_dir().toStdString();
```
and add `#include "paths/paths.h"` to its include block.

In `src/core/db/db.cpp`, make `default_path()` delegate so there is exactly one source of truth:

```cpp
// was: applicationDirPath() + "/denso.db", with a "denso.db" fallback
QString default_path() { return paths::db_file(); }
```
add `#include "paths/paths.h"`, and drop the now-unused `<QCoreApplication>` include **only if** nothing else in the file uses it. Update the `db.h` doc comment on `default_path()` to: `/// The database file inside the data dir (see denso::paths). Prefer paths::db_file() directly in new code.`

In `src/app/CMakeLists.txt`, add to the `add_executable(denso ...)` source list, right after `main.cpp`:

```cmake
    cli/run_headless.cpp
```

- [ ] **Step 4: Run it to verify it passes**

```sh
cmake --build build && ./tests/manual/slice1_modes.sh
```
Expected: every line `ok`.

Then confirm no regression and that the GUI still launches with **no** `DENSO_DATA_DIR` (the default must stay identical to today's behavior):
```sh
ctest --test-dir build 2>&1 | tail -3     # >= the baseline from the header
unset DENSO_DATA_DIR; ./build/src/app/denso   # window opens; denso.db/.log still beside the exe
```

- [ ] **Step 5: Commit**

```bash
git add src/app/cli/run_headless.h src/app/cli/run_headless.cpp src/app/main.cpp src/app/ui/startup.cpp src/app/CMakeLists.txt src/core/db/db.cpp src/core/db/db.h tests/manual/slice1_modes.sh
git commit -m "feat(app): dispatch headless modes before QApplication; route paths through denso::paths

main() parsed nothing and built QApplication first (main.cpp:84), so a headless
--check on a display-less root shell would have loaded xcb and died. Now: parse
-> QCoreApplication + run_headless -> return, else QApplication -> lock -> log
-> db. Every mutable path now comes from denso::paths, so \$DENSO_DATA_DIR moves
state off the (root-owned, upgrade-replaced) program dir. Default unchanged.

--check is still a stub (Task 6)."
```

---

### Task 6: `--check` — validate engines through the real backend

**Files:**
- Modify: `src/app/cli/run_headless.cpp` (replace the `run_check` stub)
- Test: `tests/manual/slice1_modes.sh` (extend), plus an on-device run

**Interfaces:**
- Consumes: `denso::paths::*` (Task 1); `denso::db::Db::open_read_only` **and** `denso::detection::try_attached_model_filenames` (Task 2); `denso::cli::Command::engines` (Task 4); `denso::ui::BackendEngine` (existing alias in `detection/engine_registry.h` — `OrtEngine` on Windows, `TrtEngine` on Linux). All three engines expose `class_names()` (`inference_engine.h:29`) and a `(path, cache_dir)` ctor.
- Produces: `int run_check(const QStringList& extra_engines)` — the final gate `denso-setup verify` calls in Slice 2.

**Three traps this task exists to avoid** (all verified in the source):
1. **`TrtEngine::ok()` is a hardcoded `return true`** (`trt_engine.h:42` — "ctor either succeeds or throws"), so on the Jetson the **try/catch** is the only real signal. `OrtEngine::ok()` (`ort_engine.h:26`) is a real check. Keep both.
2. **Do not parse the sidecar here.** `TrtEngine`'s ctor already reads `<stem>.names.json` itself (`trt_engine.cpp:89`) and throws without it, while Windows models are `.onnx` with names from ONNX metadata and **no sidecar at all** — so an unconditional `read_names_sidecar` would fail every valid Windows model. Validate `engine.class_names()` instead and let each backend enforce its own rule.
3. **`attached_model_filenames` cannot be used** — it returns `{}` on query failure (`repo.cpp:53`), so a corrupt DB would read as a fresh install. Use Task 2's `try_attached_model_filenames`.

**What it must NOT do** (spec, verified): call `EngineRegistry::warm_up()` (`engine_registry.cpp:42` creates the cache dir); open the DB via `Db::open()` (`db.cpp:76` rewrites journal mode); call `sync_models`; run migrations; take the lock; construct `QApplication`.

**Why a throwaway cache dir:** `TrtEngine` ignores `cache_dir` (`trt_engine.cpp:87`), but `OrtEngine` uses it on Windows. Passing a `QTemporaryDir` keeps the promise on **both** platforms — nothing is written into the real `models/trt_cache`.

- [ ] **Step 1: Write the failing test**

Append to `tests/manual/slice1_modes.sh`, before the `rm -rf "$TMP"` line:

```sh
# --check on an empty data dir: no DB and no engines is a VALID fresh install (a
# fresh DB references no cameras, so it requires no engines).
CHK=$(mktemp -d); export DENSO_DATA_DIR="$CHK"
run --check; chk "--check on a fresh data dir exits 0" 0 $?

# Persistent-mutation INVENTORY, not a list of four guesses: after --check the
# data dir must be byte-for-byte empty. Anything at all (denso.db, denso.log,
# denso.lock, models/, trt_cache, a -wal/-shm, a leftover probe file) is a
# contract violation, including artifacts we didn't think to name.
LEFT=$(ls -A "$CHK")
if [ -z "$LEFT" ]; then echo "ok   - --check left the data dir empty"
else echo "FAIL - --check created: $LEFT"; fail=1; fi

# A named package engine that isn't there must FAIL, even with no database --
# this is the fresh-install case where configured-only checking would pass a
# corrupt/absent packaged engine.
run --check --engine absent.engine; chk "--check --engine <missing> exits 1" 1 $?

# A present-but-corrupt database must FAIL, not read as "fresh".
BAD=$(mktemp -d); export DENSO_DATA_DIR="$BAD"
printf 'this is not a sqlite file' > "$BAD/denso.db"
run --check; chk "--check on a corrupt database exits 1" 1 $?
rm -rf "$BAD"

export DENSO_DATA_DIR="$TMP"
rm -rf "$CHK"

# NOTE: the unwritable-data-dir case is NOT tested here. `chmod 500` does not
# reliably make a directory unwritable to a Windows process under MSYS2, so the
# test would fail despite correct behavior. It is covered on the Jetson below.
```

- [ ] **Step 2: Run it to verify it fails**

```sh
./tests/manual/slice1_modes.sh
```
Expected: FAIL — `--check` is the Task 5 stub, so it exits 1 with "check: not implemented".

- [ ] **Step 3: Write minimal implementation**

In `src/app/cli/run_headless.cpp`, delete the stub and add these includes (note: **no** `class_names_sidecar.h` — see `validate_model`'s comment):

```cpp
#include "detection/engine_registry.h"   // denso::ui::BackendEngine
#include "detection/repo.h"

#include <QDir>
#include <QFileInfo>
#include <QStringList>
#include <QTemporaryDir>
#include <QTemporaryFile>

#include <algorithm>
#include <exception>
#include <optional>
#include <string>
#include <vector>
```

Then implement, inside the anonymous namespace:

```cpp
/// Real create-and-remove probe. access(W_OK) is weaker: it does not prove a
/// create succeeds under the actual mount, ACL, quota, or read-only conditions.
bool data_dir_writable() {
    const QString dir = denso::paths::data_dir();
    if (!QFileInfo(dir).isDir()) {
        std::fprintf(stderr, "check: data dir does not exist: %s\n", qPrintable(dir));
        return false;
    }
    QTemporaryFile probe(dir + QStringLiteral("/.denso-check-XXXXXX"));
    if (!probe.open()) {  // QTemporaryFile removes itself on close/destruction
        std::fprintf(stderr, "check: data dir is not writable: %s\n", qPrintable(dir));
        return false;
    }
    return true;
}

/// The engines configured cameras actually need. A MISSING database is an empty
/// set, never an error and never a reason to create one: a fresh DB references
/// no cameras (the query joins camera_model), so a clean install legitimately
/// needs no *configured* engines at all.
///
/// Returns nullopt for "present but UNREADABLE" — a hard failure that must never
/// be confused with the empty fresh-install case. This is why it uses
/// try_attached_model_filenames: the plain attached_model_filenames returns {}
/// for a failed query too (repo.cpp:53).
std::optional<std::vector<std::string>> configured_models() {
    const QString db_path = denso::paths::db_file();
    if (!QFileInfo::exists(db_path)) {
        std::printf("check: no database yet (fresh install) — no configured engines\n");
        return std::vector<std::string>{};
    }
    auto db = denso::db::Db::open_read_only(db_path);
    if (!db) {
        std::fprintf(stderr, "check: cannot read database: %s\n", qPrintable(db_path));
        return std::nullopt;
    }
    auto models = denso::detection::try_attached_model_filenames(db->handle());
    if (!models) {
        std::fprintf(stderr, "check: database is unreadable (bad schema?): %s\n",
                     qPrintable(db_path));
        return std::nullopt;
    }
    return models;
}

/// Load one model the way the APP does — construct the backend directly. That
/// reads the file and, on Linux, writes nothing (trt_engine.cpp:87 ignores
/// cache_dir). `trtexec --loadEngine` would only prove TensorRT can read the
/// plan, not that THIS app can load and bind it.
///
/// Class names are validated via engine.class_names(), NOT by parsing a sidecar
/// here: the backends source names differently and each already enforces its own
/// rule — TrtEngine reads <stem>.names.json in its ctor (trt_engine.cpp:89) and
/// throws without it; OrtEngine reads them from the ONNX metadata (there is no
/// sidecar for a .onnx). Parsing a sidecar unconditionally would fail every
/// valid Windows model.
bool validate_model(const std::string& filename, const QString& cache_dir) {
    const QString path =
        QDir(denso::paths::models_dir()).filePath(QString::fromStdString(filename));
    if (!QFileInfo::exists(path)) {
        std::fprintf(stderr, "check: FAIL %s — file missing from %s\n", filename.c_str(),
                     qPrintable(denso::paths::models_dir()));
        return false;
    }
    try {
        // Both signals are needed — the backends fail differently.
        // TrtEngine::ok() is a hardcoded `return true` (trt_engine.h:42 — "ctor
        // either succeeds or throws"), so on the Jetson the try/catch IS the
        // gate; OrtEngine::ok() is a real check (ort_engine.h:26).
        denso::ui::BackendEngine engine(path.toStdString(), cache_dir.toStdString());
        if (!engine.ok()) {
            std::fprintf(stderr, "check: FAIL %s — did not load\n", filename.c_str());
            return false;
        }
        if (engine.class_names().empty()) {
            std::fprintf(stderr, "check: FAIL %s — no class names (missing sidecar / "
                                 "ONNX metadata)\n", filename.c_str());
            return false;
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "check: FAIL %s — %s\n", filename.c_str(), e.what());
        return false;
    }
    std::printf("check: ok   %s\n", filename.c_str());
    return true;
}

int run_check(const QStringList& extra_engines) {
    if (!data_dir_writable()) return 1;

    const auto configured = configured_models();
    if (!configured) return 1;  // present but unreadable — NOT the same as empty

    // Validate the UNION of what the DB references and what the caller named.
    // Why the union: on a fresh install the DB is empty, so configured-only would
    // load ZERO engines and pass even with a corrupt packaged engine — yet the
    // spec makes package engines an activation blocker. Why not scan models/*:
    // an unrelated operator engine must never block an upgrade. Slice 2's
    // denso-setup passes --engine from its tracked package manifest.
    std::vector<std::string> targets = *configured;
    for (const QString& e : extra_engines) targets.push_back(e.toStdString());
    std::sort(targets.begin(), targets.end());
    targets.erase(std::unique(targets.begin(), targets.end()), targets.end());

    if (targets.empty()) {
        std::printf("check: PASS (fresh install; no engines required)\n");
        return 0;
    }

    // Throwaway cache: TrtEngine ignores cache_dir (trt_engine.cpp:87) but
    // OrtEngine uses it, so this keeps the real models/trt_cache untouched on
    // BOTH platforms.
    QTemporaryDir cache;
    if (!cache.isValid()) {
        std::fprintf(stderr, "check: cannot create a temporary cache dir\n");
        return 1;
    }

    bool ok = true;
    for (const std::string& m : targets) ok = validate_model(m, cache.path()) && ok;

    std::printf("check: %s (%zu model(s) validated)\n", ok ? "PASS" : "FAIL",
                targets.size());
    return ok ? 0 : 1;
}
```

and change the dispatch line in `run_headless` (added in Task 5) from `case Mode::Check: return run_check();` to:

```cpp
        case Mode::Check:           return run_check(cmd.engines);
```

- [ ] **Step 4: Run it to verify it passes**

```sh
cmake --build build && ./tests/manual/slice1_modes.sh
```
Expected: every line `ok`.

Then the real gate — **on the Jetson** (192.168.1.15), where a genuine `sm_87` TensorRT engine exists. The Windows box has no `.engine`, so it can never prove this path:

```sh
# on the Jetson, after building there
export DENSO_DATA_DIR=$HOME/project/Denso-DigitalReader/build/src/app

# 1. the packaged engine validates by name, with no DB involvement
./build/src/app/denso --check --engine digitv2.engine
#    expect: "check: ok   digitv2.engine" then "check: PASS (1 model(s) validated)"

# 2. no mutation: trt_cache must be untouched (record before/after)
ls -la $DENSO_DATA_DIR/models/trt_cache

# 3. the unwritable-data-dir case, which MSYS2 cannot test honestly
sudo install -d -o root -g root -m 555 /tmp/denso-ro
DENSO_DATA_DIR=/tmp/denso-ro ./build/src/app/denso --check; echo "rc=$?"   # expect rc=1
sudo rmdir /tmp/denso-ro

# 4. a real corrupt-engine failure (the whole point of the gate)
cp $DENSO_DATA_DIR/models/digitv2.engine /tmp/good.engine
printf 'corrupt' >> $DENSO_DATA_DIR/models/digitv2.engine
./build/src/app/denso --check --engine digitv2.engine; echo "rc=$?"   # expect rc=1
cp /tmp/good.engine $DENSO_DATA_DIR/models/digitv2.engine             # RESTORE IT
```

- [ ] **Step 5: Commit**

```bash
git add src/app/cli/run_headless.cpp tests/manual/slice1_modes.sh
git commit -m "feat(app): implement --check — validate models via the real backend

Constructs BackendEngine directly rather than calling EngineRegistry::warm_up
(engine_registry.cpp:42 creates trt_cache) and reads the DB via open_read_only
(db.cpp:76's WAL pragma is a mutation). Validates the UNION of DB-referenced
models and --engine names: on a fresh install the DB is empty, so a
configured-only check would load ZERO engines and pass even with a corrupt
packaged engine. Class names come from engine.class_names(), not a sidecar parse
-- Windows .onnx models have no sidecar, and TrtEngine already reads its own.
The cache dir is a QTemporaryDir so OrtEngine can't write the real trt_cache on
Windows either."
```

---

## Slice 1 exit criteria

- [ ] `ctest --test-dir build` ≥ the baseline recorded in the header, with the ~27 new cases passing.
- [ ] `./tests/manual/slice1_modes.sh` all `ok` on the Windows box.
- [ ] With `DENSO_DATA_DIR` **unset**, the GUI behaves exactly as before (db/log/models beside the exe) — this is what keeps Windows dev and the test suite unchanged.
- [ ] With `DENSO_DATA_DIR` set, the GUI puts db/log/models there and nothing in the program dir.
- [ ] **Inter-process** guard (the contract the Catch2 cases can't prove): launching a second `denso` while one runs shows "already running" and exits 3.
- [ ] `denso --check-running` exits 0 while the GUI runs, 1 when it does not.
- [ ] On the Jetson, all four on-device checks from Task 6 Step 4: the real `digitv2.engine` validates by `--engine` name; `trt_cache` is unchanged; an unwritable data dir exits 1; and a **deliberately corrupted engine exits 1** (restore it afterwards).

## What Slice 1 deliberately does NOT do

Packaging (`.deb`, `control`, maintainer scripts, `denso-setup`, `build_package.sh`), autostart/autologin, model seeding, and the apt-plan guard — all Slice 2, all Jetson-verifiable only. Also out: raise-existing-window IPC (`QLocalServer`), a `--check-gui` mode, and any change to the migration chain (still v11).
