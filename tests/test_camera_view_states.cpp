// Slice 6 — Retained-camera setup-required UI (three CameraView states).
//
// Proves CameraView's mode-gated state selection (spec §7.2, §3.5, §12.14):
//   • digit_reader, all() empty            → page 0 ("No cameras yet" + Add)
//   • digit_reader, runtime() non-empty    → page 1 (live grid)
//   • digit_reader, retained/setup-required→ page 2 + "Set up cameras"
//   • ball_leveler (ANY camera state)      → page 2 unavailable, NO pipeline
//
// Mode is a HARD admission gate: even an out-of-flow DB carrying
// mode.target=ball_leveler AND a completed+active camera must NOT start the
// digit-reader grid. We prove "no pipeline built" with two monotonic observables
// captured at the precise boundary right before the action:
//   • CameraGrid::reload_invocations() — the grid's build path (sole constructor
//     of the reporter, ZoneHealth, every DetectionProcessor and CameraStream) is
//     never entered, so its counter is unchanged.
//   • CameraStream::constructed_count() — corroboration (process-lifetime tally).
//
// Runs in denso_integration_tests: the single offscreen QApplication is provided
// by integration_main.cpp. Every seeded camera is MODEL-LESS (no attached
// models), so CameraGrid selects OrientationProcessor and never asks the
// EngineRegistry for an engine — no ORT/TensorRT engine is loaded, no GPU runs.
#include <catch2/catch_test_macros.hpp>

#include "camera/camera.h"
#include "camera/camera_stream.h"  // CameraStream::constructed_count()
#include "camera/repo.h"
#include "db/db.h"
#include "detection/engine_registry.h"
#include "mode/config.h"
#include "mode/mode.h"
#include "paths/paths.h"
#include "ui/camera/camera_view.h"
#include "ui/warmup_state.h"

#include <QByteArray>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QPushButton>
#include <QTemporaryDir>

#include <algorithm>
#include <memory>
#include <set>
#include <optional>

using denso::mode::TargetMode;
using denso::ui::CameraStream;
using denso::ui::CameraView;
using denso::ui::EngineRegistry;
using denso::ui::WarmupState;

namespace {

// Sets DENSO_DATA_DIR for the scope and restores the prior state on exit — even
// if a Catch2 assertion aborts the test — so cases sharing this one process can't
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

// A model-less camera. IP source at a CLOSED localhost port so every capture
// candidate fails fast (no real device is opened, no blocking read) — exactly the
// hermetic seed test_mode_teardown uses.
denso::camera::Camera model_less_ip_cam(const std::string& name, bool active,
                                        bool setup_complete) {
    denso::camera::Camera c;
    c.name = name;
    c.camera_type = "ip";
    c.ip = "127.0.0.1";
    c.rtsp = "rtsp://127.0.0.1:9/none";  // port 9 (discard) — refused instantly
    c.channel = 1;
    c.stream = 0;
    c.username = "admin";
    c.password = "hunter2-SECRET";  // must NEVER surface in the retained list
    c.width = 1280;
    c.height = 720;
    c.fps = 25;
    c.active = active;
    c.setup_complete = setup_complete;
    c.areas_need_review = false;
    return c;
}

// True if a QPushButton with this objectName is VISIBLE on the currently-shown
// page (a control on a hidden QStackedWidget page is explicitly hidden, so
// isVisibleTo() reports false even before the top-level is shown).
bool visible_button(CameraView& v, const QString& object_name) {
    const auto btns = v.findChildren<QPushButton*>(object_name);
    return std::any_of(btns.begin(), btns.end(),
                       [&](QPushButton* b) { return b->isVisibleTo(&v); });
}

// True if some VISIBLE label on the shown page contains `needle`.
bool visible_label_contains(CameraView& v, const QString& needle) {
    const auto labels = v.findChildren<QLabel*>();
    return std::any_of(labels.begin(), labels.end(), [&](QLabel* l) {
        return l->isVisibleTo(&v) && l->text().contains(needle);
    });
}

QJsonObject read_status_json() {
    QFile f(denso::paths::status_file());
    if (!f.open(QIODevice::ReadOnly)) return {};
    return QJsonDocument::fromJson(f.readAll()).object();
}

// Build a real CameraView over an in-memory migrated DB inside a scratch data dir.
// The env guard is a MEMBER constructed right after `data`, so it captures the
// process's true prior DENSO_DATA_DIR (not a value a separate guard already
// overwrote) and restores it when the Harness — and its QTemporaryDir — go away.
struct Harness {
    QTemporaryDir data;
    ScopedDataDir data_guard{data.isValid() ? data.path().toUtf8() : QByteArray()};
    std::optional<denso::db::Db> db;
    std::shared_ptr<EngineRegistry> engines;
    std::unique_ptr<WarmupState> warmup;

