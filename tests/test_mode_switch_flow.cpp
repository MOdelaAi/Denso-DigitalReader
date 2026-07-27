// Slice 7 / Part B — MainWindow switch-and-reset orchestration.
//
// Drives the REAL MainWindow offscreen over a seeded in-memory DB and proves the
// exact lifecycle the spec requires (§6.1, §6.6, §6.7, §12.10, §12.17, §12.18):
//
//   confirm → busy → teardown-ONLY → atomic reset
//     → on commit:   update in-memory mode, gate the Camera button, rebuild
//     → on rollback: RE-READ the mode from the DB, gate, rebuild the OLD pipeline
//
// Tests call perform_switch() — the dialog-free lifecycle half — so no modal
// automation is needed; the dialog half (on_switch_mode) is exercised only on the
// paths that must abort BEFORE any dialog is shown (bad counts / same mode / busy
// / display transaction), which is exactly where the refusal logic lives.
//
// The two load-bearing proofs are boundary comparisons, not before/after pairs:
//   • SwitchEvent ordering is asserted as an exact sequence.
//   • CameraStream::constructed_count() is sampled AT each lifecycle boundary via
//     the switch observer, so "no stream in the transaction window" holds even for
//     a mode that legitimately builds streams after the reload.
//
// Runs in denso_integration_tests: the single offscreen QApplication comes from
// integration_main.cpp. Every seeded camera is MODEL-LESS (no attached models), so
// CameraGrid selects OrientationProcessor and never asks the EngineRegistry for an
// engine — no ORT/TensorRT engine is deserialized or built, no GPU inference runs.
#include <catch2/catch_test_macros.hpp>

#include "brazing/brazing_retry_policy.h"
#include "brazing/config.h"
#include "camera/camera.h"
#include "camera/camera_stream.h"  // CameraStream::constructed_count()
#include "camera/repo.h"
#include "db/db.h"
#include "detection/engine_registry.h"
#include "mode/config.h"
#include "mode/mode.h"
#include "paths/paths.h"
#include "settings/display.h"
#include "settings/settings.h"
#include "ui/camera/camera_dialog.h"
#include "ui/mainwindow.h"
#include "ui/warmup_state.h"

#include <QApplication>
#include <QByteArray>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QPushButton>
#include <QSqlError>
#include <QSqlQuery>
#include <QString>
#include <QTemporaryDir>
#include <QTimer>
#include <QWidget>

#include <map>
#include <memory>
#include <set>
#include <optional>
#include <vector>

using denso::mode::TargetMode;
using denso::ui::CameraDialog;
using denso::ui::CameraStream;
using denso::ui::EngineRegistry;
using denso::ui::MainWindow;
using denso::ui::WarmupState;
using E = denso::ui::MainWindow::SwitchEvent;

namespace {

// Sets DENSO_DATA_DIR for the scope and restores the prior state on exit — even if
// a Catch2 assertion aborts the case — so cases sharing this one process cannot
// leak a scratch data dir into each other.
struct ScopedDataDir {
    QByteArray prev_ = qgetenv("DENSO_DATA_DIR");
    bool had_ = qEnvironmentVariableIsSet("DENSO_DATA_DIR");
    explicit ScopedDataDir(const QByteArray& path) { qputenv("DENSO_DATA_DIR", path); }
    ~ScopedDataDir() {
        if (had_) qputenv("DENSO_DATA_DIR", prev_);
        else qunsetenv("DENSO_DATA_DIR");
    }
};

// A model-less IP camera pointed at a CLOSED localhost port, so every capture
// candidate fails fast: the NVDEC GStreamer pipelines cannot build (no nv*
// elements on the dev box) and the FFMPEG fallback hits connection-refused. The
// worker therefore never enters a blocking read — it parks in stop-responsive
// reconnect backoff, so teardown joins promptly.
denso::camera::Camera model_less_cam(const std::string& name, bool active,
                                     bool setup_complete) {
    denso::camera::Camera c;
    c.name = name;
    c.camera_type = "ip";
    c.ip = "127.0.0.1";
    c.rtsp = "rtsp://127.0.0.1:9/none";  // port 9 (discard) — refused instantly
    c.channel = 1;
    c.stream = 0;
    c.username = "admin";
    c.password = "hunter2";
    c.width = 1280;
    c.height = 720;
    c.fps = 25;
    c.pitch = 0.0f;
    c.roll = 0.0f;
    c.rotation = 0;
    c.active = active;
    c.setup_complete = setup_complete;
    c.areas_need_review = false;
    return c;
}

void exec_sql(const QSqlDatabase& db, const QString& sql) {
    QSqlQuery q(db);
    REQUIRE(q.exec(sql));
}

QJsonObject read_status_json() {
    QFile f(denso::paths::status_file());
    if (!f.open(QIODevice::ReadOnly)) return {};
    return QJsonDocument::fromJson(f.readAll()).object();
}

// Builds a real MainWindow over an in-memory migrated DB inside a scratch data
// dir. The env guard is a MEMBER declared right after `data`, so it captures the
// process's true prior DENSO_DATA_DIR and restores it when the Harness — and its
// QTemporaryDir — go away.
//
// The window is NOT shown and apply_startup() is NOT called: nothing here needs a
// realized native window, and skipping it keeps the case free of screen geometry.
struct Harness {
    QTemporaryDir data;
    ScopedDataDir data_guard{data.isValid() ? data.path().toUtf8() : QByteArray()};
    std::optional<denso::db::Db> db;
    std::shared_ptr<denso::settings::Settings> state;
    std::shared_ptr<EngineRegistry> engines;
    std::unique_ptr<WarmupState> warmup;

