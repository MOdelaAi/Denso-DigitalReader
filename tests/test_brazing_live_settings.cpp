// Backend settings apply to the RUNNING appliance — no restart, no camera or
// model work — and the transport can never post to a doubled endpoint path.
//
// Three layers, each proving the part only it can see:
//   • SettingsDialog — validates, persists the CANONICAL base URL, and emits its
//     config-changed signal ONLY after the write succeeded;
//   • CameraGrid     — swaps only the reporting stack: one sender when enabled,
//     none when disabled, a replacement (not a duplicate) on a URL change, and a
//     no-op for an unchanged Save. Capture/inference are observably untouched;
//   • BrazingReporter / BrazingClient — a retired sender arms no further retry
//     and cannot be re-entered by a late reply, and each client's endpoint is
//     fixed at construction, so no request can begin against a retired URL.
//
// Drives the REAL objects over the app library. Nothing here contacts the PC
// backend, a camera or the Internet: every fixture camera points at a closed
// localhost port, every camera is model-less (no engine is ever loaded), and the
// transport is a fake in the reporter cases. No BrazingClient in this file is
// ever asked to post.
#include <catch2/catch_test_macros.hpp>

#include "brazing/brazing_client.h"
#include "brazing/brazing_reporter.h"
#include "brazing/brazing_transport.h"
#include "brazing/config.h"
#include "brazing/url.h"
#include "camera/camera.h"
#include "camera/camera_stream.h"      // CameraStream::constructed_count()
#include "camera/frame_processor.h"    // DetectionProcessor::constructed_count()
#include "camera/repo.h"
#include "db/db.h"
#include "detection/engine_registry.h"
#include "mode/config.h"
#include "mode/mode.h"
#include "mode/reset.h"
#include "paths/paths.h"
#include "ui/camera/grid/camera_grid.h"
#include "ui/settings/settings_dialog.h"

#include <QByteArray>
#include <QCheckBox>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QLabel>
#include <QLineEdit>
#include <QObject>
#include <QPushButton>
#include <QSqlQuery>
#include <QString>
#include <QTemporaryDir>

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>

using denso::mode::TargetMode;
using denso::ui::BrazingClient;
using denso::ui::BrazingReporter;
using denso::ui::BrazingTransport;
using denso::ui::CameraGrid;
using denso::ui::CameraStream;
using denso::ui::DetectionProcessor;
using denso::ui::EngineRegistry;
using denso::ui::SettingsDialog;
using denso::ui::ZoneValue;

namespace {

// The confirmed PC test backend, and a second address to switch to. Neither is
// ever contacted — they exist only as configuration strings.
constexpr const char* kUrlA = "http://192.168.1.112:8080";
constexpr const char* kUrlB = "http://192.168.1.113:9090";
constexpr const char* kUrlAEndpoint = "http://192.168.1.112:8080/api/brazing/update";

struct ScopedDataDir {
    QByteArray prev_ = qgetenv("DENSO_DATA_DIR");
    bool had_ = qEnvironmentVariableIsSet("DENSO_DATA_DIR");
    explicit ScopedDataDir(const QByteArray& path) { qputenv("DENSO_DATA_DIR", path); }
    ~ScopedDataDir() {
        if (had_) qputenv("DENSO_DATA_DIR", prev_);
        else qunsetenv("DENSO_DATA_DIR");
    }
};

// A model-less camera on a CLOSED localhost port: the capture candidate fails
// fast, so no device is opened and no engine is requested — the hermetic seed
// the other CameraGrid suites use.
denso::camera::Camera model_less_cam(const std::string& name) {
    denso::camera::Camera c;
    c.name = name;
    c.camera_type = "ip";
    c.ip = "127.0.0.1";
    c.rtsp = "rtsp://127.0.0.1:9/none";   // discard port — refused instantly
    c.channel = 1;
    c.stream = 0;
    c.width = 1280;
    c.height = 720;
    c.fps = 25;
    c.active = true;
    c.setup_complete = true;
    c.areas_need_review = false;
    return c;
}

struct Harness {
    QTemporaryDir data;
    ScopedDataDir data_guard{data.isValid() ? data.path().toUtf8() : QByteArray()};
    std::optional<denso::db::Db> db;
    std::shared_ptr<EngineRegistry> engines;

    Harness() {
        REQUIRE(data.isValid());
        QDir(data.path()).mkpath(QStringLiteral("models"));   // readable, empty
        db = denso::db::Db::open_in_memory();
        REQUIRE(db);
        REQUIRE(denso::db::run_migrations(db->handle()));
        engines = std::make_shared<EngineRegistry>(
            denso::paths::models_dir().toStdString(),
            denso::paths::trt_cache_dir().toStdString(),
            std::set<std::string>{});   // empty allow-list: nothing ever loads
    }