    Harness() {
        REQUIRE(data.isValid());
        QDir(data.path()).mkpath(QStringLiteral("models"));  // readable, empty
        db = denso::db::Db::open_in_memory();
        REQUIRE(db);
        REQUIRE(denso::db::run_migrations(db->handle()));
        engines = std::make_shared<EngineRegistry>(
            denso::paths::models_dir().toStdString(),
            denso::paths::trt_cache_dir().toStdString(),
            std::set<std::string>{});  // model-less: empty allow-list, nothing loads
        warmup = std::make_unique<WarmupState>(engines);  // model-less path ignores it
    }
    QSqlDatabase handle() { return db->handle(); }
    CameraView make_view() { return CameraView(handle(), engines, warmup.get()); }
};

}  // namespace

TEST_CASE("digit_reader with no cameras shows the empty page and + Add Camera",
          "[camera_view_states]") {
    Harness h;
    // digit_reader is the default; no camera rows.

    CameraView view = h.make_view();

    CHECK(view.current_page_index() == 0);              // "No cameras yet"
    CHECK(visible_button(view, QStringLiteral("addCameraButton")));
    CHECK_FALSE(view.grid_has_live_streams());          // no live grid
    CHECK_FALSE(visible_button(view, QStringLiteral("setupCamerasButton")));
}

TEST_CASE("digit_reader with a runtime camera shows the live grid",
          "[camera_view_states]") {
    Harness h;
    REQUIRE(denso::camera::insert(
        h.handle(), model_less_ip_cam("Runtime Cam", /*active*/ true, /*setup*/ true)));
    REQUIRE(denso::camera::runtime(h.handle()).size() == 1);

    const uint64_t before = CameraStream::constructed_count();
    CameraView view = h.make_view();

    CHECK(view.current_page_index() == 1);              // live grid
    CHECK(view.grid_has_live_streams());
    // The model-less runtime camera builds exactly one production stream via the
    // OrientationProcessor path — no engine requested.
    CHECK(CameraStream::constructed_count() == before + 1);
}

TEST_CASE("digit_reader with a retained setup-incomplete camera shows setup-required",
          "[camera_view_states]") {
    Harness h;
    REQUIRE(denso::camera::insert(
        h.handle(), model_less_ip_cam("Line A", /*active*/ true, /*setup*/ false)));
    REQUIRE(denso::camera::all(h.handle()).size() == 1);
    REQUIRE(denso::camera::runtime(h.handle()).empty());

    const uint64_t before = CameraStream::constructed_count();
    CameraView view = h.make_view();

    CHECK(view.current_page_index() == 2);              // retained/setup-required page
    // The retained camera's name and credential-free source are listed.
    CHECK(visible_label_contains(view, QStringLiteral("Line A")));
    CHECK(visible_label_contains(view, QStringLiteral("127.0.0.1")));
    CHECK(visible_label_contains(view, QStringLiteral("processing setup required")));
    // The setup action is exposed; + Add Camera is NOT the primary empty-state
    // action (page 0 is not shown).
    CHECK(visible_button(view, QStringLiteral("setupCamerasButton")));
    CHECK_FALSE(visible_button(view, QStringLiteral("addCameraButton")));
    // No credential ever leaks into the retained list.
    CHECK_FALSE(visible_label_contains(view, QStringLiteral("hunter2-SECRET")));
    // No production stream is constructed, and the grid build path is not entered.
    CHECK(CameraStream::constructed_count() == before);
    CHECK_FALSE(view.grid_has_live_streams());
}

TEST_CASE("ball_leveler with a retained incomplete camera shows the unavailable page",
          "[camera_view_states]") {
    Harness h;
    REQUIRE(denso::mode::save(h.handle(), TargetMode::BallLeveler));
    REQUIRE(denso::camera::insert(
        h.handle(), model_less_ip_cam("Leveler Cam", /*active*/ false, /*setup*/ false)));

    const uint64_t streams_before = CameraStream::constructed_count();
    CameraView view = h.make_view();
    const uint64_t reloads_before = view.grid_reload_invocations();

    CHECK(view.current_page_index() == 2);              // unavailable page
    CHECK(visible_label_contains(
        view, QStringLiteral("Floating Ball Leveler setup is not available in this release")));
    // Retained connection shown read-only; NO setup/add/wizard action.
    CHECK(visible_label_contains(view, QStringLiteral("Leveler Cam")));
    CHECK_FALSE(visible_button(view, QStringLiteral("setupCamerasButton")));
    CHECK_FALSE(visible_button(view, QStringLiteral("addCameraButton")));
    // No runtime pipeline was built.
    CHECK(view.grid_reload_invocations() == reloads_before);
    CHECK(CameraStream::constructed_count() == streams_before);
    CHECK_FALSE(view.grid_has_live_streams());
    // status.json reports the mode with setup-required permanently true.
    const QJsonObject o = read_status_json();
    CHECK(o.value(QStringLiteral("mode")).toString() == QStringLiteral("ball_leveler"));
    CHECK(o.value(QStringLiteral("mode_setup_required")).toBool() == true);
}