    Harness() {
        REQUIRE(data.isValid());
        QDir(data.path()).mkpath(QStringLiteral("models"));  // readable, empty
        db = denso::db::Db::open_in_memory();
        REQUIRE(db);
        REQUIRE(denso::db::run_migrations(db->handle()));
        state = std::make_shared<denso::settings::Settings>();
        engines = std::make_shared<EngineRegistry>(
            denso::paths::models_dir().toStdString(),
            denso::paths::trt_cache_dir().toStdString(),
            std::set<std::string>{});  // model-less: empty allow-list
        warmup = std::make_unique<WarmupState>(engines);  // model-less path ignores it
    }

    QSqlDatabase handle() { return db->handle(); }

    // Seed brazing reporting ON, so a switch has something real to disable.
    void enable_reporting() {
        denso::brazing::BrazingConfig b;
        b.enabled = true;
        b.base_url = "http://127.0.0.1:9";
        denso::brazing::save(handle(), b);
        REQUIRE(denso::brazing::load(handle()).enabled);
    }

    std::unique_ptr<MainWindow> make_window() {
        return std::make_unique<MainWindow>(handle(), state, engines, warmup.get());
    }
};

// The top-bar Camera button of a built window.
QPushButton* camera_button(MainWindow& w) {
    return w.findChild<QPushButton*>(QStringLiteral("cameraButton"));
}

// Records (event, CameraStream construction tally) at each lifecycle boundary.
struct EventLog {
    std::vector<E> order;
    std::map<E, uint64_t> streams_at;

    void install(MainWindow& w) {
        w.set_switch_observer([this](E e) {
            order.push_back(e);
            streams_at[e] = CameraStream::constructed_count();
        });
    }
};

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────
// 1. Successful switch
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("a committed switch empties runtime, disables reporting, and gates the wizard",
          "[mode_switch_flow]") {
    Harness h;
    h.enable_reporting();
    const auto cam_id = denso::camera::insert(
        h.handle(), model_less_cam("Line A", /*active*/ true, /*setup*/ true));
    REQUIRE(cam_id);
    REQUIRE(denso::camera::runtime(h.handle()).size() == 1);

    auto win = h.make_window();
    REQUIRE(win->current_mode() == TargetMode::DigitReader);

    const auto now = win->perform_switch(TargetMode::BallLeveler);

    // Return value, in-memory mode and DB truth all agree on the new mode.
    CHECK(now == TargetMode::BallLeveler);
    CHECK(win->current_mode() == TargetMode::BallLeveler);
    CHECK(denso::mode::load(h.handle()) == TargetMode::BallLeveler);

    // The reset emptied runtime() by zeroing setup_complete — the camera ROW
    // survives with its id (decision A1); it is simply no longer admissible.
    CHECK(denso::camera::runtime(h.handle()).empty());
    const auto rows = denso::camera::all(h.handle());
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].id == *cam_id);
    CHECK(rows[0].name == "Line A");
    CHECK_FALSE(rows[0].setup_complete);

    // Reporting is disabled in configuration, and the address is kept (A2).
    const auto bcfg = denso::brazing::load(h.handle());
    CHECK_FALSE(bcfg.enabled);
    CHECK(bcfg.base_url == "http://127.0.0.1:9");

    // status.json names the new mode with setup-required permanently true (§2.1),
    // written by the CameraGrid idle writer — never by MainWindow.
    const QJsonObject st = read_status_json();
    CHECK(st.value(QStringLiteral("mode")).toString() == QStringLiteral("ball_leveler"));
    CHECK(st.value(QStringLiteral("mode_setup_required")).toBool() == true);

    // The view is on the unavailable page and no setup/wizard action is exposed.
    CHECK(win->camera_view_page_index() == 2);
    QPushButton* cam_btn = camera_button(*win);
    REQUIRE(cam_btn != nullptr);
    CHECK_FALSE(cam_btn->isEnabled());          // top-bar Camera gated off

    // …and the handler itself refuses, not merely the button: open_camera() must
    // short-circuit so no code path can reach the wizard in ball_leveler.
    win->open_camera();
    CHECK(win->findChild<CameraDialog*>() == nullptr);
}