    QSqlDatabase h() { return db->handle(); }

    void seed_camera() {
        REQUIRE(denso::camera::insert(h(), model_less_cam("Line A")));
        REQUIRE(denso::camera::runtime(h()).size() == 1);
    }

    // Write the stored Backend configuration DIRECTLY, standing in for whatever
    // wrote it (the dialog, a mode switch, a restored backup).
    void store_brazing(bool enabled, const std::string& base_url) {
        denso::brazing::BrazingConfig c;
        c.enabled = enabled;
        c.base_url = base_url;
        REQUIRE(denso::brazing::save(h(), c));
    }
};

// Run the event loop for `ms`, so a queued call or a single-shot timer that
// SHOULD fire gets its chance — the only way "no retry happened" means anything.
void spin(int ms) {
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < ms) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
}

// A transport whose POSTs always fail, so the retry policy arms its timer. The
// last `done` callback is kept OUTSIDE the transport (which the reporter owns and
// destroys), so a test can still invoke it after the reporter is gone.
struct FailingTransport : BrazingTransport {
    struct State {
        int posts = 0;
        std::function<void(bool)> last_done;
    };
    explicit FailingTransport(State& s) : st(s) {}
    void post(const std::map<int, ZoneValue>&, std::function<void(bool)> done) override {
        ++st.posts;
        st.last_done = std::move(done);
    }
    State& st;
};

QString status_text(SettingsDialog& dlg) {
    auto* l = dlg.findChild<QLabel*>(QStringLiteral("brazingStatus"));
    REQUIRE(l != nullptr);
    return l->text();
}

} // namespace

// ── SettingsDialog: validate, persist canonical, then signal ─────────────────

TEST_CASE("Settings Save normalizes a pasted endpoint and reports the base URL",
          "[brazing_live]") {
    auto db = denso::db::Db::open_in_memory();
    REQUIRE(db);
    REQUIRE(denso::db::run_migrations(db->handle()));

    SettingsDialog dlg(db->handle());
    auto* url = dlg.findChild<QLineEdit*>(QStringLiteral("brazingUrl"));
    auto* on = dlg.findChild<QCheckBox*>(QStringLiteral("brazingEnabled"));
    auto* save = dlg.findChild<QPushButton*>(QStringLiteral("brazingSave"));
    REQUIRE(url != nullptr);
    REQUIRE(on != nullptr);
    REQUIRE(save != nullptr);

    int emitted = 0;
    QObject::connect(&dlg, &SettingsDialog::brazing_config_changed,
                     [&] { ++emitted; });

    // The operator pastes the complete endpoint they were told to POST to.
    on->setChecked(true);
    url->setText(QStringLiteral("  http://192.168.1.112:8080/api/brazing/update/ "));
    save->click();

    CHECK(emitted == 1);
    // PERSISTED as the base URL...
    const auto stored = denso::brazing::load(db->handle());
    CHECK(stored.enabled);
    CHECK(stored.base_url == kUrlA);
    // ...and the field now shows exactly what was stored, so the operator is
    // never looking at something other than the truth.
    CHECK(url->text() == QString::fromLatin1(kUrlA));
    CHECK_FALSE(status_text(dlg).isEmpty());     // an explicit success line
}

TEST_CASE("Settings Save refuses an arbitrary path and persists nothing",
          "[brazing_live]") {
    auto db = denso::db::Db::open_in_memory();
    REQUIRE(db);
    REQUIRE(denso::db::run_migrations(db->handle()));

    SettingsDialog dlg(db->handle());
    auto* url = dlg.findChild<QLineEdit*>(QStringLiteral("brazingUrl"));
    auto* on = dlg.findChild<QCheckBox*>(QStringLiteral("brazingEnabled"));
    auto* save = dlg.findChild<QPushButton*>(QStringLiteral("brazingSave"));
    REQUIRE(url != nullptr);

    int emitted = 0;
    QObject::connect(&dlg, &SettingsDialog::brazing_config_changed,
                     [&] { ++emitted; });

    on->setChecked(true);
    url->setText(QStringLiteral("http://192.168.1.112:8080/other/path"));
    save->click();

    CHECK(emitted == 0);                                   // no live reconfiguration
    CHECK_FALSE(denso::brazing::load(db->handle()).enabled);
    CHECK(denso::brazing::load(db->handle()).base_url.empty());
    CHECK_FALSE(status_text(dlg).isEmpty());               // a visible reason
    CHECK(url->property("invalid").toBool());              // and the red field
    // The rejected text is left alone — never silently rewritten into something
    // that would post somewhere the operator did not ask for.
    CHECK(url->text() == QStringLiteral("http://192.168.1.112:8080/other/path"));
}

