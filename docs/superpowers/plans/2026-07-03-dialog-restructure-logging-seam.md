# Dialog Restructure + Logging/Export Seam Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Split the two heaviest dialogs into thin views + controllers backed by a shared `ui/common` module, and add a dormant reading-capture seam (core `reading` module + migration v9 + `DetectionProcessor` sink) ahead of a future data logging/export feature — with zero user-visible behavior change.

**Architecture:** Extract copy-pasted dialog chrome/async/label helpers into a new `src/app/ui/common/` leaf. Move `SettingsDialog`'s network orchestration into a self-contained `NetworkPanel` widget, and `CameraDialog`'s wizard flow-state + snapshot + DB writes into a `CameraWizardController` QObject that drives the dialog via injected callbacks. Add an isolated, unit-tested `core/reading` persistence module and a null-wired `ReadingSink` hook in `DetectionProcessor`.

**Tech Stack:** C++17, Qt6 (Core/Sql/Widgets/Multimedia/Network), OpenCV, ONNX Runtime, SQLite (via QSQLITE), Catch2 v3, CMake + Ninja, MSYS2 UCRT64 toolchain.

## Global Constraints

- **Toolchain:** MSYS2 UCRT64. Build cycle: `export PATH=/c/msys64/ucrt64/bin:$PATH; cmake -S . -B build -G Ninja; cmake --build build; ctest --test-dir build`.
- **No user-visible behavior change.** Every refactor task is behavior-preserving; relocate code verbatim, do not rewrite logic.
- **`denso_core` never links `Qt6::Widgets`/OpenCV/ORT** — only `Qt6::Core`/`Sql`. The `reading` module goes in `denso_core` and stays Qt-Widgets-free.
- **`ui/common/` is a leaf:** its headers must not include any feature header (`camera/`, `network/`, `detection/`, page widgets).
- **Migrations are append-only:** add a new `if (version < N)` block; never edit a shipped migration. Bump `SCHEMA_VERSION` in the same commit.
- **Naming:** snake_case functions/files, `denso::<area>` namespaces, headers co-located with sources, `QStringLiteral` for literals, 4-space indent.
- **Frequent commits:** one commit per task (or per the sub-steps shown).
- **Docs:** `CLAUDE.md` + `docs/ARCHITECTURE.md` updated in the final task so they land with the code.

---

## File Structure

**New files**
- `src/core/reading/reading.h` — `Reading` domain struct (Qt/OpenCV-free).
- `src/core/reading/repo.{h,cpp}` — `insert` / `query` persistence.
- `tests/test_reading_repo.cpp` — Catch2 round-trip + filter tests.
- `src/app/ui/common/form_widgets.{h,cpp}` — `eyebrow`/`dim_label`/`spec_row`/`hline`.
- `src/app/ui/common/dialog_chrome.{h,cpp}` — `dialog_header`.
- `src/app/ui/common/async_runner.{h,cpp}` — `run_on_worker`/`post_to_gui`.
- `src/app/ui/settings/network_panel.{h,cpp}` — self-contained Network page.
- `src/app/ui/camera/wizard_controller.{h,cpp}` — camera wizard flow/persistence.
- `.clang-format` — repo-root style config.

**Modified files**
- `src/core/db/db.cpp` — migration v9 + `SCHEMA_VERSION` bump.
- `src/core/CMakeLists.txt` — add `reading/repo.cpp`.
- `tests/CMakeLists.txt` — add `test_reading_repo.cpp`.
- `src/app/CMakeLists.txt` — add the 5 new app `.cpp` files.
- `src/app/ui/settings/settings_dialog.{h,cpp}` — shrink to view.
- `src/app/ui/camera/camera_dialog.{h,cpp}` — shrink to view.
- `src/app/ui/camera/dialog/page_util.{h,cpp}` — re-export common `dim_label`.
- `src/app/ui/camera/grid/frame_processor.{h,cpp}` — dormant `ReadingSink` hook.
- `CLAUDE.md`, `docs/ARCHITECTURE.md` — document new modules + seam.

---

## Task 1: Core `reading` module — domain, migration v9, repo, tests

**Files:**
- Create: `src/core/reading/reading.h`
- Create: `src/core/reading/repo.h`
- Create: `src/core/reading/repo.cpp`
- Create: `tests/test_reading_repo.cpp`
- Modify: `src/core/db/db.cpp` (SCHEMA_VERSION 8→9; new `if (version < 9)` block before the `PRAGMA user_version = SCHEMA_VERSION` set)
- Modify: `src/core/CMakeLists.txt` (add `reading/repo.cpp`)
- Modify: `tests/CMakeLists.txt` (add `test_reading_repo.cpp`)

**Interfaces:**
- Consumes: `denso::db::Db`, `denso::db::run_migrations` (existing).
- Produces:
  - `struct denso::reading::Reading { int64_t id, camera_id, ts_ms; std::string value; float conf; }`
  - `std::optional<int64_t> denso::reading::insert(const QSqlDatabase&, const Reading&)`
  - `std::vector<Reading> denso::reading::query(const QSqlDatabase&, int64_t camera_id, int64_t from_ms, int64_t to_ms)`

- [ ] **Step 1: Create the domain header**

Create `src/core/reading/reading.h`:

```cpp
// A single recorded detection reading — one row in the `reading` table. Written
// by the (future) reading sink on the app side, read back by the logging/export
// UI. Qt/OpenCV-free, like the other core domain structs.
#pragma once

#include <cstdint>
#include <string>

namespace denso::reading {

/// One reading captured from a camera at a point in time. `value` is the
/// assembled reading text (assembly logic lives in the app-side sink); `conf`
/// is its aggregate confidence in [0,1]. Row in `reading`.
struct Reading {
    int64_t     id        = 0;
    int64_t     camera_id = 0;  // FK → camera.id
    int64_t     ts_ms     = 0;  // capture time, epoch milliseconds
    std::string value;          // the reading text
    float       conf      = 0.0f;
};

} // namespace denso::reading
```

- [ ] **Step 2: Create the repo header**

Create `src/core/reading/repo.h`:

```cpp
// Persistence for Reading in the SQLite `reading` table (one row per captured
// reading). Append + range-read only — readings are an immutable log. Mirrors
// the camera/detection repos: write/read failures surface as nullopt/empty so
// callers can react.
#pragma once

#include "reading/reading.h"

#include <QSqlDatabase>

#include <cstdint>
#include <optional>
#include <vector>

namespace denso::reading {

/// Insert a reading; returns its assigned id, or nullopt on a write error.
std::optional<int64_t> insert(const QSqlDatabase& db, const Reading& r);

/// Every reading for a camera with `from_ms <= ts_ms <= to_ms`, ordered by
/// ts_ms ascending then id. Empty when none match (or on error).
std::vector<Reading> query(const QSqlDatabase& db, int64_t camera_id,
                           int64_t from_ms, int64_t to_ms);

} // namespace denso::reading
```

- [ ] **Step 3: Create the repo implementation**

Create `src/core/reading/repo.cpp`:

```cpp
#include "reading/repo.h"

#include <QSqlQuery>
#include <QString>
#include <QVariant>

namespace denso::reading {

std::optional<int64_t> insert(const QSqlDatabase& db, const Reading& r) {
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "INSERT INTO reading (camera_id, ts_ms, value, conf) "
        "VALUES (?, ?, ?, ?)"));
    q.addBindValue(static_cast<qint64>(r.camera_id));
    q.addBindValue(static_cast<qint64>(r.ts_ms));
    q.addBindValue(QString::fromStdString(r.value));
    q.addBindValue(static_cast<double>(r.conf));
    if (!q.exec()) {
        return std::nullopt;
    }
    return q.lastInsertId().toLongLong();
}

std::vector<Reading> query(const QSqlDatabase& db, int64_t camera_id,
                           int64_t from_ms, int64_t to_ms) {
    std::vector<Reading> out;
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "SELECT id, camera_id, ts_ms, value, conf FROM reading "
        "WHERE camera_id = ? AND ts_ms >= ? AND ts_ms <= ? "
        "ORDER BY ts_ms ASC, id ASC"));
    q.addBindValue(static_cast<qint64>(camera_id));
    q.addBindValue(static_cast<qint64>(from_ms));
    q.addBindValue(static_cast<qint64>(to_ms));
    if (!q.exec()) {
        return out;
    }
    while (q.next()) {
        Reading r;
        r.id        = q.value(0).toLongLong();
        r.camera_id = q.value(1).toLongLong();
        r.ts_ms     = q.value(2).toLongLong();
        r.value     = q.value(3).toString().toStdString();
        r.conf      = static_cast<float>(q.value(4).toDouble());
        out.push_back(std::move(r));
    }
    return out;
}

} // namespace denso::reading
```

