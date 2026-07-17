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

QString db_file()              { return data_dir() + QStringLiteral("/denso.db"); }
QString log_file()             { return data_dir() + QStringLiteral("/denso.log"); }
QString models_dir()           { return data_dir() + QStringLiteral("/models"); }
QString trt_cache_dir()        { return models_dir() + QStringLiteral("/trt_cache"); }
QString lock_file()            { return data_dir() + QStringLiteral("/denso.lock"); }
QString legacy_settings_json() { return data_dir() + QStringLiteral("/settings.json"); }

} // namespace denso::paths
```

In `src/core/CMakeLists.txt`, add to the `add_library(denso_core STATIC ...)` source list, immediately before `db/db.cpp`:

```cmake
    paths/paths.cpp
```

- [ ] **Step 4: Run tests to verify they pass**

```sh
cmake --build build && ctest --test-dir build -R "paths" -V
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

### Task 2: `Db::open_read_only` — read configured models without touching the DB

**Files:**
- Modify: `src/core/db/db.h` (add `open_read_only`; retarget `default_path` docs), `src/core/db/db.cpp`
- Test: `tests/test_db.cpp` (append)

**Interfaces:**
- Consumes: `denso::paths::db_file()` (Task 1).
- Produces: `static std::optional<Db> Db::open_read_only(const QString& path);` — Task 6 uses it. Returns `nullopt` when the file is absent or unreadable, **without creating it**.

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

TEST_CASE("open_read_only does not rewrite the journal mode", "[db][readonly]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("journal.db"));

    QString before;
    {
        auto rw = Db::open(path);
        REQUIRE(rw.has_value());
        QSqlQuery q(rw->handle());
        REQUIRE(q.exec(QStringLiteral("PRAGMA journal_mode")));
        REQUIRE(q.next());
        before = q.value(0).toString();
    }

    {
        auto ro = Db::open_read_only(path);
        REQUIRE(ro.has_value());
    }

    auto after_db = Db::open(path);
    REQUIRE(after_db.has_value());
    QSqlQuery q(after_db->handle());
    REQUIRE(q.exec(QStringLiteral("PRAGMA journal_mode")));
    REQUIRE(q.next());
    REQUIRE(q.value(0).toString() == before);
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
    /// Caveat: reading a WAL-mode database may still create a transient `-shm`
    /// sidecar. That is why any root-side caller must drop to the target user
    /// first — a root-owned `-shm` in an operator-owned data dir breaks the app.
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
    }
    if (!ok) {
        QSqlDatabase::removeDatabase(name);
        return std::nullopt;
    }
    return Db(name);
}
```

- [ ] **Step 4: Run tests to verify they pass**

```sh
cmake --build build && ctest --test-dir build -R "db" -V
```
Expected: PASS, including the three new cases.

If "refuses writes" fails: confirm the connect option string is exactly `QSQLITE_OPEN_READONLY` and that it is set **before** `db.open()`.

- [ ] **Step 5: Commit**

```bash
git add src/core/db/db.h src/core/db/db.cpp tests/test_db.cpp
git commit -m "feat(core): add Db::open_read_only for non-mutating inspection

open() runs PRAGMA journal_mode=WAL, which rewrites the file header -- so it
cannot serve --check. open_read_only skips the pragma and never creates an
absent file (a missing DB is an empty configured-model set)."
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
cmake --build build && ctest --test-dir build -R "instance" -V
```
Expected: PASS — 5 test cases.

**If "a second acquire on the same lock fails" FAILS:** `QLockFile` is documented for *inter*-process use; verify what it does when the holder is the *same* pid. If Qt treats our own live pid as a valid owner the test passes as written. If it instead reclaims the lock, do **not** weaken the test — the guard is then not sound in-process, and you must add a process-local flag (a `static std::atomic_flag` keyed on the canonical path) checked before `tryLock`. Report this before changing the test.

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
- Produces: `namespace denso::cli` — `enum class Mode { Gui, Version, Check, CheckRunning, CheckMigrations, Error }`, `struct Command { Mode mode = Mode::Gui; QString arg; QString error; }`, `Command parse(const QStringList& args)` (args **exclude** `argv[0]`), `bool is_headless(Mode m)`, `QString usage()`. Tasks 5 and 6 use these exact names.

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

TEST_CASE("--check-migrations carries its db path", "[cli]") {
    const Command c = parse({QStringLiteral("--check-migrations"),
                             QStringLiteral("/tmp/copy.db")});
    REQUIRE(c.mode == Mode::CheckMigrations);
    REQUIRE(c.arg == QStringLiteral("/tmp/copy.db"));
}

TEST_CASE("--check-migrations without a path is an error, not a GUI launch", "[cli]") {
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
```