// ─────────────────────────────────────────────────────────────────────────────
// 2. Transaction rollback
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("a rolled-back switch keeps the OLD mode in memory and in the DB",
          "[mode_switch_flow]") {
    Harness h;
    h.enable_reporting();
    const auto cam_id = denso::camera::insert(
        h.handle(), model_less_cam("Line A", /*active*/ true, /*setup*/ true));
    REQUIRE(cam_id);
    // Inject a failure INSIDE the reset transaction: the DELETE FROM reading step
    // aborts, so switch_and_reset must roll everything back.
    exec_sql(h.handle(), QStringLiteral(
        "CREATE TRIGGER boom BEFORE DELETE ON reading BEGIN "
        "SELECT RAISE(ABORT,'injected'); END"));
    exec_sql(h.handle(), QStringLiteral(
        "INSERT INTO reading (camera_id,ts_ms,value,conf) VALUES (%1,1,'1',0.5)")
        .arg(*cam_id));

    auto win = h.make_window();
    REQUIRE(win->current_mode() == TargetMode::DigitReader);

    const auto now = win->perform_switch(TargetMode::BallLeveler);

    // The returned mode is the one actually in effect — re-read from the DB, never
    // the optimistic target (spec §6.7, §12.17).
    CHECK(now == TargetMode::DigitReader);
    CHECK(win->current_mode() == TargetMode::DigitReader);
    CHECK(win->current_mode() == denso::mode::load(h.handle()));  // memory == DB truth

    // Everything the transaction would have changed reverted together.
    CHECK(denso::brazing::load(h.handle()).enabled);          // reporting still armed
    const auto rows = denso::camera::all(h.handle());
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].setup_complete);                            // processing flag intact
    CHECK_FALSE(rows[0].areas_need_review);
    CHECK_FALSE(denso::camera::runtime(h.handle()).empty());  // old pipeline admissible

    // The old Digital Reader pipeline was rebuilt, and the wizard stays reachable.
    CHECK(win->camera_view_page_index() == 1);                // live grid
    QPushButton* cam_btn = camera_button(*win);
    REQUIRE(cam_btn != nullptr);
    CHECK(cam_btn->isEnabled());

    // No optimistic BallLeveler state survives anywhere.
    CHECK(denso::mode::load(h.handle()) == TargetMode::DigitReader);
}

TEST_CASE("a rolled-back switch surfaces the verbatim SQL error", "[mode_switch_flow]") {
    Harness h;
    const auto cam_id = denso::camera::insert(
        h.handle(), model_less_cam("Line A", /*active*/ true, /*setup*/ true));
    REQUIRE(cam_id);
    exec_sql(h.handle(), QStringLiteral(
        "CREATE TRIGGER boom BEFORE DELETE ON reading BEGIN "
        "SELECT RAISE(ABORT,'injected-marker'); END"));
    exec_sql(h.handle(), QStringLiteral(
        "INSERT INTO reading (camera_id,ts_ms,value,conf) VALUES (%1,1,'1',0.5)")
        .arg(*cam_id));

    auto win = h.make_window();
    REQUIRE(win->perform_switch(TargetMode::BallLeveler) == TargetMode::DigitReader);

    // The RAISE reason reaches the operator-facing error, not a generic message —
    // otherwise a failed switch is undiagnosable in the field.
    CHECK(win->last_switch_error().contains(QStringLiteral("injected-marker")));
}

// ─────────────────────────────────────────────────────────────────────────────
// 3. No CameraStream is constructed in the transaction window
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("no CameraStream is constructed between teardown start and commit",
          "[mode_switch_flow]") {
    Harness h;
    REQUIRE(denso::camera::insert(
        h.handle(), model_less_cam("Line A", /*active*/ true, /*setup*/ true)));

    const uint64_t before_window = CameraStream::constructed_count();
    auto win = h.make_window();
    // The window's own build DID construct a stream, so the window under test is a
    // live digit_reader pipeline — the comparison below is measuring a real
    // teardown, not a window that never had anything to tear down.
    REQUIRE(CameraStream::constructed_count() > before_window);
    REQUIRE(win->camera_view_page_index() == 1);  // live grid

    EventLog log;
    log.install(*win);

    REQUIRE(win->perform_switch(TargetMode::BallLeveler) == TargetMode::BallLeveler);

    REQUIRE(log.streams_at.count(E::TeardownStarted) == 1);
    REQUIRE(log.streams_at.count(E::TransactionCommitted) == 1);
    // The monotonic construction tally is IDENTICAL at both boundaries, so no
    // CameraStream was built anywhere in the pre-commit window (spec §12.18). A
    // boundary comparison, not a whole-switch before/after: the latter would also
    // pass on a build-then-teardown inside the window.
    CHECK(log.streams_at.at(E::TransactionCommitted) ==
          log.streams_at.at(E::TeardownStarted));
}