- [ ] **Step 4: Add migration v9**

In `src/core/db/db.cpp`, change line 18 from `constexpr int SCHEMA_VERSION = 8;` to `constexpr int SCHEMA_VERSION = 9;`.

Then insert this block immediately **after** the `if (version < 8) { … }` block closes (currently line 314) and **before** the `// PRAGMA can't be parameterized;` comment (currently line 316):

```cpp
    if (version < 9) {
        // Recorded readings — an append-only log of detections captured per
        // camera over time, consumed by the logging/export feature. Indexed on
        // (camera_id, ts_ms) for the range queries the export UI runs.
        if (!run("CREATE TABLE IF NOT EXISTS reading ("
                 "    id        INTEGER PRIMARY KEY,"
                 "    camera_id INTEGER NOT NULL REFERENCES camera(id),"
                 "    ts_ms     INTEGER NOT NULL,"
                 "    value     TEXT    NOT NULL,"
                 "    conf      REAL    NOT NULL"
                 ")")) {
            return false;
        }
        if (!run("CREATE INDEX IF NOT EXISTS idx_reading_camera_ts "
                 "ON reading(camera_id, ts_ms)")) {
            return false;
        }
    }
```

- [ ] **Step 5: Register the repo source**

In `src/core/CMakeLists.txt`, add `reading/repo.cpp` to the `add_library(denso_core STATIC …)` list, immediately after the `detection/repo.cpp` line:

```cmake
    detection/class_names.cpp
    detection/repo.cpp
    reading/repo.cpp
    ui/convert.cpp
```

- [ ] **Step 6: Write the failing test**

Create `tests/test_reading_repo.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>

#include "db/db.h"
#include "reading/reading.h"
#include "reading/repo.h"

#include <optional>
#include <utility>

using denso::db::Db;
using denso::db::run_migrations;
using denso::reading::insert;
using denso::reading::query;
using denso::reading::Reading;

namespace {

Db db() {
    auto d = Db::open_in_memory();
    REQUIRE(d.has_value());
    REQUIRE(run_migrations(d->handle()));
    return std::move(*d);
}

Reading make(int64_t cam, int64_t ts, const char* value, float conf) {
    Reading r;
    r.camera_id = cam;
    r.ts_ms = ts;
    r.value = value;
    r.conf = conf;
    return r;
}

} // namespace

TEST_CASE("reading insert returns an id and round-trips via query") {
    Db d = db();
    const auto id = insert(d.handle(), make(1, 1000, "0042", 0.9f));
    REQUIRE(id.has_value());
    REQUIRE(*id > 0);

    const std::vector<Reading> rows = query(d.handle(), 1, 0, 2000);
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].id == *id);
    CHECK(rows[0].camera_id == 1);
    CHECK(rows[0].ts_ms == 1000);
    CHECK(rows[0].value == "0042");
    CHECK(rows[0].conf == 0.9f);
}

TEST_CASE("reading query filters by camera and time range, ordered by ts_ms") {
    Db d = db();
    REQUIRE(insert(d.handle(), make(1, 3000, "c", 0.5f)).has_value());
    REQUIRE(insert(d.handle(), make(1, 1000, "a", 0.5f)).has_value());
    REQUIRE(insert(d.handle(), make(1, 2000, "b", 0.5f)).has_value());
    REQUIRE(insert(d.handle(), make(2, 1500, "other", 0.5f)).has_value());

    SECTION("only the requested camera, ascending ts_ms") {
        const std::vector<Reading> rows = query(d.handle(), 1, 0, 10000);
        REQUIRE(rows.size() == 3);
        CHECK(rows[0].value == "a");
        CHECK(rows[1].value == "b");
        CHECK(rows[2].value == "c");
    }

    SECTION("time range is inclusive and excludes out-of-range rows") {
        const std::vector<Reading> rows = query(d.handle(), 1, 1000, 2000);
        REQUIRE(rows.size() == 2);
        CHECK(rows[0].value == "a");
        CHECK(rows[1].value == "b");
    }

    SECTION("no matches yields an empty vector") {
        CHECK(query(d.handle(), 1, 5000, 6000).empty());
        CHECK(query(d.handle(), 99, 0, 10000).empty());
    }
}
```

In `tests/CMakeLists.txt`, add `test_reading_repo.cpp` to the `add_executable(denso_tests …)` list, immediately after `test_detection_repo.cpp`:

```cmake
    test_camera_repo.cpp
    test_detection_repo.cpp
    test_reading_repo.cpp
    test_area_points.cpp
```

- [ ] **Step 7: Configure, build, and run — verify tests pass**

Run:

```bash
export PATH=/c/msys64/ucrt64/bin:$PATH
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

Expected: build succeeds; `ctest` reports all tests passing, including the two new `reading …` cases (green baseline is 136 tests → 138 after this task). If the reading tests are the first run before the repo compiled, they would fail to link — a clean `cmake --build build` compiles `reading/repo.cpp` into `denso_core` first.

- [ ] **Step 8: Commit**

```bash
git add src/core/reading tests/test_reading_repo.cpp src/core/db/db.cpp \
        src/core/CMakeLists.txt tests/CMakeLists.txt
git commit -m "feat(reading): core reading log module + migration v9

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Task 2: `ui/common/` shared dialog primitives

**Files:**
- Create: `src/app/ui/common/form_widgets.h`
- Create: `src/app/ui/common/form_widgets.cpp`
- Create: `src/app/ui/common/dialog_chrome.h`
- Create: `src/app/ui/common/dialog_chrome.cpp`
- Create: `src/app/ui/common/async_runner.h`
- Create: `src/app/ui/common/async_runner.cpp`
- Modify: `src/app/CMakeLists.txt` (register the three new `.cpp`)

**Interfaces:**
- Produces:
  - `QLabel* denso::ui::common::eyebrow(const QString&)`
  - `QLabel* denso::ui::common::dim_label(const QString&)`
  - `QWidget* denso::ui::common::spec_row(const QString& label, QLabel** value_out)`
  - `QFrame* denso::ui::common::hline()`
  - `QVBoxLayout* denso::ui::common::dialog_header(QDialog* dlg, const QString& title)`
  - `void denso::ui::common::run_on_worker(std::function<void()> work)`
  - `void denso::ui::common::post_to_gui(QObject* ctx, std::function<void()> fn)`

- [ ] **Step 1: Create `form_widgets.h`**

```cpp
// Small shared label/row factories used by the app's dialogs. Extracted from
// the copies that lived in settings_dialog.cpp's anon namespace and camera's
// page_util, so there is a single definition. Leaf: depends only on Qt.
#pragma once

#include <QString>

class QFrame;
class QLabel;
class QWidget;

namespace denso::ui::common {

/// Bold, letter-spaced, faint section header (the "APPEARANCE"/"NETWORK" caps).
QLabel* eyebrow(const QString& text);

/// A dimmed secondary label (property "dim").
QLabel* dim_label(const QString& text);

/// A "label … value" row; the value QLabel is returned via value_out for later
/// text updates.
QWidget* spec_row(const QString& label, QLabel** value_out);

/// A 1px horizontal divider.
QFrame* hline();

} // namespace denso::ui::common
```

- [ ] **Step 2: Create `form_widgets.cpp`** (bodies moved verbatim from `settings_dialog.cpp` lines 30–63)

```cpp
#include "ui/common/form_widgets.h"

#include <QFont>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QWidget>

namespace denso::ui::common {

QLabel* eyebrow(const QString& text) {
    auto* l = new QLabel(text);
    l->setProperty("faint", true);
    QFont f = l->font();
    f.setBold(true);
    f.setPointSizeF(f.pointSizeF() - 1.0);
    f.setLetterSpacing(QFont::AbsoluteSpacing, 1.5);
    l->setFont(f);
    return l;
}

QLabel* dim_label(const QString& text) {
    auto* l = new QLabel(text);
    l->setProperty("dim", true);
    return l;
}

QWidget* spec_row(const QString& label, QLabel** value_out) {
    auto* w = new QWidget;
    auto* row = new QHBoxLayout(w);
    row->setContentsMargins(0, 0, 0, 0);
    row->addWidget(dim_label(label), 1);
    auto* v = new QLabel;
    row->addWidget(v, 0);
    *value_out = v;
    return w;
}

QFrame* hline() {
    auto* line = new QFrame;
    line->setFrameShape(QFrame::HLine);
    line->setFixedHeight(1);
    return line;
}

} // namespace denso::ui::common
```