Register `test_cli_args.cpp` in `tests/CMakeLists.txt`.

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
    QString arg;    ///< CheckMigrations: the db path to migrate
    QString error;  ///< Error: the human-readable reason
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
        "usage: denso [--version | --check | --check-running |\n"
        "              --check-migrations <db-path>]\n"
        "\n"
        "  (no flags)                 run the application\n"
        "  --version                  print the version and exit\n"
        "  --check                    validate runtime + engines; no persistent mutation\n"
        "  --check-running            exit 0 if an instance holds the lock, 1 if not\n"
        "  --check-migrations <db>    run the migration chain against <db> ONLY\n");
}

bool is_headless(Mode m) { return m != Mode::Gui; }

Command parse(const QStringList& args) {
    if (args.isEmpty()) return Command{Mode::Gui, {}, {}};

    const QString& flag = args.first();

    if (flag == QStringLiteral("--check-migrations")) {
        if (args.size() == 2) return Command{Mode::CheckMigrations, args.at(1), {}};
        return Command{Mode::Error, {},
                       QStringLiteral("--check-migrations requires exactly one "
                                      "database path")};
    }

    if (args.size() == 1) {
        if (flag == QStringLiteral("--version"))       return Command{Mode::Version, {}, {}};
        if (flag == QStringLiteral("--check"))         return Command{Mode::Check, {}, {}};
        if (flag == QStringLiteral("--check-running")) return Command{Mode::CheckRunning, {}, {}};
    }

    return Command{Mode::Error, {},
                   QStringLiteral("unknown or malformed option: %1").arg(flag)};
}

} // namespace denso::cli
```

In `src/core/CMakeLists.txt`, add to the `denso_core` source list after `instance/single_instance.cpp`:

```cmake
    cli/args.cpp
```

- [ ] **Step 4: Run tests to verify they pass**

```sh
cmake --build build && ctest --test-dir build -R "cli" -V
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

TMP=$(mktemp -d)
export DENSO_DATA_DIR="$TMP"

"$EXE" --version >/dev/null 2>&1;            chk "--version exits 0" 0 $?
"$EXE" --wat >/dev/null 2>&1;                chk "unknown flag exits 2" 2 $?
"$EXE" --check-migrations >/dev/null 2>&1;   chk "--check-migrations without path exits 2" 2 $?
"$EXE" --check-running >/dev/null 2>&1;      chk "--check-running with nothing running exits 1" 1 $?

# --version must not create ANY state in the data dir.
if [ -z "$(ls -A "$TMP")" ]; then echo "ok   - --version left the data dir empty"; else echo "FAIL - --version created: $(ls -A "$TMP")"; fail=1; fi

# --check-migrations builds the chain in a throwaway db and touches nothing else.
"$EXE" --check-migrations "$TMP/copy.db" >/dev/null 2>&1; chk "--check-migrations exits 0" 0 $?
[ -f "$TMP/copy.db" ] && echo "ok   - migration ran against the given path" || { echo "FAIL - copy.db absent"; fail=1; }
[ -f "$TMP/denso.db" ] && { echo "FAIL - --check-migrations touched the live denso.db"; fail=1; } || echo "ok   - live denso.db untouched"

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

**Until Task 6 lands**, add this stub above `run_headless` so the file compiles:

```cpp
namespace { int run_check() { std::fprintf(stderr, "check: not implemented\n"); return 1; } }
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
- Consumes: `denso::paths::*` (Task 1); `denso::db::Db::open_read_only` (Task 2); `denso::detection::attached_model_filenames(const QSqlDatabase&)` (existing, `src/core/detection/repo.cpp:48`); **`denso::ui::read_names_sidecar(const std::filesystem::path&)`** (existing, `detection/class_names_sidecar.h`, namespace `denso::ui`, in the `denso_detection` lib); `denso::ui::BackendEngine` (existing alias in `detection/engine_registry.h` — `OrtEngine` on Windows, `TrtEngine` on Linux; both expose `ok()` and a `(path, cache_dir)` ctor, **but `TrtEngine::ok()` is a hardcoded `return true`** — on Linux the ctor throwing is the only real signal).
- Produces: `int run_check()` — the final gate `denso-setup verify` calls in Slice 2.

**What it must NOT do** (spec, verified): call `EngineRegistry::warm_up()` (`engine_registry.cpp:42` creates the cache dir); open the DB via `Db::open()` (`db.cpp:76` rewrites journal mode); call `sync_models`; run migrations; take the lock; construct `QApplication`.

**Why a throwaway cache dir:** `TrtEngine` ignores `cache_dir` (`trt_engine.cpp:87`), but `OrtEngine` uses it on Windows. Passing a `QTemporaryDir` keeps the promise on **both** platforms — nothing is written into the real `models/trt_cache`.

- [ ] **Step 1: Write the failing test**

Append to `tests/manual/slice1_modes.sh`, before the `rm -rf "$TMP"` line:

```sh
# --check on an empty data dir: no DB and no engines is a VALID fresh install
# (a fresh DB references no cameras, so it requires no engines), and it must not
# conjure a database or a trt_cache into existence.
"$EXE" --check >/dev/null 2>&1; chk "--check on a fresh data dir exits 0" 0 $?
[ -f "$TMP/denso.db" ] && { echo "FAIL - --check created denso.db"; fail=1; } || echo "ok   - --check created no denso.db"
[ -d "$TMP/models/trt_cache" ] && { echo "FAIL - --check created trt_cache"; fail=1; } || echo "ok   - --check created no trt_cache"
[ -f "$TMP/denso.log" ] && { echo "FAIL - --check initialized the log sink"; fail=1; } || echo "ok   - --check wrote no log"
[ -f "$TMP/denso.lock" ] && { echo "FAIL - --check took the lock"; fail=1; } || echo "ok   - --check took no lock"

# A data dir the app cannot write is a hard failure, not a warning.
RO=$(mktemp -d); chmod 500 "$RO"
DENSO_DATA_DIR="$RO" "$EXE" --check >/dev/null 2>&1; chk "--check fails on an unwritable data dir" 1 $?
chmod 700 "$RO"; rm -rf "$RO"
```

- [ ] **Step 2: Run it to verify it fails**

```sh
./tests/manual/slice1_modes.sh
```
Expected: FAIL — `--check` is the Task 5 stub, so it exits 1 with "check: not implemented".

- [ ] **Step 3: Write minimal implementation**

In `src/app/cli/run_headless.cpp`, delete the stub and add these includes:

```cpp
#include "detection/class_names_sidecar.h"
#include "detection/engine_registry.h"
#include "detection/repo.h"

#include <QDir>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTemporaryFile>

#include <filesystem>
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
/// no cameras (detection::attached_model_filenames joins camera_model), so a
/// clean install legitimately needs no engines at all.
///
/// Returns nullopt for "present but UNREADABLE", which is a hard failure and
/// must never be confused with the empty (fresh-install) case.
std::optional<std::vector<std::string>> configured_models() {
    const QString db_path = denso::paths::db_file();
    if (!QFileInfo::exists(db_path)) {
        std::printf("check: no database yet (fresh install) — no engines required\n");
        return std::vector<std::string>{};
    }
    auto db = denso::db::Db::open_read_only(db_path);
    if (!db) {
        std::fprintf(stderr, "check: cannot read database: %s\n", qPrintable(db_path));
        return std::nullopt;
    }
    return denso::detection::attached_model_filenames(db->handle());
}