TEST_CASE("Settings Save refuses to enable reporting with no address",
          "[brazing_live]") {
    auto db = denso::db::Db::open_in_memory();
    REQUIRE(db);
    REQUIRE(denso::db::run_migrations(db->handle()));

    SettingsDialog dlg(db->handle());
    auto* on = dlg.findChild<QCheckBox*>(QStringLiteral("brazingEnabled"));
    auto* save = dlg.findChild<QPushButton*>(QStringLiteral("brazingSave"));
    int emitted = 0;
    QObject::connect(&dlg, &SettingsDialog::brazing_config_changed,
                     [&] { ++emitted; });

    on->setChecked(true);   // URL left empty
    save->click();

    CHECK(emitted == 0);
    CHECK_FALSE(denso::brazing::load(db->handle()).enabled);
    CHECK_FALSE(status_text(dlg).isEmpty());
}

TEST_CASE("Settings Save emits nothing when the write fails", "[brazing_live]") {
    // The signal means "the STORED config changed, re-read it". Emitting it for a
    // write that did not land would make the running pipeline adopt a
    // configuration the database does not hold. Forced deterministically by
    // removing the table the upsert targets — no mock, no timing.
    auto db = denso::db::Db::open_in_memory();
    REQUIRE(db);
    REQUIRE(denso::db::run_migrations(db->handle()));

    SettingsDialog dlg(db->handle());
    auto* url = dlg.findChild<QLineEdit*>(QStringLiteral("brazingUrl"));
    auto* on = dlg.findChild<QCheckBox*>(QStringLiteral("brazingEnabled"));
    auto* save = dlg.findChild<QPushButton*>(QStringLiteral("brazingSave"));

    QSqlQuery drop(db->handle());
    REQUIRE(drop.exec(QStringLiteral("DROP TABLE settings")));

    int emitted = 0;
    QObject::connect(&dlg, &SettingsDialog::brazing_config_changed,
                     [&] { ++emitted; });

    on->setChecked(true);
    url->setText(QString::fromLatin1(kUrlA));
    save->click();

    CHECK(emitted == 0);
    CHECK_FALSE(status_text(dlg).isEmpty());   // the failure is reported, not hidden
}

// ── CameraGrid: the live reporting-stack swap ────────────────────────────────

TEST_CASE("enabling reporting live creates exactly one sender", "[brazing_live]") {
    Harness h;
    h.seed_camera();
    h.store_brazing(false, "");            // boots with reporting off

    CameraGrid grid(h.h(), h.engines, /*warmup*/ nullptr);
    grid.reload();
    REQUIRE_FALSE(grid.has_brazing_sender());
    REQUIRE(grid.brazing_sender_builds() == 0);

    h.store_brazing(true, kUrlA);
    grid.apply_brazing_config();

    CHECK(grid.has_brazing_sender());
    CHECK(grid.brazing_sender_builds() == 1);
    CHECK(grid.active_brazing_base_url() == kUrlA);
}

TEST_CASE("saving an unchanged configuration creates no second reporter",
          "[brazing_live]") {
    Harness h;
    h.seed_camera();
    h.store_brazing(true, kUrlA);

    CameraGrid grid(h.h(), h.engines, /*warmup*/ nullptr);
    grid.reload();
    REQUIRE(grid.brazing_sender_builds() == 1);

    grid.apply_brazing_config();
    grid.apply_brazing_config();
    grid.apply_brazing_config();

    // Idempotent: one sender, so one POST per snapshot. A rebuild on every Save
    // would also silently reset delivery state and re-send acked values.
    CHECK(grid.brazing_sender_builds() == 1);
    CHECK(grid.has_brazing_sender());
}

TEST_CASE("a different spelling of the same server is not a change",
          "[brazing_live]") {
    // The UI stores the canonical base, but a legacy or externally written row can
    // hold the full endpoint. Both denote ONE server, so the sender must not churn.
    Harness h;
    h.seed_camera();
    h.store_brazing(true, kUrlA);

    CameraGrid grid(h.h(), h.engines, /*warmup*/ nullptr);
    grid.reload();
    REQUIRE(grid.brazing_sender_builds() == 1);

    h.store_brazing(true, kUrlAEndpoint);
    grid.apply_brazing_config();

    CHECK(grid.brazing_sender_builds() == 1);
    CHECK(grid.active_brazing_base_url() == kUrlA);
}