- [ ] **Step 3: Create `dialog_chrome.h`**

```cpp
// The shared modal header: a bold title, a ✕ close-glyph wired to the dialog's
// reject(), and the gold underline rule. Extracted from the duplicated header
// blocks in settings_dialog.cpp and camera_dialog.cpp. Leaf: depends only on Qt.
#pragma once

#include <QString>

class QDialog;
class QVBoxLayout;

namespace denso::ui::common {

/// Build the header layout for a modal: `title` + close glyph (→ dlg->reject())
/// + gold underline. The caller adds the returned layout to its outer layout.
QVBoxLayout* dialog_header(QDialog* dlg, const QString& title);

} // namespace denso::ui::common
```

- [ ] **Step 4: Create `dialog_chrome.cpp`** (generalized from `camera_dialog.cpp` lines 37–58 — title is now a parameter)

```cpp
#include "ui/common/dialog_chrome.h"

#include <QDialog>
#include <QFont>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

namespace denso::ui::common {

QVBoxLayout* dialog_header(QDialog* dlg, const QString& title) {
    auto* h = new QVBoxLayout;
    h->setSpacing(10);
    auto* row = new QHBoxLayout;
    auto* label = new QLabel(title);
    QFont tf = label->font();
    tf.setBold(true);
    tf.setPointSizeF(tf.pointSizeF() + 6.0);
    label->setFont(tf);
    auto* close_glyph = new QPushButton(QStringLiteral("✕"));
    close_glyph->setProperty("flatText", true);
    close_glyph->setFixedSize(28, 28);
    QObject::connect(close_glyph, &QPushButton::clicked, dlg, &QDialog::reject);
    row->addWidget(label, 1);
    row->addWidget(close_glyph, 0);
    h->addLayout(row);
    auto* underline = new QFrame;
    underline->setObjectName(QStringLiteral("goldUnderline"));
    underline->setFixedSize(48, 3);
    h->addWidget(underline, 0, Qt::AlignLeft);
    return h;
}

} // namespace denso::ui::common
```

- [ ] **Step 5: Create `async_runner.h`**

```cpp
// The dialogs' shared "run blocking OS work off the GUI thread, post the result
// back" idiom, extracted from SettingsDialog::run_async and the inline
// QThread::create in CameraDialog::capture_snapshot. Leaf: depends only on Qt.
#pragma once

#include <functional>

class QObject;

namespace denso::ui::common {

/// Run `work` on a throwaway worker QThread (auto-deleted when it finishes). A
/// real QThread is used (not QtConcurrent) so QProcess inside `work` has an
/// event dispatcher — the Qt analog of std::thread + upgrade_in_event_loop.
void run_on_worker(std::function<void()> work);

/// Post `fn` to `ctx`'s thread (the GUI thread) as a queued call. Use from
/// inside worker `work` to marshal results back to widgets.
void post_to_gui(QObject* ctx, std::function<void()> fn);

} // namespace denso::ui::common
```

- [ ] **Step 6: Create `async_runner.cpp`**

```cpp
#include "ui/common/async_runner.h"

#include <QMetaObject>
#include <QObject>
#include <QThread>

#include <utility>

namespace denso::ui::common {

void run_on_worker(std::function<void()> work) {
    auto* thread = QThread::create(std::move(work));
    QObject::connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();
}

void post_to_gui(QObject* ctx, std::function<void()> fn) {
    QMetaObject::invokeMethod(ctx, std::move(fn), Qt::QueuedConnection);
}

} // namespace denso::ui::common
```

- [ ] **Step 7: Register the three sources**

In `src/app/CMakeLists.txt`, add a `# common dialog primitives` group immediately after the `# app shell` block (after `ui/startup.cpp`, before `# settings modal`):

```cmake
    ui/startup.cpp
    # common dialog primitives (leaf — used by every dialog)
    ui/common/form_widgets.cpp
    ui/common/dialog_chrome.cpp
    ui/common/async_runner.cpp
    # settings modal
    ui/settings/settings_dialog.cpp
```

- [ ] **Step 8: Build — verify it compiles**

Run:

```bash
export PATH=/c/msys64/ucrt64/bin:$PATH
cmake -S . -B build -G Ninja
cmake --build build
```

