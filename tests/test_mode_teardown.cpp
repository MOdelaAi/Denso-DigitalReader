// Slice 4 / Task 4.1 — the authoritative teardown-only seam.
//
// Regression guard (spec §6.2, §12.18): the pre-transaction teardown must call
// CameraGrid's ONE authoritative teardown (clear()) and must NOT re-query
// runtime() or restart the old-mode pipeline. So it constructs NO CameraStream
// and leaves no live stream — the view is parked on the neutral, non-live page.
//
// We prove "no construction" with the process-lifetime CameraStream construction
// counter (monotonic; only ever increments), captured at the precise boundary
// right before teardown — not merely "no live stream after", which a build-then-
// teardown would also satisfy.
//
// Runs in denso_integration_tests: the single offscreen QApplication is provided
// by integration_main.cpp. The seeded camera is model-less (active + setup, no
// attached models), so CameraGrid selects OrientationProcessor and never asks the
// EngineRegistry for an engine — no ORT/TensorRT engine is loaded, no GPU runs.
#include <catch2/catch_test_macros.hpp>

#include "camera/camera_stream.h"   // CameraStream::constructed_count()
#include "camera/repo.h"
#include "db/db.h"
#include "detection/engine_registry.h"
#include "paths/paths.h"
#include "ui/camera/camera_view.h"
#include "ui/camera/grid/camera_grid.h"   // CameraGrid::generation()
#include "ui/warmup_state.h"

#include <QByteArray>
#include <QTemporaryDir>
#include <memory>

using denso::ui::CameraGrid;
using denso::ui::CameraStream;
using denso::ui::CameraView;
using denso::ui::EngineRegistry;
using denso::ui::WarmupState;

namespace {
// Sets DENSO_DATA_DIR for the scope and restores the prior state on exit — even
// if a Catch2 assertion aborts the test — so integration cases sharing this one
// process can't leak a scratch data dir into each other. Restores the previous
// value if one was set, otherwise unsets.
struct ScopedDataDir {
    QByteArray prev_ = qgetenv("DENSO_DATA_DIR");
    bool had_ = qEnvironmentVariableIsSet("DENSO_DATA_DIR");
    explicit ScopedDataDir(const QByteArray& path) { qputenv("DENSO_DATA_DIR", path); }
    ~ScopedDataDir() {
        if (had_) qputenv("DENSO_DATA_DIR", prev_);
        else qunsetenv("DENSO_DATA_DIR");
    }
};
}  // namespace

TEST_CASE("teardown_for_switch stops the grid, shows the neutral page, and builds no stream",
          "[mode_teardown]") {
    // Keep every filesystem side effect (status.json, models_dir scan) inside a
    // scratch dir so the test is hermetic. data_dir() reads DENSO_DATA_DIR fresh;
    // the guard restores the prior env even if an assertion below aborts the test.
    QTemporaryDir data;
    REQUIRE(data.isValid());
    ScopedDataDir data_dir_guard(data.path().toUtf8());
    QDir(data.path()).mkpath(QStringLiteral("models"));  // readable, empty

    // In-memory, migrated DB. The QSqlDatabase copy shares the connection, so the
    // handle passed to CameraView drives the same in-memory store we seed here.
    auto db = denso::db::Db::open_in_memory();
    REQUIRE(db);
    REQUIRE(denso::db::run_migrations(db->handle()));

    // One runnable, MODEL-LESS camera: active + setup_complete, no camera_model
    // rows -> runtime() returns it and start_one selects OrientationProcessor.
    // An IP source pointing at a CLOSED localhost port makes every capture-ladder
    // candidate fail fast — the NVDEC GStreamer pipelines can't build (nv* elements
    // are absent on the Windows dev box) and the FFMPEG fallback hits an immediate
    // connection-refused — so the worker never enters a blocking read; it parks in
    // stop-responsive reconnect backoff and teardown joins promptly. (No real
    // device is opened, unlike a USB CAP_ANY fallback which can block on read.)
    denso::camera::Camera cam;
    cam.name = "cam";
    cam.camera_type = "ip";
    cam.ip = "127.0.0.1";
    cam.rtsp = "rtsp://127.0.0.1:9/none";  // port 9 (discard) — refused instantly
    cam.width = 1280;
    cam.height = 720;
    cam.fps = 25;
    cam.pitch = 0.0f;
    cam.roll = 0.0f;
    cam.rotation = 0;
    cam.active = true;
    cam.setup_complete = true;
    cam.areas_need_review = false;
    REQUIRE(denso::camera::insert(db->handle(), cam));
    REQUIRE(denso::camera::runtime(db->handle()).size() == 1);

    // Build the REAL view over the DB. Its ctor runs reload() -> start_one for the
    // model-less camera -> constructs exactly one production CameraStream. An empty
    // required-set registry is constructed but never queried (no models attached).
    auto engines = std::make_shared<EngineRegistry>(
        denso::paths::models_dir().toStdString(),
        denso::paths::trt_cache_dir().toStdString());
    auto warmup = std::make_unique<WarmupState>(engines);  // not started; model-less path ignores it
    CameraView view(db->handle(), engines, warmup.get());

    // The live grid is shown and a stream exists after the build.
    CHECK(view.current_page_index() == 1);        // live grid page
    CHECK(view.grid_has_live_streams());

    // Capture the construction tally at the precise boundary, then tear down.
    const uint64_t after_build = CameraStream::constructed_count();
    view.teardown_for_switch();

    // No NEW CameraStream was constructed by the teardown seam (it must not
    // reload()/re-query runtime()/restart the old pipeline).
    CHECK(CameraStream::constructed_count() == after_build);
    // No live stream remains, and the view is parked on the neutral, non-live page.
    CHECK_FALSE(view.grid_has_live_streams());
    CHECK(view.current_page_index() == 0);
    // DENSO_DATA_DIR is restored by ScopedDataDir on scope exit.
}