TEST_CASE("disabling reporting live stops delivery immediately", "[brazing_live]") {
    Harness h;
    h.seed_camera();
    h.store_brazing(true, kUrlA);

    CameraGrid grid(h.h(), h.engines, /*warmup*/ nullptr);
    grid.reload();
    REQUIRE(grid.has_brazing_sender());

    h.store_brazing(false, kUrlA);      // the address is deliberately preserved
    grid.apply_brazing_config();

    CHECK_FALSE(grid.has_brazing_sender());
    CHECK(grid.active_brazing_base_url().empty());
    CHECK(grid.brazing_sender_builds() == 1);   // nothing was rebuilt on the way out
    // Aggregation continues: the on-screen readings are a LOCAL check and must
    // not depend on the backend.
    CHECK(grid.has_live_streams());
}

TEST_CASE("changing the URL live replaces the sender", "[brazing_live]") {
    Harness h;
    h.seed_camera();
    h.store_brazing(true, kUrlA);

    CameraGrid grid(h.h(), h.engines, /*warmup*/ nullptr);
    grid.reload();
    REQUIRE(grid.brazing_sender_builds() == 1);
    REQUIRE(grid.active_brazing_base_url() == kUrlA);

    h.store_brazing(true, kUrlB);
    grid.apply_brazing_config();

    // REPLACED, not added: one live sender, built a second time, on the new URL.
    CHECK(grid.has_brazing_sender());
    CHECK(grid.brazing_sender_builds() == 2);
    CHECK(grid.active_brazing_base_url() == kUrlB);
    // The old BrazingReporter (and with it its retry timer, its queued snapshot
    // and the BrazingClient bound to kUrlA) was destroyed before the replacement
    // was constructed — see the reporter/client cases below for what that means
    // for in-flight and retried requests.
}

TEST_CASE("an unusable stored address starts no sender", "[brazing_live]") {
    Harness h;
    h.seed_camera();
    h.store_brazing(true, "http://192.168.1.112:8080/other/path");

    CameraGrid grid(h.h(), h.engines, /*warmup*/ nullptr);
    grid.reload();

    // Enabled, but the address is not a base URL. Refuse rather than post to a
    // resource the operator never asked for.
    CHECK_FALSE(grid.has_brazing_sender());
    CHECK(grid.brazing_sender_builds() == 0);
    CHECK(grid.has_live_streams());   // …and the appliance keeps reading
}

TEST_CASE("a Backend settings change restarts no camera and no model",
          "[brazing_live]") {
    Harness h;
    h.seed_camera();
    h.store_brazing(false, "");

    CameraGrid grid(h.h(), h.engines, /*warmup*/ nullptr);
    grid.reload();
    REQUIRE(grid.has_live_streams());

    // reload_invocations() is the grid's build path — the SOLE constructor of the
    // reporter, ZoneHealth, every processor and every CameraStream. generation()
    // advances on every authoritative teardown. Both frozen here means nothing
    // was torn down or rebuilt.
    const uint64_t reloads = grid.reload_invocations();
    const uint64_t gen = grid.generation();
    const uint64_t streams = CameraStream::constructed_count();
    const uint64_t procs = DetectionProcessor::constructed_count();

    h.store_brazing(true, kUrlA);
    grid.apply_brazing_config();
    h.store_brazing(true, kUrlB);
    grid.apply_brazing_config();
    h.store_brazing(false, kUrlB);
    grid.apply_brazing_config();

    CHECK(grid.reload_invocations() == reloads);
    CHECK(grid.generation() == gen);
    CHECK(CameraStream::constructed_count() == streams);
    CHECK(DetectionProcessor::constructed_count() == procs);
    CHECK(grid.has_live_streams());
}

TEST_CASE("a mode switch still disables reporting and preserves the address",
          "[brazing_live]") {
    // Unchanged rule, restated at this seam because the live-reload work is the
    // obvious place someone would "helpfully" make the switch keep reporting on.
    // No grid: switch_and_reset's contract is that the runtime is already torn
    // down when it runs.
    Harness h;
    h.store_brazing(true, kUrlA);

    const auto res = denso::mode::switch_and_reset(h.h(), TargetMode::BallLeveler);
    REQUIRE(res.ok);

    const auto cfg = denso::brazing::load(h.h());
    CHECK_FALSE(cfg.enabled);
    CHECK(cfg.base_url == kUrlA);   // the operator need not retype it
}