Expected: build succeeds. Nothing consumes the new module yet (it's added in Tasks 3–5), so there is no behavior change; this step only proves the new files compile and link into `denso`.

- [ ] **Step 9: Commit**

```bash
git add src/app/ui/common src/app/CMakeLists.txt
git commit -m "feat(ui): shared ui/common dialog primitives (chrome, async, labels)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Task 3: `SettingsDialog` → thin view + `NetworkPanel`

**Files:**
- Create: `src/app/ui/settings/network_panel.h`
- Create: `src/app/ui/settings/network_panel.cpp`
- Modify: `src/app/ui/settings/settings_dialog.h`
- Modify: `src/app/ui/settings/settings_dialog.cpp`
- Modify: `src/app/CMakeLists.txt` (register `network_panel.cpp`)

**Interfaces:**
- Consumes: `common::eyebrow`, `common::run_on_worker`, `common::post_to_gui` (Task 2); `NetCard`, `network::*`, `to_net_status`/`to_ui_config`/`from_ui_config`/`wifi_rows` (existing).
- Produces: `class denso::ui::NetworkPanel : public QWidget` with `explicit NetworkPanel(QSqlDatabase db, QWidget* parent = nullptr)` and `void on_shown()`.

- [ ] **Step 1: Create `network_panel.h`**

```cpp
// The Network settings page as a self-contained widget: it owns the two
// NetCards, the Refresh button, the DB handle, and the threaded
// scan/connect/refresh/apply handlers. Extracted from SettingsDialog so the
// dialog is a thin view. on_shown() re-seeds the editors from saved config and
// refreshes live status, reproducing the "entering the Network tab reloads" of
// the Slint original.
#pragma once

#include "ui/viewmodel.h"

#include <QSqlDatabase>
#include <QWidget>

#include <string>

class QPushButton;

namespace denso::ui {

class NetCard;

class NetworkPanel : public QWidget {
    Q_OBJECT

public:
    explicit NetworkPanel(QSqlDatabase db, QWidget* parent = nullptr);

    /// Re-seed both cards from saved config, then refresh live status. Called by
    /// the settings dialog when the Network tab becomes visible.
    void on_shown();

private:
    void refresh_network();
    void apply_net_config(const std::string& iface, const NetConfigUi& ui);
    void scan_wifi();
    void connect_wifi(const std::string& ssid, const std::string& password);

    QSqlDatabase db_;
    QPushButton* refresh_btn_ = nullptr;
    NetCard* eth_card_ = nullptr;
    NetCard* wifi_card_ = nullptr;
    // Editors re-seed from these on each on_shown() (so un-applied edits are
    // discarded); apply_net_config refreshes them.
    NetConfigUi eth_config_;
    NetConfigUi wifi_config_;
};

} // namespace denso::ui
```

- [ ] **Step 2: Create `network_panel.cpp`** (constructor built from `settings_dialog.cpp`'s `build_network()` + the nav slot's row==3 branch; handlers moved verbatim, `run_async`→`common::run_on_worker`, `QMetaObject::invokeMethod`→`common::post_to_gui`)

```cpp
#include "ui/settings/network_panel.h"

#include "network/backend.h"
#include "network/model.h"
#include "network/repo.h"
#include "ui/common/async_runner.h"
#include "ui/common/form_widgets.h"
#include "ui/convert.h"
#include "ui/settings/netcard.h"

#include <QHBoxLayout>
#include <QPushButton>
#include <QVBoxLayout>

#include <exception>
#include <optional>
#include <vector>

namespace denso::ui {
namespace {

/// Saved config for `iface`, or a DHCP default when none is stored yet.
NetConfigUi load_config_or_default(const QSqlDatabase& db, const std::string& iface) {
    std::optional<network::NetConfig> stored = network::load(db, iface);
    if (stored) return to_ui_config(*stored);
    network::NetConfig def;
    def.iface = iface;
    def.mode = "dhcp";
    return to_ui_config(def);
}

} // namespace

NetworkPanel::NetworkPanel(QSqlDatabase db, QWidget* parent)
    : QWidget(parent), db_(std::move(db)) {
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(12);

    auto* head = new QHBoxLayout;
    head->addWidget(common::eyebrow(QStringLiteral("NETWORK")), 1);
    refresh_btn_ = new QPushButton(QStringLiteral("Refresh"));
    refresh_btn_->setProperty("flatText", true);
    connect(refresh_btn_, &QPushButton::clicked, this, [this] { refresh_network(); });
    head->addWidget(refresh_btn_, 0);
    v->addLayout(head);

    eth_config_ = load_config_or_default(db_, "ethernet");
    eth_card_ = new NetCard(QStringLiteral("Ethernet"), "ethernet", false, eth_config_);
    eth_card_->on_apply = [this](const std::string& iface, const NetConfigUi& ui) {
        apply_net_config(iface, ui);
    };
    v->addWidget(eth_card_);

    wifi_config_ = load_config_or_default(db_, "wifi");
    wifi_card_ = new NetCard(QStringLiteral("Wi-Fi"), "wifi", true, wifi_config_);
    wifi_card_->on_apply = [this](const std::string& iface, const NetConfigUi& ui) {
        apply_net_config(iface, ui);
    };
    wifi_card_->on_scan = [this] { scan_wifi(); };
    wifi_card_->on_connect = [this](const std::string& ssid, const std::string& pw) {
        connect_wifi(ssid, pw);
    };
    v->addWidget(wifi_card_);
    v->addStretch(1);
}

void NetworkPanel::on_shown() {
    // Re-seed the editors from the saved config (discarding un-applied edits),
    // then refresh live status — the Slint nav callback's behavior.
    eth_card_->set_config(eth_config_);
    wifi_card_->set_config(wifi_config_);
    refresh_network();
}

void NetworkPanel::refresh_network() {
    refresh_btn_->setText(QStringLiteral("Loading…"));
    common::run_on_worker([this] {
        const network::NetworkSnapshot snap = network::backend()->snapshot();
        common::post_to_gui(this, [this, snap] {
            eth_card_->set_status(to_net_status(snap.ethernet));
            wifi_card_->set_status(to_net_status(snap.wifi));
            refresh_btn_->setText(QStringLiteral("Refresh"));
        });
    });
}

void NetworkPanel::apply_net_config(const std::string& iface, const NetConfigUi& ui) {
    const network::NetConfig cfg = from_ui_config(iface, ui);
    network::save(db_, cfg);  // app owns the truth; persist before pushing
    QString status;
    try {
        network::backend()->apply_config(cfg);
        status = QStringLiteral("Applied");
    } catch (const std::exception& e) {
        status = QStringLiteral("Error: %1").arg(QString::fromUtf8(e.what()));
    }
    const NetConfigUi canonical = to_ui_config(cfg);
    (iface == "wifi" ? wifi_config_ : eth_config_) = canonical;
    NetCard* card = iface == "wifi" ? wifi_card_ : eth_card_;
    card->set_config(canonical);
    card->set_config_status(status);
}

void NetworkPanel::scan_wifi() {
    wifi_card_->set_scanning(true);
    const std::string current_ssid = wifi_card_->current_ssid();
    common::run_on_worker([this, current_ssid] {
        std::optional<std::vector<network::WifiNetwork>> nets;
        std::string err;
        try {
            nets = network::backend()->scan_wifi();
        } catch (const std::exception& e) {
            err = e.what();
        }
        common::post_to_gui(this, [this, nets, err, current_ssid] {
            if (nets)
                wifi_card_->set_networks(wifi_rows(*nets, current_ssid));
            else
                wifi_card_->set_connect_status(
                    QStringLiteral("Scan failed: %1").arg(QString::fromStdString(err)));
            wifi_card_->set_scanning(false);
        });
    });
}

void NetworkPanel::connect_wifi(const std::string& ssid, const std::string& password) {
    wifi_card_->set_connect_status(
        QStringLiteral("Connecting to %1…").arg(QString::fromStdString(ssid)));
    common::run_on_worker([this, ssid, password] {
        const std::optional<std::string> pw =
            password.empty() ? std::nullopt : std::optional<std::string>(password);
        bool ok = true;
        std::string err;
        try {
            network::backend()->connect_wifi(ssid, pw);
        } catch (const std::exception& e) {
            ok = false;
            err = e.what();
        }
        common::post_to_gui(this, [this, ssid, ok, err] {
            wifi_card_->set_connect_status(
                ok ? QStringLiteral("Connected to %1").arg(QString::fromStdString(ssid))
                   : QStringLiteral("Error: %1").arg(QString::fromStdString(err)));
        });
    });
}

} // namespace denso::ui
```

- [ ] **Step 3: Slim `settings_dialog.h`**

Replace the `#include`s block near the top so `<functional>` and the network forward-decls go away and the panel is declared. Specifically:

Remove the line:
```cpp
#include <functional>
```

Change the forward declaration `class NetCard;` to:
```cpp
class NetworkPanel;
```

Remove these member/method declarations from the `private:` section:
```cpp
    // Network handlers (own the DB + the per-call backend, like the Rust wiring).
    void refresh_network();
    void apply_net_config(const std::string& iface, const NetConfigUi& ui);
    void scan_wifi();
    void connect_wifi(const std::string& ssid, const std::string& password);

    // Run blocking OS work on a worker thread, posting results back to the GUI
    // thread (the Qt analog of `std::thread` + `upgrade_in_event_loop`). A real
    // QThread is used so QProcess in the platform backends has an event
    // dispatcher.
    void run_async(std::function<void()> work);
```

Replace the Network members block:
```cpp
    // Network. The configs mirror the Slint window's eth-config/wifi-config
    // properties: the editors re-seed from them on each Network-tab entry (so
    // un-applied edits are discarded), and Apply refreshes them.
    QPushButton* refresh_btn_ = nullptr;
    NetCard* eth_card_ = nullptr;
    NetCard* wifi_card_ = nullptr;
    NetConfigUi eth_config_;
    NetConfigUi wifi_config_;
```
with:
```cpp
    // Network page — a self-contained widget owning its cards + async handlers.
    NetworkPanel* network_panel_ = nullptr;
```

Because `NetConfigUi` is no longer named in the header, also remove `#include "ui/viewmodel.h"` **only if** nothing else in the header uses it — it does not, so remove it. (The header keeps `QDialog`, `QSqlDatabase`, `QString`.)

- [ ] **Step 4: Slim `settings_dialog.cpp`**

Apply these edits:

1. Replace the top includes block (lines 1–25) with:
```cpp
#include "ui/settings/settings_dialog.h"

#include "ui/common/form_widgets.h"
#include "ui/settings/network_panel.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFont>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QScrollArea>
#include <QShowEvent>
#include <QStackedWidget>
#include <QVBoxLayout>
```

2. Delete the entire anonymous-namespace helper block (old lines 28–76): the `eyebrow`, `dim_label`, `spec_row`, `hline`, and `load_config_or_default` definitions and the `namespace {` / `} // namespace` around them. They now live in `ui/common/form_widgets` and `network_panel.cpp`.

3. In `build_appearance`, `build_display`, `build_system`, `build_about`, prefix the helper calls with `common::` — i.e. `eyebrow(...)`→`common::eyebrow(...)`, `dim_label(...)`→`common::dim_label(...)`, `spec_row(...)`→`common::spec_row(...)`. (These four methods otherwise stay exactly as they are.)

4. Replace `build_network()` (old lines 230–262) entirely with:
```cpp
QWidget* SettingsDialog::build_network() {
    network_panel_ = new NetworkPanel(db_);
    return network_panel_;
}
```

5. In the constructor, replace the nav `currentRowChanged` lambda (old lines 135–146) with:
```cpp
    connect(nav_, &QListWidget::currentRowChanged, this, [this](int row) {
        if (row < 0) return;
        stack_->setCurrentIndex(row);
        if (row == 3) {  // Network tab: re-seed cards + refresh status
            network_panel_->on_shown();
        }
    });
```

6. In the footer setup, the `hline()` call (old line 149) becomes `common::hline()`.

7. Delete the `run_async`, `refresh_network`, `apply_net_config`, `scan_wifi`, and `connect_wifi` method definitions (old lines 320–408) and the `// ── Network handlers ──` banner comment. They moved to `network_panel.cpp`.

- [ ] **Step 5: Register `network_panel.cpp`**

In `src/app/CMakeLists.txt`, under `# settings modal`, add it after `settings_dialog.cpp`:
```cmake
    # settings modal
    ui/settings/settings_dialog.cpp
    ui/settings/network_panel.cpp
    ui/settings/netcard.cpp
```

- [ ] **Step 6: Build — verify it compiles**

Run:
```bash
export PATH=/c/msys64/ucrt64/bin:$PATH
cmake -S . -B build -G Ninja
cmake --build build
```
Expected: build succeeds with no warnings about unused members or missing symbols.

- [ ] **Step 7: Manual smoke test (behavior preservation)**

Run `./build/src/app/denso` and verify the Settings modal is unchanged:
- Open **Settings** → **Appearance**: toggling Dark mode restyles the app.
- **Display**: Resolution combo + Fullscreen toggle behave as before; **Apply** resizes and closes.
- **System**: OS/Device/RAM/Storage rows show values.
- **Network**: entering the tab shows "Loading…" then live status on both cards; edit Ethernet IP → **Apply** shows "Applied"; Wi-Fi **Scan** lists networks; **Connect** shows the connecting/connected status.
- **About**: shows the version.
- Re-open Settings → it starts on **Appearance** (nav reset) and the Network cards re-seed.

Expected: identical to pre-refactor behavior.

- [ ] **Step 8: Commit**

```bash
git add src/app/ui/settings src/app/CMakeLists.txt
git commit -m "refactor(settings): extract NetworkPanel, thin SettingsDialog to a view

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Task 4: `CameraDialog` → thin view + `CameraWizardController`

**Files:**
- Create: `src/app/ui/camera/wizard_controller.h`
- Create: `src/app/ui/camera/wizard_controller.cpp`
- Modify: `src/app/ui/camera/camera_dialog.h`
- Modify: `src/app/ui/camera/camera_dialog.cpp`
- Modify: `src/app/CMakeLists.txt` (register `wizard_controller.cpp`)

**Interfaces:**
- Consumes: `common::dialog_header`, `common::run_on_worker`, `common::post_to_gui` (Task 2); pages `CameraConfigurePage`/`ModelsPage`/`CameraAreasPage` + `camera::*`/`detection::set_camera_models`/`with_credentials`/`grab_snapshot`/`apply_orientation` (existing).
- Produces: `class denso::ui::CameraWizardController : public QObject` with the `Pages` struct, constructor `(QSqlDatabase, Pages, std::function<void(int)> show_page, QObject* parent=nullptr)`, the public slots listed below, and signals `cameras_changed()` + `request_show_list()`.

- [ ] **Step 1: Create `wizard_controller.h`**

```cpp
// Owns the Camera wizard's flow: the add/edit state, the threaded snapshot
// capture, and every DB write (camera insert/update, model attach, ROI replace).
// Extracted from CameraDialog so the dialog is a thin view over the page stack.
// The controller never touches the QStackedWidget or the stepper directly — it
// asks the view to switch pages through the injected show_page callback and
// signals request_show_list() for transitions that return to the list.
#pragma once

#include "camera/camera.h"

#include <QImage>
#include <QObject>
#include <QSqlDatabase>
#include <QString>

#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

namespace denso::ui {

class CameraConfigurePage;
class ModelsPage;
class CameraAreasPage;

class CameraWizardController : public QObject {
    Q_OBJECT

public:
    // The three interactive wizard pages the controller drives. Owned by the
    // dialog; the controller only reads/populates them.
    struct Pages {
        CameraConfigurePage* configure = nullptr;
        ModelsPage* models = nullptr;
        CameraAreasPage* areas = nullptr;
    };

    CameraWizardController(QSqlDatabase db, Pages pages,
                           std::function<void(int)> show_page,
                           QObject* parent = nullptr);

signals:
    void cameras_changed();     // camera set changed → main view refreshes
    void request_show_list();   // return to the list page (view owns that switch)

public slots:
    // Configure flow.
    void begin_configure(const camera::Camera& cam, std::optional<int64_t> id,
                         const QString& preview_text);  // seed draft + open Configure
    void capture_snapshot();       // threaded grab → push frame to the pages
    void save_configured_camera(); // insert/update from draft_, then Models step
    void configure_back();         // Configure Back: edit→list, add→Source page

    // Models flow.
    void save_models();            // persist attachments → advance to Areas step

    // Areas flow.
    void begin_areas_direct(const camera::Camera& cam);  // per-row Areas button
    void save_areas(const std::vector<camera::CameraArea>& areas);  // persist + list
    void areas_back();             // Areas Back: direct→list, wizard→Models step

private:
    void enter_models();           // load catalog + attachments → Models page
    void enter_areas(bool direct); // load areas + frame → Areas page
    void update_areas_background(); // push the oriented frame to the Areas canvas

    QSqlDatabase db_;
    Pages pages_;
    std::function<void(int)> show_page_;

    std::optional<int64_t> editing_id_;  // set in edit mode; empty when adding
    camera::Camera draft_;               // camera being added/edited
    QImage last_frame_;                  // most recent un-rotated snapshot frame
    bool entered_areas_directly_ = false;  // true: per-row Areas (Back → list)
};

} // namespace denso::ui
```

- [ ] **Step 2: Create `wizard_controller.cpp`** (methods moved verbatim from `camera_dialog.cpp`; `show_page(n)`→`show_page_(n)`; the two return-to-list paths emit `request_show_list()`; the threaded capture uses `common::run_on_worker`/`common::post_to_gui`)

```cpp
#include "ui/camera/wizard_controller.h"

#include "camera/repo.h"
#include "detection/repo.h"
#include "ui/camera/dialog/areas_page.h"
#include "ui/camera/dialog/configure_page.h"
#include "ui/camera/dialog/models_page.h"
#include "ui/camera/shared/rtsp_templates.h"  // with_credentials
#include "ui/camera/shared/snapshot.h"        // grab_snapshot, apply_orientation
#include "ui/common/async_runner.h"

#include <QSize>

#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace denso::ui {

CameraWizardController::CameraWizardController(QSqlDatabase db, Pages pages,
                                              std::function<void(int)> show_page,
                                              QObject* parent)
    : QObject(parent), db_(std::move(db)), pages_(pages),
      show_page_(std::move(show_page)) {}

void CameraWizardController::begin_configure(const camera::Camera& cam,
                                             std::optional<int64_t> id,
                                             const QString& preview_text) {
    editing_id_ = id;
    draft_ = cam;
    last_frame_ = QImage();
    pages_.configure->populate(draft_);
    pages_.configure->set_preview_text(preview_text);
    pages_.configure->clear_error();
    show_page_(2);
    capture_snapshot();
}

void CameraWizardController::capture_snapshot() {
    pages_.configure->set_capturing(true);
    pages_.configure->set_preview_text(QStringLiteral("Capturing…"));

    std::optional<int> index;
    QString url;
    if (draft_.camera_type == "usb") {
        index = draft_.index ? std::optional<int>(static_cast<int>(*draft_.index))
                             : std::optional<int>(0);
    } else {
        const QString rtsp = draft_.rtsp ? QString::fromStdString(*draft_.rtsp) : QString();
        const QString user = draft_.username ? QString::fromStdString(*draft_.username) : QString();
        const QString pass = draft_.password ? QString::fromStdString(*draft_.password) : QString();
        url = with_credentials(rtsp, user, pass);
    }
    const QSize res = pages_.configure->resolution();

    common::run_on_worker([this, index, url, res] {
        const Snapshot snap = grab_snapshot(index, url, res.width(), res.height());
        common::post_to_gui(this, [this, snap] {
            pages_.configure->set_capturing(false);
            if (snap.image.isNull()) {
                pages_.configure->set_preview_text(snap.error);
                return;
            }
            last_frame_ = snap.image;
            pages_.configure->set_frame(last_frame_);
            update_areas_background();  // refresh the ROI canvas if it's showing
        });
    });
}

void CameraWizardController::save_configured_camera() {
    pages_.configure->read_into(draft_);

    if (editing_id_.has_value()) {
        draft_.id = *editing_id_;
        if (!camera::update(db_, draft_)) {
            pages_.configure->show_error(QStringLiteral("Failed to save the camera."));
            return;
        }
    } else {
        const auto new_id = camera::insert(db_, draft_);
        if (!new_id.has_value()) {
            pages_.configure->show_error(QStringLiteral("Failed to save the camera."));
            return;
        }
        editing_id_ = *new_id;
        draft_.id = *new_id;
    }
    emit cameras_changed();
    enter_models();
}

void CameraWizardController::configure_back() {
    // Editing an existing camera has no Source step to return to.
    if (editing_id_.has_value())
        emit request_show_list();
    else
        show_page_(1);
}

void CameraWizardController::enter_models() {
    pages_.models->load_for(editing_id_.value_or(0));
    show_page_(3);
}

void CameraWizardController::save_models() {
    if (editing_id_.has_value()) {
        denso::detection::set_camera_models(
            db_, *editing_id_, pages_.models->selections(*editing_id_));
        emit cameras_changed();
    }
    enter_areas(/*direct=*/false);
}

void CameraWizardController::begin_areas_direct(const camera::Camera& cam) {
    editing_id_ = cam.id;
    draft_ = cam;
    last_frame_ = QImage();
    enter_areas(/*direct=*/true);
    capture_snapshot();
}

void CameraWizardController::enter_areas(bool direct) {
    entered_areas_directly_ = direct;
    pages_.areas->load(editing_id_.has_value()
                           ? camera::areas_for(db_, *editing_id_)
                           : std::vector<camera::CameraArea>{});
    update_areas_background();
    show_page_(4);
}

void CameraWizardController::update_areas_background() {
    if (last_frame_.isNull()) {
        pages_.areas->set_background(QImage());
        return;
    }
    pages_.areas->set_background(apply_orientation(
        last_frame_, static_cast<int>(draft_.rotation), draft_.pitch, draft_.roll));
}

void CameraWizardController::save_areas(const std::vector<camera::CameraArea>& areas) {
    if (editing_id_.has_value() &&
        !camera::replace_areas(db_, *editing_id_, areas)) {
        pages_.areas->show_save_error();
        return;
    }
    emit request_show_list();
}

void CameraWizardController::areas_back() {
    // Direct entry (per-row Areas) has no Models step to return to.
    if (entered_areas_directly_)
        emit request_show_list();
    else
        show_page_(3);
}

} // namespace denso::ui
```

- [ ] **Step 3: Slim `camera_dialog.h`**

Add a forward declaration for the controller and drop the flow-state members the controller now owns.

Add to the forward-decl block (after `class CameraAreasPage;`):
```cpp
class CameraWizardController;
```

Remove these private method declarations (the controller owns them now):
```cpp
    // Configure flow (the coordinator owns the camera source + DB writes).
    void begin_configure(const camera::Camera& cam, std::optional<int64_t> id,
                         const QString& preview_text);  // seed draft + open Configure
    void capture_snapshot();       // threaded grab → push frame to the pages
    void save_configured_camera(); // insert/update from draft_, then Models step

    // Models flow.
    void enter_models();  // load the catalog + current attachments → Models page
    void save_models();   // persist the attached models → advance to Areas step

    // Areas flow.
    void enter_areas(bool direct);  // load areas + frame → Areas page
    void update_areas_background();  // push the oriented frame to the Areas canvas
    void save_areas(const std::vector<camera::CameraArea>& areas);  // persist + list