TEST_CASE("no CameraStream is constructed between teardown start and rollback",
          "[mode_switch_flow]") {
    Harness h;
    const auto cam_id = denso::camera::insert(
        h.handle(), model_less_cam("Line A", /*active*/ true, /*setup*/ true));
    REQUIRE(cam_id);
    exec_sql(h.handle(), QStringLiteral(
        "CREATE TRIGGER boom BEFORE DELETE ON reading BEGIN "
        "SELECT RAISE(ABORT,'injected'); END"));
    exec_sql(h.handle(), QStringLiteral(
        "INSERT INTO reading (camera_id,ts_ms,value,conf) VALUES (%1,1,'1',0.5)")
        .arg(*cam_id));

    auto win = h.make_window();
    EventLog log;
    log.install(*win);

    REQUIRE(win->perform_switch(TargetMode::BallLeveler) == TargetMode::DigitReader);

    REQUIRE(log.streams_at.count(E::TeardownStarted) == 1);
    REQUIRE(log.streams_at.count(E::TransactionRolledBack) == 1);
    CHECK(log.streams_at.at(E::TransactionRolledBack) ==
          log.streams_at.at(E::TeardownStarted));
    // …and the rebuild that follows DOES construct one, so the assertion above is
    // measuring a real window rather than a switch that never builds anything.
    CHECK(CameraStream::constructed_count() >
          log.streams_at.at(E::TransactionRolledBack));
}

// ─────────────────────────────────────────────────────────────────────────────
// 4. Event ordering
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("a successful switch fires the lifecycle events in exact order",
          "[mode_switch_flow]") {
    Harness h;
    REQUIRE(denso::camera::insert(
        h.handle(), model_less_cam("Line A", /*active*/ true, /*setup*/ true)));

    auto win = h.make_window();
    EventLog log;
    log.install(*win);

    REQUIRE(win->perform_switch(TargetMode::BallLeveler) == TargetMode::BallLeveler);

    CHECK(log.order == std::vector<E>{E::TeardownStarted, E::TeardownCompleted,
                                      E::TransactionStarted, E::TransactionCommitted,
                                      E::ReloadStarted});
}

TEST_CASE("a rolled-back switch fires the lifecycle events in exact order",
          "[mode_switch_flow]") {
    Harness h;
    const auto cam_id = denso::camera::insert(
        h.handle(), model_less_cam("Line A", /*active*/ true, /*setup*/ true));
    REQUIRE(cam_id);
    exec_sql(h.handle(), QStringLiteral(
        "CREATE TRIGGER boom BEFORE DELETE ON reading BEGIN "
        "SELECT RAISE(ABORT,'injected'); END"));
    exec_sql(h.handle(), QStringLiteral(
        "INSERT INTO reading (camera_id,ts_ms,value,conf) VALUES (%1,1,'1',0.5)")
        .arg(*cam_id));

    auto win = h.make_window();
    EventLog log;
    log.install(*win);

    REQUIRE(win->perform_switch(TargetMode::BallLeveler) == TargetMode::DigitReader);

    CHECK(log.order == std::vector<E>{E::TeardownStarted, E::TeardownCompleted,
                                      E::TransactionStarted, E::TransactionRolledBack,
                                      E::ReloadStarted});
}

// ─────────────────────────────────────────────────────────────────────────────
// 5. Preview-count failure aborts before confirmation AND before teardown
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("a count-query failure aborts with no confirmation and no teardown",
          "[mode_switch_flow]") {
    Harness h;
    REQUIRE(denso::camera::insert(
        h.handle(), model_less_cam("Line A", /*active*/ true, /*setup*/ true)));

    auto win = h.make_window();
    EventLog log;
    log.install(*win);
    const uint64_t streams_before = CameraStream::constructed_count();

    // Break a count query. preview_counts() then returns nullopt, and A3 forbids
    // confirming on fabricated zeros — the operator must never authorise deleting
    // data they were told was empty.
    exec_sql(h.handle(), QStringLiteral("DROP TABLE reading"));

    // The UI handler runs its REAL decision logic. It aborts before ever
    // constructing ModeConfirmDialog, which is why this does not block on a modal.
    win->on_switch_mode(static_cast<int>(TargetMode::BallLeveler));

    CHECK(log.order.empty());                   // no teardown, no transaction event
    CHECK(CameraStream::constructed_count() == streams_before);  // pipeline untouched
    CHECK(win->current_mode() == TargetMode::DigitReader);
    CHECK(denso::mode::load(h.handle()) == TargetMode::DigitReader);
    CHECK(win->camera_view_page_index() == 1);  // still the live grid
    CHECK_FALSE(denso::camera::runtime(h.handle()).empty());
    // The operator is told something went wrong, without a fabricated count.
    CHECK_FALSE(win->last_switch_error().isEmpty());
    CHECK_FALSE(win->last_switch_error().contains(QStringLiteral("0 ")));
}