TEST_CASE("re-enabling after a disable starts the sender again with no restart",
          "[brazing_live]") {
    // The state a mode switch leaves behind: reporting off, the address kept. The
    // operator ticks the box and saves — and delivery must resume there and then.
    Harness h;
    h.seed_camera();
    h.store_brazing(false, kUrlA);

    CameraGrid grid(h.h(), h.engines, /*warmup*/ nullptr);
    grid.reload();
    REQUIRE_FALSE(grid.has_brazing_sender());
    const uint64_t reloads = grid.reload_invocations();

    h.store_brazing(true, kUrlA);
    grid.apply_brazing_config();

    CHECK(grid.has_brazing_sender());
    CHECK(grid.active_brazing_base_url() == kUrlA);
    CHECK(grid.reload_invocations() == reloads);   // the app was never restarted
}

// ── The retired sender: no retry, no re-entry ────────────────────────────────

TEST_CASE("destroying a sender cancels its armed retry", "[brazing_live]") {
    // What "retire the old sender" has to mean: the single-shot retry QTimer is a
    // child of the reporter, so destruction cancels it. Without that, a POST
    // armed against the OLD address would fire after the operator changed it.
    FailingTransport::State st;
    {
        BrazingReporter reporter(std::make_unique<FailingTransport>(st));
        reporter.submit(std::map<int, ZoneValue>{{1, ZoneValue{42}}});
        REQUIRE(st.posts == 1);
        REQUIRE(st.last_done);
        st.last_done(false);          // the server rejected it → backoff armed
        REQUIRE(st.posts == 1);
    }   // ~BrazingReporter — the timer dies here.

    // Well past the 1 s first retry delay.
    spin(1400);
    CHECK(st.posts == 1);             // nothing was ever re-sent
}

TEST_CASE("a reply completing after replacement cannot re-enter the old sender",
          "[brazing_live]") {
    // The real ordering on a URL change: a POST is in flight, the sender is
    // destroyed, and only then does the network reply arrive. The QPointer guard
    // in BrazingReporter::apply is what stops that callback from driving a
    // destroyed policy — and from starting another request to the old address.
    FailingTransport::State st;
    {
        BrazingReporter reporter(std::make_unique<FailingTransport>(st));
        reporter.submit(std::map<int, ZoneValue>{{1, ZoneValue{42}}});
        REQUIRE(st.posts == 1);
        REQUIRE(st.last_done);
    }   // destroyed with the POST still in flight

    st.last_done(false);              // the late reply — must be inert
    spin(1400);
    CHECK(st.posts == 1);
}

// ── The transport boundary: a fixed, never-doubled endpoint ──────────────────

TEST_CASE("BrazingClient composes the endpoint exactly once", "[brazing_live]") {
    // No network: the composed URL is read straight off the client. Each client
    // binds ONE endpoint at construction, which is why a replaced client cannot
    // begin a request against the address it replaced.
    BrazingClient plain(kUrlA);
    CHECK(plain.endpoint() == QString::fromLatin1(kUrlAEndpoint));

    BrazingClient slashed(std::string(kUrlA) + "/");
    CHECK(slashed.endpoint() == QString::fromLatin1(kUrlAEndpoint));

    // The defect: a stored value that ALREADY carries the endpoint.
    BrazingClient pasted(kUrlAEndpoint);
    CHECK(pasted.endpoint() == QString::fromLatin1(kUrlAEndpoint));
    CHECK_FALSE(pasted.endpoint().contains(
        QStringLiteral("/api/brazing/update/api/brazing/update")));
}

TEST_CASE("BrazingClient with no address posts nothing", "[brazing_live]") {
    BrazingClient unset(std::string{});
    CHECK(unset.endpoint().isEmpty());

    bool called = false;
    bool ok = true;
    unset.post({{1, ZoneValue{42}}}, [&](bool result) { called = true; ok = result; });
    CHECK(called);          // reported immediately...
    CHECK_FALSE(ok);        // ...as a failure, with no request issued
}

TEST_CASE("BrazingClient refuses an address the dialog would reject",
          "[brazing_live]") {
    // The transport must not be more permissive than the UI: if it composed
    // ".../other/path/api/brazing/update" from a config the dialog refuses, the
    // two boundaries would disagree and an arbitrary path would reach the wire
    // through a construction path that skipped validation.
    BrazingClient odd_path("http://192.168.1.112:8080/other/path");
    CHECK(odd_path.endpoint().isEmpty());

    bool called = false;
    bool ok = true;
    odd_path.post({{1, ZoneValue{42}}},
                  [&](bool result) { called = true; ok = result; });
    CHECK(called);
    CHECK_FALSE(ok);        // reported as a failure — nothing was sent anywhere
}