```

Keep `show_page`, `show_list`, `show_add`, `expand_for_areas`, `restore_size`. Add a controller pointer and remove the migrated state members. Replace the members block (old lines 78–86):
```cpp
    // Add/edit mode state.
    std::optional<int64_t> editing_id_;  // set in edit mode; empty when adding
    camera::Camera draft_;               // camera being added/edited
    QImage last_frame_;                  // most recent un-rotated snapshot frame

    // Areas-step sizing + Back routing.
    bool areas_expanded_ = false;          // modal currently grown for drawing
    QRect pre_areas_geometry_;             // geometry to restore when leaving Areas
    bool entered_areas_directly_ = false;  // true: per-row Areas button (Back → list)
```
with:
```cpp
    CameraWizardController* controller_ = nullptr;  // owns flow-state + persistence

    // Areas-step sizing (view-owned: only the dialog resizes).
    bool areas_expanded_ = false;  // modal currently grown for drawing
    QRect pre_areas_geometry_;     // geometry to restore when leaving Areas
```

`#include "camera/camera.h"` stays (the constructor lambdas still name `camera::Camera`/`camera::CameraArea`). `QImage` include may now be unused in the header — keep it only if still referenced; it is not, so remove `#include <QImage>` and `#include <cstdint>` from the header (they move to the controller). Keep `<optional>` and `<vector>` (used by the page-signal lambdas' parameter types in the .cpp — but those are in the .cpp; if the header no longer names `std::optional`/`std::vector`, remove them too). After editing, the header names: `QDialog`, `QRect`, `QSqlDatabase`, `QString` (via signals/methods). Remove now-unused std includes accordingly and let the build confirm.