TEST_CASE("the connection carries no statement error, which is why the preview message is generic",
          "[mode_switch_flow]") {
    // Pins the reason on_switch_mode's preview-failure message cannot name the
    // failing statement, so the limitation is recorded as a fact about Qt rather
    // than an unexplained gap in the copy.
    //
    // Qt keeps a STATEMENT error on the QSqlResult (QSqlQuery::lastError), and only
    // driver-level errors (open/close/transaction) on the connection
    // (QSqlDatabase::lastError). preview_counts() runs local QSqlQuery objects and
    // discards them, so MainWindow cannot recover the text from the connection —
    // reading it there would surface nothing, or a stale unrelated error. Carrying
    // it properly requires mode::preview_counts() to return it (src/core/mode/reset.h).
    Harness h;
    exec_sql(h.handle(), QStringLiteral("DROP TABLE reading"));

    QSqlQuery q(h.handle());
    REQUIRE_FALSE(q.exec(QStringLiteral("SELECT COUNT(*) FROM reading")));
    // The QUERY has the real text…
    CHECK(q.lastError().text().contains(QStringLiteral("no such table"),
                                        Qt::CaseInsensitive));
    // …and the CONNECTION does not, so db_.lastError() is not a usable substitute.
    CHECK_FALSE(h.handle().lastError().text().contains(QStringLiteral("no such table"),
                                                       Qt::CaseInsensitive));
}

// ─────────────────────────────────────────────────────────────────────────────
// 6. Same-mode request refusal
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("a request for the already-active mode does nothing at all",
          "[mode_switch_flow]") {
    Harness h;
    h.enable_reporting();
    const auto cam_id = denso::camera::insert(
        h.handle(), model_less_cam("Line A", /*active*/ true, /*setup*/ true));
    REQUIRE(cam_id);

    auto win = h.make_window();
    EventLog log;
    log.install(*win);
    const uint64_t streams_before = CameraStream::constructed_count();

    win->on_switch_mode(static_cast<int>(TargetMode::DigitReader));  // == current

    CHECK(log.order.empty());                   // no preview, no confirm, no teardown
    CHECK(CameraStream::constructed_count() == streams_before);
    CHECK(win->current_mode() == TargetMode::DigitReader);
    // No DB change whatsoever: the workspace the reset would have destroyed is
    // fully intact.
    CHECK(denso::mode::load(h.handle()) == TargetMode::DigitReader);
    CHECK(denso::brazing::load(h.handle()).enabled);
    CHECK_FALSE(denso::camera::runtime(h.handle()).empty());
    CHECK(denso::camera::all(h.handle())[0].setup_complete);
}

TEST_CASE("an out-of-range selector index is validated, not stored raw",
          "[mode_switch_flow]") {
    Harness h;
    auto win = h.make_window();
    EventLog log;
    log.install(*win);

    // from_index() maps any unknown int to DigitReader — never an invalid enum —
    // so a garbage index resolves to the ACTIVE mode and is refused as same-mode.
    win->on_switch_mode(99);

    CHECK(log.order.empty());
    CHECK(win->current_mode() == TargetMode::DigitReader);
    CHECK(denso::mode::load(h.handle()) == TargetMode::DigitReader);
}

// ─────────────────────────────────────────────────────────────────────────────
// 7. Busy-state refusal (deterministic re-entrancy, no timing sleeps)
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("a second switch request during an active switch does nothing",
          "[mode_switch_flow]") {
    Harness h;
    REQUIRE(denso::camera::insert(
        h.handle(), model_less_cam("Line A", /*active*/ true, /*setup*/ true)));

    auto win = h.make_window();

    // Re-enter the UI handler from INSIDE the switch, at the earliest lifecycle
    // boundary — the real hazard the busy flag exists for (a second click landing
    // during the multi-second synchronous teardown join). Deterministic: driven by
    // the observer, not by a sleep.
    //
    // The re-entrant target MUST be BallLeveler, not DigitReader: current_mode_ is
    // still DigitReader at this point (it only moves after commit), so a
    // DigitReader request would be refused by the SAME-MODE guard and the case
    // would pass with the busy guard deleted entirely. BallLeveler is not the
    // current mode, so only the busy guard can refuse it.
    int reentrant_calls = 0;
    std::vector<E> order;
    MainWindow* raw = win.get();
    raw->set_switch_observer([&, raw](E e) {
        order.push_back(e);
        if (e == E::TeardownStarted && reentrant_calls == 0) {
            ++reentrant_calls;
            REQUIRE(raw->current_mode() == TargetMode::DigitReader);  // not yet moved
            raw->on_switch_mode(static_cast<int>(TargetMode::BallLeveler));
        }
    });

    REQUIRE(win->perform_switch(TargetMode::BallLeveler) == TargetMode::BallLeveler);
    REQUIRE(reentrant_calls == 1);  // the re-entrant attempt really was made

    // It did nothing: the event stream is exactly ONE switch, with no second
    // teardown/transaction spliced into it.
    CHECK(order == std::vector<E>{E::TeardownStarted, E::TeardownCompleted,
                                  E::TransactionStarted, E::TransactionCommitted,
                                  E::ReloadStarted});
    CHECK(win->current_mode() == TargetMode::BallLeveler);
    CHECK(denso::mode::load(h.handle()) == TargetMode::BallLeveler);
}