TEST_CASE("ball_leveler cannot start a completed active camera (out-of-flow DB)",
          "[camera_view_states]") {
    Harness h;
    REQUIRE(denso::mode::save(h.handle(), TargetMode::BallLeveler));
    // The DB says this camera is completed AND active — camera::runtime() alone
    // would admit it. The mode gate must still refuse to start the digit grid.
    REQUIRE(denso::camera::insert(
        h.handle(), model_less_ip_cam("Rogue Cam", /*active*/ true, /*setup*/ true)));
    REQUIRE(denso::camera::runtime(h.handle()).size() == 1);

    const uint64_t streams_before = CameraStream::constructed_count();
    CameraView view = h.make_view();
    const uint64_t reloads_before = view.grid_reload_invocations();

    // Re-run reload() and prove the grid build path is NEVER entered across it.
    view.reload();

    CHECK(view.current_page_index() == 2);              // unavailable page
    CHECK(view.grid_reload_invocations() == reloads_before);       // no grid build
    CHECK(CameraStream::constructed_count() == streams_before);    // no stream built
    CHECK_FALSE(view.grid_has_live_streams());
    CHECK_FALSE(visible_button(view, QStringLiteral("setupCamerasButton")));
    CHECK_FALSE(visible_button(view, QStringLiteral("addCameraButton")));
    // Despite a completed camera, setup-required stays true for ball_leveler.
    const QJsonObject o = read_status_json();
    CHECK(o.value(QStringLiteral("mode")).toString() == QStringLiteral("ball_leveler"));
    CHECK(o.value(QStringLiteral("mode_setup_required")).toBool() == true);
}

TEST_CASE("retained list strips user-info from an IP field so no credential leaks",
          "[camera_view_states]") {
    Harness h;
    // The IP field is a free-text line edit; a pasted URL can leave "user:pass@"
    // in it. The retained list must show only the bare host, never the credential.
    denso::camera::Camera cam =
        model_less_ip_cam("Tainted Cam", /*active*/ true, /*setup*/ false);
    cam.ip = "operator:s3cr3t@10.0.0.9";
    REQUIRE(denso::camera::insert(h.handle(), cam));

    CameraView view = h.make_view();

    CHECK(view.current_page_index() == 2);
    CHECK(visible_label_contains(view, QStringLiteral("10.0.0.9")));      // bare host shown
    CHECK_FALSE(visible_label_contains(view, QStringLiteral("s3cr3t")));  // password stripped
    CHECK_FALSE(visible_label_contains(view, QStringLiteral("operator:"))); // user-info stripped
    CHECK_FALSE(visible_label_contains(view, QStringLiteral("@")));       // no user-info marker
}

TEST_CASE("ball_leveler with zero cameras shows the unavailable page, never page 0",
          "[camera_view_states]") {
    Harness h;
    REQUIRE(denso::mode::save(h.handle(), TargetMode::BallLeveler));
    REQUIRE(denso::camera::all(h.handle()).empty());

    const uint64_t streams_before = CameraStream::constructed_count();
    CameraView view = h.make_view();

    CHECK(view.current_page_index() == 2);              // unavailable, NOT page 0
    CHECK(visible_label_contains(
        view, QStringLiteral("Floating Ball Leveler setup is not available in this release")));
    CHECK_FALSE(visible_button(view, QStringLiteral("addCameraButton")));
    CHECK_FALSE(visible_button(view, QStringLiteral("setupCamerasButton")));
    CHECK(view.grid_reload_invocations() == 0);         // grid build path never entered
    CHECK(CameraStream::constructed_count() == streams_before);
    const QJsonObject o = read_status_json();
    CHECK(o.value(QStringLiteral("mode")).toString() == QStringLiteral("ball_leveler"));
    CHECK(o.value(QStringLiteral("mode_setup_required")).toBool() == true);
}