- [ ] **Step 4: Rewrite `camera_dialog.cpp`**

Replace the whole file with the view-only version below. It constructs the pages + stepper + header (now via `common::dialog_header`), builds the controller, wires page signals to controller slots, and keeps only page-switching/sizing.

```cpp
#include "ui/camera/camera_dialog.h"

#include "camera/camera.h"
#include "ui/camera/dialog/add_page.h"
#include "ui/camera/dialog/areas_page.h"
#include "ui/camera/dialog/configure_page.h"
#include "ui/camera/dialog/list_page.h"
#include "ui/camera/dialog/models_page.h"
#include "ui/camera/dialog/wizard_stepper.h"
#include "ui/camera/wizard_controller.h"
#include "ui/common/dialog_chrome.h"

#include <QPoint>
#include <QRect>
#include <QScreen>
#include <QShowEvent>
#include <QStackedWidget>
#include <QVBoxLayout>

#include <optional>

namespace denso::ui {

CameraDialog::CameraDialog(QSqlDatabase db, QWidget* parent)
    : QDialog(parent), db_(std::move(db)) {
    setWindowTitle(QStringLiteral("Camera"));
    setObjectName(QStringLiteral("dialogPanel"));
    resize(760, 600);

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(24, 24, 24, 24);
    outer->setSpacing(22);
    outer->addLayout(common::dialog_header(this, QStringLiteral("Camera")));

    // Wizard step indicator — shown only during the add/edit flow (pages 1–3),
    // hidden on the list; driven by show_page().
    stepper_ = new WizardStepper(
        {QStringLiteral("Source"), QStringLiteral("Configure"),
         QStringLiteral("Models"), QStringLiteral("Areas")});
    stepper_->setVisible(false);
    outer->addWidget(stepper_, 0);

    stack_ = new QStackedWidget;

    list_page_ = new CameraListPage(db_);            // index 0
    add_page_ = new CameraAddPage;                   // index 1
    configure_page_ = new CameraConfigurePage;       // index 2
    models_page_ = new ModelsPage;                   // index 3
    areas_page_ = new CameraAreasPage;               // index 4
    models_page_->set_db(db_);
    stack_->addWidget(list_page_);
    stack_->addWidget(add_page_);
    stack_->addWidget(configure_page_);
    stack_->addWidget(models_page_);
    stack_->addWidget(areas_page_);
    outer->addWidget(stack_, 1);

    // The controller owns flow-state + persistence; the dialog owns widgets +
    // sizing. It drives page switches through this show_page callback.
    controller_ = new CameraWizardController(
        db_, CameraWizardController::Pages{configure_page_, models_page_, areas_page_},
        [this](int index) { show_page(index); }, this);
    connect(controller_, &CameraWizardController::cameras_changed, this,
            &CameraDialog::cameras_changed);
    connect(controller_, &CameraWizardController::request_show_list, this,
            &CameraDialog::show_list);

    // ── List page signals ─────────────────────────────────────────────────
    connect(list_page_, &CameraListPage::add_requested, this, &CameraDialog::show_add);
    connect(list_page_, &CameraListPage::configure_requested, this,
            [this](const camera::Camera& cam) {
                controller_->begin_configure(cam, cam.id, QStringLiteral("Capturing…"));
            });
    connect(list_page_, &CameraListPage::areas_requested, this,
            [this](const camera::Camera& cam) { controller_->begin_areas_direct(cam); });
    connect(list_page_, &CameraListPage::changed, this, &CameraDialog::cameras_changed);

    // ── Add / Source page signals ─────────────────────────────────────────
    connect(add_page_, &CameraAddPage::cancel_requested, this, &CameraDialog::show_list);
    connect(add_page_, &CameraAddPage::next_requested, this,
            [this](const camera::Camera& draft) {
                controller_->begin_configure(draft, std::nullopt,
                                             QStringLiteral("Click Capture to preview"));
            });

    // ── Configure page signals ────────────────────────────────────────────
    connect(configure_page_, &CameraConfigurePage::back_requested, controller_,
            &CameraWizardController::configure_back);
    connect(configure_page_, &CameraConfigurePage::next_requested, controller_,
            &CameraWizardController::save_configured_camera);
    connect(configure_page_, &CameraConfigurePage::capture_requested, controller_,
            &CameraWizardController::capture_snapshot);

    // ── Models page signals ───────────────────────────────────────────────
    connect(models_page_, &ModelsPage::back_requested, this, [this] { show_page(2); });
    connect(models_page_, &ModelsPage::finish_requested, controller_,
            &CameraWizardController::save_models);

    // ── Areas page signals ────────────────────────────────────────────────
    connect(areas_page_, &CameraAreasPage::back_requested, controller_,
            &CameraWizardController::areas_back);
    connect(areas_page_, &CameraAreasPage::skip_requested, this, &CameraDialog::show_list);
    connect(areas_page_, &CameraAreasPage::save_requested, controller_,
            &CameraWizardController::save_areas);

    list_page_->reload();
}

void CameraDialog::showEvent(QShowEvent* e) {
    QDialog::showEvent(e);
    // The dialog is created once and reused; always reopen on the list page at
    // the compact size, even if it was closed mid-flow on the expanded Areas step.
    show_list();
}

void CameraDialog::show_page(int index) {
    // The stepper belongs to the add/edit flow (pages 1–3), not the list.
    stepper_->setVisible(index >= 1);
    if (index >= 1) {
        stepper_->set_current(index - 1);  // page 1→step 0, 2→1, 3→2
    }
    // Near-fullscreen only while drawing areas; restore the compact size else.
    if (index == 4) {
        expand_for_areas();
    } else {
        restore_size();
    }
    stack_->setCurrentIndex(index);
}

void CameraDialog::expand_for_areas() {
    if (areas_expanded_) {
        return;
    }
    pre_areas_geometry_ = geometry();
    areas_expanded_ = true;
    if (QScreen* s = screen()) {
        const QRect avail = s->availableGeometry();
        const int w = static_cast<int>(avail.width() * 0.92);
        const int h = static_cast<int>(avail.height() * 0.92);
        resize(w, h);
        move(avail.center() - QPoint(w / 2, h / 2));
    }
}

void CameraDialog::restore_size() {
    if (!areas_expanded_) {
        return;
    }
    areas_expanded_ = false;
    setGeometry(pre_areas_geometry_);
}

void CameraDialog::show_list() {
    list_page_->reload();
    show_page(0);
}

void CameraDialog::show_add() {
    add_page_->reset();
    show_page(1);
}

} // namespace denso::ui
```