// Slice 8 / Step 4 — the deterministic, GPU-free half of the stale-callback proof
// (spec §12.15, §5-R2). The [callback_generation] unit cases prove the drop
// PREDICATE; this proves the grid actually ADVANCES the epoch that predicate
// compares against, so a callback captured by a worker before a rebuild is
// dropped after it. Driving a real inference worker to actually fire
// WorkerFailedFn would need an attached engine, which this model-less harness
// deliberately avoids, so that end-to-end path is NOT covered here — it is
// deferred to on-device validation and is not claimed to have been observed.
//
// CameraGrid is driven DIRECTLY here (not through CameraView) because generation()
// is a grid-level observable; the grid's ctor does not reload(), so the epoch
// starts at 0 and every advance below is caused by the call it follows.
TEST_CASE("every authoritative teardown advances the grid generation",
          "[mode_teardown]") {
    QTemporaryDir data;
    REQUIRE(data.isValid());
    ScopedDataDir data_dir_guard(data.path().toUtf8());
    QDir(data.path()).mkpath(QStringLiteral("models"));

    auto db = denso::db::Db::open_in_memory();
    REQUIRE(db);
    REQUIRE(denso::db::run_migrations(db->handle()));

    // Same model-less loopback source as the case above (see its comment for why
    // this URL is used): runtime() admits it, start_one picks OrientationProcessor,
    // and no engine is ever requested. Loopback, so no LAN device is contacted.
    denso::camera::Camera cam;
    cam.name = "cam";
    cam.camera_type = "ip";
    cam.ip = "127.0.0.1";
    cam.rtsp = "rtsp://127.0.0.1:9/none";  // port 9 (discard) — refused instantly
    cam.width = 1280;
    cam.height = 720;
    cam.fps = 25;
    cam.pitch = 0.0f;
    cam.roll = 0.0f;
    cam.rotation = 0;
    cam.active = true;
    cam.setup_complete = true;
    cam.areas_need_review = false;
    REQUIRE(denso::camera::insert(db->handle(), cam));

    auto engines = std::make_shared<EngineRegistry>(
        denso::paths::models_dir().toStdString(),
        denso::paths::trt_cache_dir().toStdString());
    auto warmup = std::make_unique<WarmupState>(engines);
    CameraGrid grid(db->handle(), engines, warmup.get());

    // The ctor builds no pipeline, so nothing has been torn down yet.
    const uint64_t at_build = grid.generation();

    // A teardown with a live pipeline advances the epoch.
    grid.reload();
    REQUIRE(grid.has_live_streams());
    const uint64_t after_reload = grid.generation();
    CHECK(after_reload > at_build);  // reload() clear()s first, so it advances too

    grid.teardown();
    const uint64_t after_teardown = grid.generation();
    CHECK(after_teardown > after_reload);
    CHECK_FALSE(grid.has_live_streams());

    // A rebuild advances it again — this is the transition that would otherwise
    // let an old-generation callback land on a RETAINED camera id (spec §11-R8).
    grid.reload();
    CHECK(grid.generation() > after_teardown);

    // Park the grid so no capture thread outlives the scratch data dir.
    grid.teardown();
    CHECK_FALSE(grid.has_live_streams());
}