TEST_CASE("a re-entrant perform_switch is refused and cannot clear the busy flag",
          "[mode_switch_flow]") {
    // perform_switch is public (it is the dialog-free seam), and the lifecycle is
    // synchronous — so exclusion has to be enforced by the lifecycle itself. A
    // guard that merely SET the flag and cleared it in RAII would let this nested
    // call clear it on the way out, re-admitting switches while the outer one is
    // still mid-flight.
    Harness h;
    REQUIRE(denso::camera::insert(
        h.handle(), model_less_cam("Line A", /*active*/ true, /*setup*/ true)));

    auto win = h.make_window();

    int nested_calls = 0;
    std::optional<TargetMode> nested_result;
    std::vector<E> order;
    MainWindow* raw = win.get();
    raw->set_switch_observer([&, raw](E e) {
        order.push_back(e);
        if (e == E::TeardownStarted && nested_calls == 0) {
            ++nested_calls;
            // A DIFFERENT target from the outer switch, and different from the
            // still-current mode — so nothing but the busy guard can refuse it.
            nested_result = raw->perform_switch(TargetMode::BallLeveler);
        }
    });

    REQUIRE(win->perform_switch(TargetMode::BallLeveler) == TargetMode::BallLeveler);
    REQUIRE(nested_calls == 1);
    // The nested call returned the mode still in effect and performed no lifecycle.
    REQUIRE(nested_result.has_value());
    CHECK(*nested_result == TargetMode::DigitReader);
    // Exactly ONE lifecycle ran: no second teardown/transaction is spliced in.
    CHECK(order == std::vector<E>{E::TeardownStarted, E::TeardownCompleted,
                                  E::TransactionStarted, E::TransactionCommitted,
                                  E::ReloadStarted});
    CHECK(denso::mode::load(h.handle()) == TargetMode::BallLeveler);
}

// ─────────────────────────────────────────────────────────────────────────────
// 8. Display-transaction refusal
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("a switch is refused while a display confirm/revert transaction is live",
          "[mode_switch_flow]") {
    Harness h;
    REQUIRE(denso::camera::insert(
        h.handle(), model_less_cam("Line A", /*active*/ true, /*setup*/ true)));

    auto win = h.make_window();
    EventLog log;
    log.install(*win);
    const uint64_t streams_before = CameraStream::constructed_count();

    // Use the REAL display-transaction state, not a fabricated flag setter:
    // on_apply_display() arms display_txn_active_ and defers the rest to the next
    // event-loop tick. We never spin the loop, so the transaction stays live for
    // the duration of this case (and the deferred lambda is dropped when the
    // window — its Qt context — is destroyed at scope exit).
    win->on_apply_display(static_cast<int>(denso::settings::DisplayMode::Fullscreen),
                          static_cast<int>(h.state->width),
                          static_cast<int>(h.state->height));

    win->on_switch_mode(static_cast<int>(TargetMode::BallLeveler));

    CHECK(log.order.empty());                   // refused before anything happened
    CHECK(CameraStream::constructed_count() == streams_before);
    CHECK(win->current_mode() == TargetMode::DigitReader);
    CHECK(denso::mode::load(h.handle()) == TargetMode::DigitReader);
    CHECK_FALSE(denso::camera::runtime(h.handle()).empty());
}

TEST_CASE("the identical request proceeds when no display transaction is active",
          "[mode_switch_flow]") {
    // Negative control for the case above. Without it, that case would also pass if
    // on_switch_mode() were an unconditional no-op — it proves the refusal is caused
    // by display_txn_active_, not by the handler simply never doing anything.
    Harness h;
    REQUIRE(denso::camera::insert(
        h.handle(), model_less_cam("Line A", /*active*/ true, /*setup*/ true)));

    auto win = h.make_window();
    EventLog log;
    log.install(*win);

    // Same window, same request, same seeded DB — the ONLY difference is that
    // on_apply_display() was never called, so no display transaction is live.
    // (perform_switch is used here because on_switch_mode would open the real modal
    // confirmation; the refusal under test happens before that dialog either way.)
    REQUIRE(win->perform_switch(TargetMode::BallLeveler) == TargetMode::BallLeveler);

    CHECK_FALSE(log.order.empty());  // the lifecycle DID run this time
    CHECK(log.order.front() == E::TeardownStarted);
    CHECK(denso::mode::load(h.handle()) == TargetMode::BallLeveler);
}