- [ ] **Step 5: Register `wizard_controller.cpp`**

In `src/app/CMakeLists.txt`, under `# camera management — entry points`, add it after `camera_dialog.cpp`:
```cmake
    # camera management — entry points
    ui/camera/camera_view.cpp
    ui/camera/camera_dialog.cpp
    ui/camera/wizard_controller.cpp
```

- [ ] **Step 6: Build — verify it compiles**

Run:
```bash
export PATH=/c/msys64/ucrt64/bin:$PATH
cmake -S . -B build -G Ninja
cmake --build build
```
Expected: build succeeds. If the header trimming of includes was too aggressive (e.g. a `std::optional` still named), the compiler will point to the exact line — re-add that single include.

- [ ] **Step 7: Manual smoke test (behavior preservation)**

Run `./build/src/app/denso`, open **Camera**, and walk every flow:
- **Add USB:** + Add → pick USB device → Next → Configure: **Capture** shows a preview; set rotation/pitch/roll → Next saves the camera → **Models**: attach a model + set class confidence → Next → **Areas**: draw a polygon → **Finish** returns to the list with the camera present.
- **Add IP:** same, entering manufacturer/stream/credentials; the RTSP preview builds; Capture works.
- **Back routing:** From Configure in *add* mode, **Back** returns to Source; in *edit* mode (Configure from the list), **Back** returns to the list. From Areas reached via the wizard, **Back** returns to Models; from the per-row **Areas** button, **Back** returns to the list.
- **Skip Areas:** on the Areas step, **Skip** returns to the list without writing ROIs.
- **Per-row Areas:** a list row's Areas button opens Areas directly with the saved ROIs + a fresh snapshot.
- **Delete** a camera from the list.
- **Areas sizing:** the modal grows near-fullscreen on Areas and restores on leaving; re-opening the dialog starts compact on the list.

Expected: identical to pre-refactor behavior; the live grid refreshes after saves (cameras_changed).

- [ ] **Step 8: Commit**

```bash
git add src/app/ui/camera/wizard_controller.h src/app/ui/camera/wizard_controller.cpp \
        src/app/ui/camera/camera_dialog.h src/app/ui/camera/camera_dialog.cpp \
        src/app/CMakeLists.txt
git commit -m "refactor(camera): extract CameraWizardController, thin CameraDialog to a view

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Task 5: Dormant `ReadingSink` seam in `DetectionProcessor`

**Files:**
- Modify: `src/app/ui/camera/grid/frame_processor.h`
- Modify: `src/app/ui/camera/grid/frame_processor.cpp`

**Interfaces:**
- Consumes: `NamedDetection` (from `merge_detections.h`, already used by `DetectionProcessor`).
- Produces: `struct denso::ui::ReadingSink` (pure virtual `on_reading`); `DetectionProcessor` constructor gains trailing defaulted params `int64_t camera_id = 0, ReadingSink* sink = nullptr`.

- [ ] **Step 1: Add the sink interface + constructor params to `frame_processor.h`**

Add `#include <cstdint>` to the header's include block (after `#include <vector>`).