/// Load one engine the way the APP does — construct the backend directly, which
/// reads the engine + sidecar and (on Linux) writes nothing. trtexec
/// --loadEngine would only prove TensorRT can read the plan, not that this app
/// can load and bind it.
bool validate_engine(const std::string& filename, const QString& cache_dir) {
    const QString path = denso::paths::models_dir() + QStringLiteral("/") +
                         QString::fromStdString(filename);
    if (!QFileInfo::exists(path)) {
        std::fprintf(stderr, "check: FAIL %s — engine missing\n", filename.c_str());
        return false;
    }
    try {
        const auto names = denso::ui::read_names_sidecar(
            std::filesystem::path(path.toStdString()));
        if (!names || names->empty()) {
            std::fprintf(stderr, "check: FAIL %s — missing/empty .names.json sidecar\n",
                         filename.c_str());
            return false;
        }
        // BOTH signals are needed, because the two backends fail differently:
        // TrtEngine::ok() is a hardcoded `return true` (trt_engine.h:42 — "ctor
        // either succeeds or throws"), so on the Jetson the try/catch IS the
        // gate; OrtEngine::ok() is a real check (ort_engine.h:26). Keep both.
        denso::ui::BackendEngine engine(path.toStdString(), cache_dir.toStdString());
        if (!engine.ok()) {
            std::fprintf(stderr, "check: FAIL %s — engine did not load\n", filename.c_str());
            return false;
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "check: FAIL %s — %s\n", filename.c_str(), e.what());
        return false;
    }
    std::printf("check: ok   %s\n", filename.c_str());
    return true;
}

int run_check() {
    if (!data_dir_writable()) return 1;

    const auto maybe_required = configured_models();
    if (!maybe_required) return 1;  // present but unreadable — NOT the same as empty
    const std::vector<std::string>& required = *maybe_required;

    // Throwaway cache: TrtEngine ignores cache_dir (trt_engine.cpp:87) but
    // OrtEngine uses it, so this keeps "no persistent mutation" true on both.
    QTemporaryDir cache;
    if (!cache.isValid()) {
        std::fprintf(stderr, "check: cannot create a temporary cache dir\n");
        return 1;
    }

    bool ok = true;
    for (const std::string& m : required) ok = validate_engine(m, cache.path()) && ok;

    std::printf("check: %s (%zu engine(s) required)\n", ok ? "PASS" : "FAIL",
                required.size());
    return ok ? 0 : 1;
}
```

- [ ] **Step 4: Run it to verify it passes**

```sh
cmake --build build && ./tests/manual/slice1_modes.sh
```
Expected: every line `ok`.

Then the real gate — **on the Jetson**, where a genuine TensorRT engine exists (`--check` cannot validate an engine on the Windows box; there is no `.engine` for `sm_87` there):
```sh
# on 192.168.1.15, after building there
export DENSO_DATA_DIR=$HOME/project/Denso-DigitalReader/build/src/app
./build/src/app/denso --check      # expect: "check: ok digitv2.engine" then "check: PASS"
ls $DENSO_DATA_DIR/models/trt_cache 2>/dev/null   # must be unchanged/absent — no mutation
```

- [ ] **Step 5: Commit**

```bash
git add src/app/cli/run_headless.cpp tests/manual/slice1_modes.sh
git commit -m "feat(app): implement --check — validate engines via the real backend

Constructs BackendEngine directly rather than calling EngineRegistry::warm_up
(engine_registry.cpp:42 creates trt_cache) and reads the DB via open_read_only
(db.cpp:76's WAL pragma is a mutation). A missing DB is an empty configured-model
set, never created. Writability is a real create-and-remove probe. The cache dir
is a QTemporaryDir so OrtEngine can't write the real trt_cache on Windows either."
```

---

## Slice 1 exit criteria

- [ ] `ctest --test-dir build` ≥ the baseline recorded in the header, with the ~18 new cases passing.
- [ ] `./tests/manual/slice1_modes.sh` all `ok` on the Windows box.
- [ ] With `DENSO_DATA_DIR` **unset**, the GUI behaves exactly as before (db/log/models beside the exe) — this is what keeps Windows dev and the test suite unchanged.
- [ ] With `DENSO_DATA_DIR` set, the GUI puts db/log/models there and nothing in the program dir.
- [ ] Launching a second `denso` while one runs shows "already running" and exits 3.
- [ ] On the Jetson: `denso --check` passes against the real `digitv2.engine` and creates no `trt_cache`.
- [ ] `denso --check-running` exits 0 while the GUI runs, 1 when it does not.

## What Slice 1 deliberately does NOT do

Packaging (`.deb`, `control`, maintainer scripts, `denso-setup`, `build_package.sh`), autostart/autologin, model seeding, and the apt-plan guard — all Slice 2, all Jetson-verifiable only. Also out: raise-existing-window IPC (`QLocalServer`), a `--check-gui` mode, and any change to the migration chain (still v11).