// ─────────────────────────────────────────────────────────────────────────────
// 9. Boot already in Ball Leveler
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("a window booted in ball_leveler is gated from the start",
          "[mode_switch_flow]") {
    Harness h;
    REQUIRE(denso::mode::save(h.handle(), TargetMode::BallLeveler));
    // An out-of-flow DB: this camera is completed AND active, so runtime() alone
    // would admit it. The boot-time mode gate must still refuse to build anything.
    REQUIRE(denso::camera::insert(
        h.handle(), model_less_cam("Rogue Cam", /*active*/ true, /*setup*/ true)));
    REQUIRE(denso::camera::runtime(h.handle()).size() == 1);

    const uint64_t streams_before = CameraStream::constructed_count();
    auto win = h.make_window();

    // The committed mode is adopted at construction — never assumed DigitReader.
    CHECK(win->current_mode() == TargetMode::BallLeveler);
    CHECK(win->camera_view_page_index() == 2);              // unavailable page
    CHECK(win->camera_view_grid_reload_invocations() == 0); // grid build never entered
    CHECK(CameraStream::constructed_count() == streams_before);

    QPushButton* cam_btn = camera_button(*win);
    REQUIRE(cam_btn != nullptr);
    CHECK_FALSE(cam_btn->isEnabled());
    // The handler refuses too, so no path reaches the wizard.
    win->open_camera();
    CHECK(win->findChild<CameraDialog*>() == nullptr);
}

TEST_CASE("a window booted in digit_reader keeps the Camera button usable",
          "[mode_switch_flow]") {
    // The negative control for the gate: without it, a gate that disabled the
    // button unconditionally would pass every ball_leveler assertion above.
    Harness h;
    REQUIRE(denso::camera::insert(
        h.handle(), model_less_cam("Line A", /*active*/ true, /*setup*/ true)));

    auto win = h.make_window();

    CHECK(win->current_mode() == TargetMode::DigitReader);
    QPushButton* cam_btn = camera_button(*win);
    REQUIRE(cam_btn != nullptr);
    CHECK(cam_btn->isEnabled());
    win->open_camera();
    CHECK(win->findChild<CameraDialog*>() != nullptr);  // the wizard DOES open
}

// ─────────────────────────────────────────────────────────────────────────────
// 10. Reporting isolation
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("after a switch no reporter is rebuilt and reporting stays off",
          "[mode_switch_flow]") {
    Harness h;
    h.enable_reporting();
    REQUIRE(denso::camera::insert(
        h.handle(), model_less_cam("Line A", /*active*/ true, /*setup*/ true)));

    auto win = h.make_window();
    const uint64_t reloads_before = win->camera_view_grid_reload_invocations();

    REQUIRE(win->perform_switch(TargetMode::BallLeveler) == TargetMode::BallLeveler);

    // Written inside the reset transaction (A2), so reporting cannot silently
    // resume when the destination mode is later configured.
    CHECK_FALSE(denso::brazing::load(h.handle()).enabled);

    // No reporter was rebuilt: CameraGrid constructs the ZoneReporter and
    // BrazingReporter ONLY inside reload()'s build path, and ball_leveler never
    // enters it — so an unchanged invocation counter covers all three object kinds
    // (reporter, ZoneHealth, every processor/stream) with one observable.
    CHECK(win->camera_view_grid_reload_invocations() == reloads_before);

    // Nothing re-enables reporting on its own; that is an explicit operator action.
    CHECK_FALSE(denso::brazing::load(h.handle()).enabled);
}

// ─────────────────────────────────────────────────────────────────────────────
// End-to-end through the REAL confirmation dialog
//
// Every case above drives perform_switch(), which deliberately has no dialog.
// These two drive on_switch_mode() with counts that succeed, so the REAL
// ModeConfirmDialog is constructed and exec()'d, and press its real buttons —
// covering the operator's actual path: real counts rendered, Cancel changes
// nothing, confirm proceeds.
// ─────────────────────────────────────────────────────────────────────────────

namespace {

// Presses a named button on whatever modal dialog is up, from inside that dialog's
// own event loop. Armed BEFORE the blocking exec() call, so it fires once the loop
// starts.
//
// The timer is a MEMBER, not QTimer::singleShot(0, qApp, ...): a static single-shot
// bound to qApp outlives the test case, so if the dialog never appeared it would
// fire during some LATER case with dangling pointers into this one's stack. Owning
// the timer means scope exit cancels any pending fire.
//
// If the dialog is up but the button is missing, it closes the dialog rather than
// returning — otherwise exec() would never return and the case would hang instead
// of failing. `clicked` stays false, so REQUIRE(clicked) reports the real problem.
class ModalClicker {
public:
    explicit ModalClicker(QString button_text) : button_text_(std::move(button_text)) {
        timer_.setSingleShot(true);
        QObject::connect(&timer_, &QTimer::timeout, [this] { fire(); });
        timer_.start(0);
    }

    QString body;         // the dialog's rendered body text, as the operator saw it
    bool clicked = false; // the named button really was found and pressed

private:
    void fire() {
        QWidget* modal = QApplication::activeModalWidget();
        if (!modal) {
            timer_.start(0);  // not up yet — look again on the next turn
            return;
        }
        if (auto* label = modal->findChild<QLabel*>(QStringLiteral("modeConfirmBody"))) {
            body = label->text();
        }
        for (QPushButton* b : modal->findChildren<QPushButton*>()) {
            if (b->text() == button_text_) {
                clicked = true;
                b->click();
                return;
            }
        }
        modal->close();  // never leave exec() spinning forever
    }