Add the `ReadingSink` struct immediately before `class DetectionProcessor` (after `OrientationProcessor`'s closing brace, before the `///` doc comment for DetectionProcessor):

```cpp
/// Optional per-frame capture hook for the detection pipeline. When a
/// DetectionProcessor has a sink, it calls on_reading() with the frame's kept
/// detections so a consumer can record them (the data-logging feature).
///
/// CONTRACT: on_reading() is invoked on the CAPTURE THREAD, in the hot path. An
/// implementation MUST NOT block or do DB I/O inline — it must hand the data off
/// to a worker (e.g. a queue or common::post_to_gui) and return immediately.
struct ReadingSink {
    virtual ~ReadingSink() = default;
    virtual void on_reading(int64_t camera_id, int64_t ts_ms,
                            const std::vector<NamedDetection>& kept) = 0;
};
```

`NamedDetection` is declared in `merge_detections.h`; add its include near the top of `frame_processor.h` (after the existing `#include "ui/camera/shared/detection/inference_engine.h"`):
```cpp
#include "ui/camera/shared/detection/merge_detections.h"  // NamedDetection
```

Change the `DetectionProcessor` constructor declaration to add the two trailing defaulted params:
```cpp
    DetectionProcessor(int degrees, double pitch, double roll,
                       std::vector<ModelRun> models,
                       std::vector<denso::camera::CameraArea> areas = {},
                       int64_t camera_id = 0, ReadingSink* sink = nullptr);
```

Add the two members to the `private:` section (after `areas_`):
```cpp
    int64_t camera_id_ = 0;
    ReadingSink* sink_ = nullptr;  // non-owning; null = no reading capture
```

- [ ] **Step 2: Wire the constructor + dormant call in `frame_processor.cpp`**

Update the `DetectionProcessor` constructor definition (lines 23–27) to initialize the new members:
```cpp
DetectionProcessor::DetectionProcessor(int degrees, double pitch, double roll,
                                       std::vector<ModelRun> models,
                                       std::vector<denso::camera::CameraArea> areas,
                                       int64_t camera_id, ReadingSink* sink)
    : degrees_(degrees), pitch_(pitch), roll_(roll),
      models_(std::move(models)), areas_(std::move(areas)),
      camera_id_(camera_id), sink_(sink) {}
```

Add `#include <chrono>` to the includes block (after `#include <algorithm>`).

In `process()`, immediately after `const std::vector<NamedDetection> kept = merge_detections(std::move(pool), kMergeIoU);` (line 77) and before the draw loop, add the dormant hook:
```cpp
    // Reading-capture seam (dormant until a sink is wired by the logging
    // feature). Timestamp is only computed when a sink exists, so a camera with
    // no sink pays nothing.
    if (sink_) {
        const int64_t ts_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  std::chrono::system_clock::now().time_since_epoch())
                                  .count();
        sink_->on_reading(camera_id_, ts_ms, kept);
    }
```

- [ ] **Step 3: Build + run the test suite — verify no behavior change**

Run:
```bash
export PATH=/c/msys64/ucrt64/bin:$PATH
cmake --build build
ctest --test-dir build --output-on-failure
```
Expected: build succeeds; all tests still pass (138, unchanged by this task). `camera_grid` constructs `DetectionProcessor` with the pre-existing 5-argument call, which now binds `camera_id=0, sink=nullptr` via the defaults — so `sink_` is null and the new branch is never taken. No runtime change.

- [ ] **Step 4: Commit**

```bash
git add src/app/ui/camera/grid/frame_processor.h src/app/ui/camera/grid/frame_processor.cpp
git commit -m "feat(detection): dormant ReadingSink seam in DetectionProcessor

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Task 6: `page_util` dedup, `.clang-format`, and docs

**Files:**
- Modify: `src/app/ui/camera/dialog/page_util.h`
- Modify: `src/app/ui/camera/dialog/page_util.cpp`
- Create: `.clang-format`
- Modify: `CLAUDE.md`
- Modify: `docs/ARCHITECTURE.md`

- [ ] **Step 1: Point `page_util::dim_label` at the common one**

First inspect the current `page_util` to see what it exports:
```bash
cat src/app/ui/camera/dialog/page_util.h src/app/ui/camera/dialog/page_util.cpp
```

In `page_util.cpp`, replace the body of its `dim_label` implementation with a delegation to the common helper, and add `#include "ui/common/form_widgets.h"` to its includes:
```cpp
QLabel* dim_label(const QString& text) {
    return common::dim_label(text);
}
```
Leave `page_util`'s other members (e.g. the error-colour helper) unchanged. This removes the second definition of the same widget factory without churning every caller. (If `page_util.h` declares `dim_label` in `denso::ui`, keep that declaration so existing page callers compile untouched.)

- [ ] **Step 2: Build — verify the dedup compiles**

Run:
```bash
export PATH=/c/msys64/ucrt64/bin:$PATH
cmake --build build
```
Expected: build succeeds; the pages render their dim labels exactly as before.

- [ ] **Step 3: Add `.clang-format`**

Create `.clang-format` at the repo root, matching the existing style (GCC/MSYS2, 4-space indent, ~100 col, attach braces, pointer-left):

```yaml
# Style for Denso-DigitalReader. Matches the existing hand-written style; does
# not reformat existing files unless run explicitly. Run on new/changed files:
#   clang-format -i <file>
---
Language: Cpp
BasedOnStyle: Google
IndentWidth: 4
ColumnLimit: 100
DerivePointerAlignment: false
PointerAlignment: Left
AccessModifierOffset: -4
AllowShortFunctionsOnASingleLine: Empty
AllowShortIfStatementsOnASingleLine: WithoutElse
SortIncludes: false
```

- [ ] **Step 4: Verify `.clang-format` is clean on the new files**

Run (clang-format ships with the MSYS2 clang tooling; if absent, `pacman -S mingw-w64-ucrt-x86_64-clang-tools-extra`):
```bash
export PATH=/c/msys64/ucrt64/bin:$PATH
clang-format --dry-run --Werror \
    src/core/reading/repo.cpp \
    src/app/ui/common/form_widgets.cpp \
    src/app/ui/common/dialog_chrome.cpp \
    src/app/ui/common/async_runner.cpp \
    src/app/ui/settings/network_panel.cpp \
    src/app/ui/camera/wizard_controller.cpp
```
Expected: no diffs reported (or only trivial ones you then apply with `-i` and re-verify). Do **not** run clang-format across the whole tree — the config is for new/changed files only.

- [ ] **Step 5: Update `CLAUDE.md`**

In the `### src/core/` table, add a row after the `detection/` row:
```markdown
| `reading/` | Append-only detection-reading log: `reading.h` (`Reading`: camera_id + ts_ms + value + conf) + `repo` (`insert`/`query` by camera + time range). Consumed by the logging/export feature; written by the app-side reading sink. Migration **v9**. |
```

In the `### src/app/` intro, add `ui/common/` to the grouping sentence, and add a row to the `src/app/` table (before the `ui/theme` row):
```markdown
| `ui/common/` | **Leaf** shared dialog primitives (Qt-only, no feature deps): `dialog_chrome` (`dialog_header`), `async_runner` (`run_on_worker`/`post_to_gui`), `form_widgets` (`eyebrow`/`dim_label`/`spec_row`/`hline`). Both dialogs build on these instead of re-copying chrome. |
```

Update the `settings/settings_dialog` and `camera/camera_dialog` rows to note the split:
- settings row → append: "The Network page is a self-contained `ui/settings/network_panel` (`NetworkPanel`) owning its cards + threaded apply/scan/connect/refresh; the dialog is a thin view + nav."
- camera row → append: "Flow-state, threaded snapshot capture, and all DB writes live in `ui/camera/wizard_controller` (`CameraWizardController`); the dialog owns only the page stack, stepper, and sizing."

In `## Hard rules`, add:
```markdown
- Dialogs are a **thin view + a controller/panel** that owns flow-state, async
  work, and persistence. Shared dialog chrome (header, async runner, label
  factories) lives in `ui/common/` and is never re-copied into a feature. The
  detection pipeline's `ReadingSink` hook must hand off to a worker — never do
  DB I/O on the capture thread.
```

- [ ] **Step 6: Update `docs/ARCHITECTURE.md`**

- Under `## Project layout`, note the new `ui/common/` leaf and the `core/reading/` module.
- In the GUI section, add a **`ui/common/`** paragraph describing the three primitives as a leaf with no feature dependencies.
- Update the `settings_dialog` bullet to describe the `NetworkPanel` extraction, and the `camera_dialog` bullet to describe the `CameraWizardController` split (view = widgets/stepper/sizing; controller = state/snapshot/persistence; wired via an injected `show_page` callback + `request_show_list` signal).
- Add a **Reading log** subsection under the persistence/detection area: the `core/reading` module (migration v9, `reading` table indexed on `(camera_id, ts_ms)`), and the dormant `ReadingSink` seam in `DetectionProcessor` — documenting the capture-thread threading contract (the sink must marshal off the hot path) and that assembly of detections into a reading `value` is deferred to the logging/export feature (Spec 2).

- [ ] **Step 7: Final full build + test gate**

Run the complete cycle from clean to be sure everything is coherent:
```bash
export PATH=/c/msys64/ucrt64/bin:$PATH
rm -rf build
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```
Expected: clean configure, build with no warnings, all tests pass (138).

- [ ] **Step 8: Commit**

```bash
git add src/app/ui/camera/dialog/page_util.h src/app/ui/camera/dialog/page_util.cpp \
        .clang-format CLAUDE.md docs/ARCHITECTURE.md
git commit -m "chore: dedup page_util, add .clang-format, document restructure + seam

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Self-Review

**Spec coverage:**
- §1 `ui/common/` → Task 2 (+ adopted in Tasks 3–5, dedup in Task 6). ✓
- §2 SettingsDialog → NetworkPanel → Task 3. ✓
- §3 CameraDialog → CameraWizardController → Task 4. ✓
- §4 reading module + migration v9 + repo + tests → Task 1; dormant DetectionProcessor sink → Task 5. ✓
- §5 helper dedup + `.clang-format` → Tasks 2/6; deferred backlog (CI/UI-tests/clang-tidy) explicitly not scoped. ✓
- §6 CLAUDE.md + ARCHITECTURE.md → Task 6. ✓

**Placeholder scan:** No TBD/TODO/"handle edge cases"/"similar to". Every code step shows full code or an exact, quoted edit. Doc steps (Task 6 §5–6) specify the exact rows/paragraphs to add.

**Type consistency:**
- `Reading` fields (`id`/`camera_id`/`ts_ms`/`value`/`conf`) identical across `reading.h`, `repo.cpp`, tests, and the migration columns. ✓
- `reading::insert`/`query` signatures identical in `repo.h`, `repo.cpp`, and the test `using` declarations. ✓
- `common::` helper names (`eyebrow`/`dim_label`/`spec_row`/`hline`/`dialog_header`/`run_on_worker`/`post_to_gui`) consistent between Task 2 definitions and their Task 3/4/6 call sites. ✓
- `CameraWizardController::Pages{configure, models, areas}` order matches the brace-init in `camera_dialog.cpp`. ✓
- `DetectionProcessor` new trailing params `(… , int64_t camera_id = 0, ReadingSink* sink = nullptr)` are defaulted, so the existing `camera_grid` 5-arg call still binds. ✓
- `ReadingSink::on_reading(int64_t, int64_t, const std::vector<NamedDetection>&)` signature identical in header decl and the `process()` call. ✓

No gaps found.

---

## Execution Handoff

Plan complete and saved to `docs/superpowers/plans/2026-07-03-dialog-restructure-logging-seam.md`.