    QTimer timer_;
    QString button_text_;
};

}  // namespace

TEST_CASE("the confirmation shows real counts and Cancel changes nothing",
          "[mode_switch_flow]") {
    Harness h;
    h.enable_reporting();
    const auto cam_id = denso::camera::insert(
        h.handle(), model_less_cam("Line A", /*active*/ true, /*setup*/ true));
    REQUIRE(cam_id);
    // A realistic mode-owned workspace, so the dialog has something real to count.
    for (int zone : {3, 4, 5, 7}) {
        exec_sql(h.handle(),
                 QStringLiteral("INSERT INTO camera_area(camera_id,name,points,zone) "
                                "VALUES(%1,'a%2','[[0.1,0.1],[0.9,0.1],[0.9,0.9]]',%2)")
                     .arg(*cam_id)
                     .arg(zone));
    }
    exec_sql(h.handle(), QStringLiteral(
        "INSERT INTO reading(camera_id,ts_ms,value,conf) VALUES(%1,1,'1234',0.9)")
        .arg(*cam_id));

    auto win = h.make_window();
    EventLog log;
    log.install(*win);

    ModalClicker clicker{QStringLiteral("Cancel")};
    win->on_switch_mode(static_cast<int>(TargetMode::BallLeveler));

    REQUIRE(clicker.clicked);  // the real dialog was shown and really was dismissed
    // It rendered REAL counts read from this DB — not zeros, not placeholders.
    CHECK(clicker.body.contains(QStringLiteral("1 camera connection")));
    CHECK(clicker.body.contains(QStringLiteral("4 detection areas")));
    CHECK(clicker.body.contains(QStringLiteral("3, 4, 5, 7")));
    CHECK(clicker.body.contains(QStringLiteral("reporting will be turned off")));
    CHECK(clicker.body.contains(QStringLiteral("not available in this release")));
    // …and it never claims the cameras themselves are deleted.
    CHECK_FALSE(clicker.body.contains(QStringLiteral("cameras will be deleted")));

    // Cancel performed NO lifecycle step and changed nothing at all.
    CHECK(log.order.empty());
    CHECK(win->current_mode() == TargetMode::DigitReader);
    CHECK(denso::mode::load(h.handle()) == TargetMode::DigitReader);
    CHECK(denso::brazing::load(h.handle()).enabled);
    CHECK_FALSE(denso::camera::runtime(h.handle()).empty());
    CHECK(denso::camera::all(h.handle())[0].setup_complete);
}

TEST_CASE("confirming the dialog runs the full switch", "[mode_switch_flow]") {
    Harness h;
    h.enable_reporting();
    const auto cam_id = denso::camera::insert(
        h.handle(), model_less_cam("Line A", /*active*/ true, /*setup*/ true));
    REQUIRE(cam_id);
    exec_sql(h.handle(),
             QStringLiteral("INSERT INTO camera_area(camera_id,name,points,zone) "
                            "VALUES(%1,'a3','[[0.1,0.1],[0.9,0.1],[0.9,0.9]]',3)")
                 .arg(*cam_id));

    auto win = h.make_window();
    EventLog log;
    log.install(*win);

    ModalClicker clicker{QStringLiteral("Switch and Reset")};
    win->on_switch_mode(static_cast<int>(TargetMode::BallLeveler));

    REQUIRE(clicker.clicked);
    // The whole lifecycle ran, in order, from the operator's own entry point.
    CHECK(log.order == std::vector<E>{E::TeardownStarted, E::TeardownCompleted,
                                      E::TransactionStarted, E::TransactionCommitted,
                                      E::ReloadStarted});
    CHECK(win->current_mode() == TargetMode::BallLeveler);
    CHECK(denso::mode::load(h.handle()) == TargetMode::BallLeveler);
    CHECK(denso::camera::runtime(h.handle()).empty());
    CHECK_FALSE(denso::brazing::load(h.handle()).enabled);
    CHECK(denso::camera::all(h.handle()).size() == 1);  // camera KEPT, not deleted
    CHECK(win->camera_view_page_index() == 2);
    QPushButton* cam_btn = camera_button(*win);
    REQUIRE(cam_btn != nullptr);
    CHECK_FALSE(cam_btn->isEnabled());
}

TEST_CASE("no stale delivered_ can suppress the next mode's first snapshot",
          "[mode_switch_flow]") {
    // Retry state is in-memory only and dies with the reporter (spec §12.10). The
    // grid builds a FRESH BrazingRetryPolicy per reload, and a fresh policy starts
    // with an empty delivered_, so its first submit always sends — a snapshot from
    // the old mode can never mark the new mode's first reading as already
    // delivered. Asserted directly on the policy, which is where the state lives.
    denso::ui::BrazingRetryPolicy fresh;
    const auto a = fresh.submit(std::map<int, int>{{3, 120}});
    CHECK(a.kind == denso::ui::RetryAction::Kind::Send);
    CHECK(a.snapshot == std::map<int, int>{{3, 120}});
}
