// The three main-UI usability changes, over the REAL widgets:
//
//   A. Settings has ONE primary action. "Save changes" validates, persists and
//      only then applies; "Cancel" leaves the database and the running app
//      exactly as it found them. The old gold "Apply" (display only) and the
//      Server page's own gold "Save" (brazing only) are gone.
//   B. The top bar reports backend reporting from the ONE authority that owns
//      the sender, and never calls an enabled sender "connected".
//   C. "Refresh Cameras" is a runtime rebuild through the existing seam — no
//      write, no reset, no duplicate runtime.
//
// Hermetic: every fixture camera is model-less and points at a closed localhost
// port, the EngineRegistry gets an empty allow-list so no engine is ever loaded,
// and no BrazingClient here is ever asked to post. Nothing contacts the PC
// backend, a camera or the network.
#include <catch2/catch_test_macros.hpp>

#include "brazing/brazing_reporter.h"
#include "brazing/brazing_status.h"
#include "brazing/brazing_transport.h"
#include "brazing_form_util.h"

#include "brazing/config.h"
#include "camera/camera.h"
#include "camera/camera_stream.h"      // CameraStream::constructed_count()
#include "camera/repo.h"
#include "db/db.h"
#include "detection/engine_registry.h"
#include "detection/repo.h"
#include "mode/config.h"
#include "mode/mode.h"
#include "paths/paths.h"
#include "settings/repo.h"
#include "settings/settings.h"
#include "ui/camera/camera_view.h"
#include "ui/mainwindow.h"
#include "ui/settings/settings_dialog.h"
#include "ui/warmup_state.h"

#include <QByteArray>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QIODevice>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QObject>
#include <QPushButton>
#include <QSqlQuery>
#include <QSqlRecord>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QTimer>

#include <algorithm>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

using denso::mode::TargetMode;
using denso::ui::BrazingReporter;
using denso::ui::BrazingStatus;
using denso::ui::BrazingTransport;
using denso::ui::CameraStream;
using denso::ui::EngineRegistry;
using denso::ui::MainWindow;
using denso::ui::SettingsDialog;
using denso::ui::WarmupState;
using denso::ui::ZoneValue;

namespace {

constexpr const char* kUrlA = "http://192.168.1.112:8080";
constexpr const char* kUrlB = "http://192.168.1.113:9090";

struct ScopedDataDir {
    QByteArray prev_ = qgetenv("DENSO_DATA_DIR");
    bool had_ = qEnvironmentVariableIsSet("DENSO_DATA_DIR");
    explicit ScopedDataDir(const QByteArray& path) { qputenv("DENSO_DATA_DIR", path); }
    ~ScopedDataDir() {
        if (had_) qputenv("DENSO_DATA_DIR", prev_);
        else qunsetenv("DENSO_DATA_DIR");
    }
};

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

/// Run the event loop for `ms`. Refresh defers its rebuild one tick so the busy
/// state paints first, so a test MUST pump to observe the outcome — and pumping
/// is also what would let a wrongly-queued second rebuild show itself.
void spin(int ms) {
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < ms) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    }
}

QString read_source(const char* rel) {
    QFile f(QStringLiteral(DENSO_SOURCE_DIR) + QString::fromUtf8(rel));
    REQUIRE(f.open(QIODevice::ReadOnly));
    return QString::fromUtf8(f.readAll());
}

struct Harness {
    QTemporaryDir data;
    ScopedDataDir data_guard{data.isValid() ? data.path().toUtf8() : QByteArray()};
    std::optional<denso::db::Db> db;
    std::shared_ptr<denso::settings::Settings> state;
    std::shared_ptr<EngineRegistry> engines;
    std::unique_ptr<WarmupState> warmup;

    Harness() {
        REQUIRE(data.isValid());
        QDir(data.path()).mkpath(QStringLiteral("models"));
        db = denso::db::Db::open_in_memory();
        REQUIRE(db);
        REQUIRE(denso::db::run_migrations(db->handle()));
        state = std::make_shared<denso::settings::Settings>();
        engines = std::make_shared<EngineRegistry>(
            denso::paths::models_dir().toStdString(),
            denso::paths::trt_cache_dir().toStdString(),
            std::set<std::string>{});
        warmup = std::make_unique<WarmupState>(engines);
    }

    QSqlDatabase h() { return db->handle(); }

    int64_t seed_camera(const std::string& name = "Line A") {
        const auto id = denso::camera::insert(h(), model_less_cam(name));
        REQUIRE(id);
        return *id;
    }

    void store_brazing(bool enabled, const std::string& base_url) {
        denso::brazing::BrazingConfig c;
        c.enabled = enabled;
        c.base_url = base_url;
        REQUIRE(denso::brazing::save(h(), c));
    }

    std::unique_ptr<MainWindow> make_window() {
        return std::make_unique<MainWindow>(h(), state, engines, warmup.get());
    }
};

// ── Widget lookups ───────────────────────────────────────────────────────────

QPushButton* btn(QWidget& w, const char* name) {
    return w.findChild<QPushButton*>(QString::fromLatin1(name));
}
QLineEdit* line(QWidget& w, const char* name) {
    return w.findChild<QLineEdit*>(QString::fromLatin1(name));
}
QCheckBox* check(QWidget& w, const char* name) {
    return w.findChild<QCheckBox*>(QString::fromLatin1(name));
}
QLabel* label(QWidget& w, const char* name) {
    return w.findChild<QLabel*>(QString::fromLatin1(name));
}

/// Every signal Save/Cancel may or may not fire, counted in one place so a case
/// can assert on the WHOLE set — "nothing was applied" is only meaningful if
/// every apply signal is watched, not just the interesting one.
struct SignalTally {
    int brazing = 0;
    int theme_commit = 0;
    int theme_preview = 0;
    bool last_preview_dark = false;
    int display = 0;

    void watch(SettingsDialog& d) {
        QObject::connect(&d, &SettingsDialog::brazing_config_changed,
                         [this] { ++brazing; });
        QObject::connect(&d, &SettingsDialog::theme_changed, [this](bool) { ++theme_commit; });
        QObject::connect(&d, &SettingsDialog::theme_preview_requested, [this](bool dark) {
            ++theme_preview;
            last_preview_dark = dark;
        });
        QObject::connect(&d, &SettingsDialog::apply_display_requested,
                         [this](int, int, int) { ++display; });
    }
    int applies() const { return brazing + theme_commit + display; }
};

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────
// A. One primary action
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Settings exposes exactly one primary action", "[ui_controls][settings]") {
    Harness h;
    SettingsDialog dlg(h.h());

    REQUIRE(btn(dlg, "saveChangesButton") != nullptr);
    REQUIRE(btn(dlg, "cancelButton") != nullptr);
    CHECK(btn(dlg, "saveChangesButton")->text() == QStringLiteral("Save changes"));
    CHECK(btn(dlg, "cancelButton")->text() == QStringLiteral("Cancel"));

    // "Primary" is carried by the gold property in this theme, so counting gold
    // buttons counts primaries. Exactly one, and it is Save changes — this is what
    // the old dialog failed: a gold Apply in the footer AND a gold Save on the
    // Server page, each committing a different, invisible subset.
    const auto all = dlg.findChildren<QPushButton*>();
    std::vector<QPushButton*> gold;
    std::copy_if(all.begin(), all.end(), std::back_inserter(gold),
                 [](QPushButton* b) { return b->property("gold").toBool(); });
    REQUIRE(gold.size() == 1);
    CHECK(gold.front()->objectName() == QStringLiteral("saveChangesButton"));
}

TEST_CASE("the separate Apply and page-level Save actions are gone",
          "[ui_controls][settings]") {
    Harness h;
    SettingsDialog dlg(h.h());

    // The Server page's own commit button no longer exists.
    CHECK(btn(dlg, "brazingSave") == nullptr);

    // No PRIMARY action says "Apply" or "Save" any more. NetworkPanel still has a
    // per-adapter "Apply" inside its cards — that applies one adapter's IP
    // configuration and is a flat card action, not a second way to commit this
    // form — so the assertion is scoped to primaries, which is where the
    // ambiguity actually lived.
    for (QPushButton* b : dlg.findChildren<QPushButton*>()) {
        if (!b->property("gold").toBool()) continue;
        CHECK(b->text() != QStringLiteral("Apply"));
        CHECK(b->text() != QStringLiteral("Save"));
    }
}

TEST_CASE("Save changes validates before it persists anything",
          "[ui_controls][settings]") {
    Harness h;
    SettingsDialog dlg(h.h());
    SignalTally sig;
    sig.watch(dlg);

    check(dlg, "brazingEnabled")->setChecked(true);
    // The decomposed form's version of the same operator error: a whole URL
    // pasted into the field that takes only the address.
    denso::testing::brazing_host(dlg)->setText(
        QStringLiteral("http://192.168.1.112:8080/other/path"));
    btn(dlg, "saveChangesButton")->click();

    // Nothing persisted, NOTHING applied — not the brazing config, not the theme,
    // not the display. A partially-applied form is the failure this ordering
    // exists to prevent.
    CHECK_FALSE(denso::brazing::load(h.h()).enabled);
    CHECK(denso::brazing::load(h.h()).base_url.empty());
    CHECK(sig.applies() == 0);
    // The dialog stays open, on the page that failed, with the reason visible.
    CHECK(dlg.result() != QDialog::Accepted);
    CHECK_FALSE(label(dlg, "brazingStatus")->text().isEmpty());
    CHECK(dlg.findChild<QListWidget*>(QStringLiteral("navList"))->currentRow() == 4);
}

TEST_CASE("a failed write applies nothing", "[ui_controls][settings]") {
    // The runtime must never adopt a configuration the database does not hold.
    // Forced deterministically by removing the table the upserts target.
    Harness h;
    SettingsDialog dlg(h.h());
    SignalTally sig;
    sig.watch(dlg);

    QSqlQuery q(h.h());
    REQUIRE(q.exec(QStringLiteral("DROP TABLE settings")));

    check(dlg, "brazingEnabled")->setChecked(true);
    denso::testing::set_brazing_base(dlg, QString::fromLatin1(kUrlA));
    btn(dlg, "saveChangesButton")->click();

    CHECK(sig.applies() == 0);
    CHECK(dlg.result() != QDialog::Accepted);
    CHECK_FALSE(label(dlg, "brazingStatus")->text().isEmpty());
}

TEST_CASE("Save changes persists and applies every dirty page",
          "[ui_controls][settings]") {
    Harness h;
    SettingsDialog dlg(h.h());
    dlg.set_theme_dark(true);
    SignalTally sig;
    sig.watch(dlg);

    check(dlg, "brazingEnabled")->setChecked(true);
    denso::testing::set_brazing_base(dlg, QString::fromLatin1(kUrlA));
    check(dlg, "darkModeSwitch")->setChecked(false);
    // The theme previewed immediately (no persistence yet) — that is the whole
    // point of splitting preview from commit.
    CHECK(sig.theme_preview == 1);
    CHECK(sig.theme_commit == 0);

    btn(dlg, "saveChangesButton")->click();

    const auto stored = denso::brazing::load(h.h());
    CHECK(stored.enabled);
    CHECK(stored.base_url == kUrlA);
    CHECK(sig.brazing == 1);       // backend reload requested
    CHECK(sig.theme_commit == 1);  // theme committed
    CHECK(sig.display == 1);       // display transaction requested
    CHECK(dlg.result() == QDialog::Accepted);   // closes on success
}

TEST_CASE("Cancel persists nothing and applies nothing", "[ui_controls][settings]") {
    Harness h;
    h.store_brazing(false, "");
    SettingsDialog dlg(h.h());
    dlg.set_theme_dark(true);
    SignalTally sig;
    sig.watch(dlg);

    check(dlg, "brazingEnabled")->setChecked(true);
    denso::testing::set_brazing_base(dlg, QString::fromLatin1(kUrlB));
    check(dlg, "darkModeSwitch")->setChecked(false);   // previews light
    REQUIRE(sig.theme_preview == 1);

    btn(dlg, "cancelButton")->click();

    // Nothing was written…
    const auto stored = denso::brazing::load(h.h());
    CHECK_FALSE(stored.enabled);
    CHECK(stored.base_url.empty());
    // …and no COMMIT signal fired.
    CHECK(sig.brazing == 0);
    CHECK(sig.theme_commit == 0);
    CHECK(sig.display == 0);
    // The one thing that WAS applied — the live theme preview — is undone, so the
    // running app is left exactly as the dialog found it.
    CHECK(sig.theme_preview == 2);
    CHECK(sig.last_preview_dark == true);
    CHECK(dlg.result() == QDialog::Rejected);
}

TEST_CASE("Save changes is disabled until something is edited",
          "[ui_controls][settings]") {
    Harness h;
    h.store_brazing(true, kUrlA);
    SettingsDialog dlg(h.h());
    dlg.show();          // showEvent re-seeds from the DB and clears dirty
    spin(30);

    // Seeding is not an edit: opening Settings must not arm the primary action.
    CHECK_FALSE(dlg.is_dirty());
    CHECK_FALSE(btn(dlg, "saveChangesButton")->isEnabled());

    check(dlg, "brazingEnabled")->setChecked(false);
    CHECK(dlg.is_dirty());
    CHECK(btn(dlg, "saveChangesButton")->isEnabled());
}

TEST_CASE("Save changes still normalizes a pasted endpoint",
          "[ui_controls][settings]") {
    Harness h;
    SettingsDialog dlg(h.h());
    SignalTally sig;
    sig.watch(dlg);

    check(dlg, "brazingEnabled")->setChecked(true);
    // Typed as the operator is told it: an address and a port, with the
    // surrounding whitespace a copy/paste carries.
    denso::testing::brazing_host(dlg)->setText(QStringLiteral(" 192.168.1.112 "));
    denso::testing::brazing_port(dlg)->setText(QStringLiteral(" 8080 "));
    btn(dlg, "saveChangesButton")->click();

    // Composed and canonicalized into the one stored value…
    CHECK(denso::brazing::load(h.h()).base_url == kUrlA);
    // …and the controls are re-seeded from what was STORED, so the operator is
    // never looking at something other than the truth.
    CHECK(denso::testing::brazing_host(dlg)->text() ==
          QStringLiteral("192.168.1.112"));
    CHECK(denso::testing::brazing_port(dlg)->text() == QStringLiteral("8080"));
    CHECK(sig.brazing == 1);   // …and the live reload is still requested
}

// ─────────────────────────────────────────────────────────────────────────────
// B. Backend status in the top bar
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("the top bar reports enabled reporting as ON", "[ui_controls][backend]") {
    Harness h;
    h.seed_camera();
    h.store_brazing(true, kUrlA);

    auto win = h.make_window();
    CHECK(win->displayed_brazing_status() == BrazingStatus::On);
    CHECK(btn(*win, "backendStatus")->text() == QStringLiteral("Backend: ON"));
    // The indicator shows the AUTHORITY, not its own idea of the config.
    CHECK(win->displayed_brazing_status() ==
          win->findChild<denso::ui::CameraView*>()->brazing_status());
}

TEST_CASE("the top bar reports disabled reporting as OFF", "[ui_controls][backend]") {
    Harness h;
    h.seed_camera();
    h.store_brazing(false, kUrlA);

    auto win = h.make_window();
    CHECK(win->displayed_brazing_status() == BrazingStatus::Off);
    CHECK(btn(*win, "backendStatus")->text() == QStringLiteral("Backend: OFF"));
}

TEST_CASE("an unusable base URL is never reported as ON", "[ui_controls][backend]") {
    // Enabled in the database, but the address is not a base URL, so no sender
    // exists. Reporting "ON" here would tell an operator readings are going out
    // when nothing is.
    Harness h;
    h.seed_camera();
    h.store_brazing(true, "http://192.168.1.112:8080/other/path");

    auto win = h.make_window();
    CHECK(win->displayed_brazing_status() == BrazingStatus::Off);
    CHECK(btn(*win, "backendStatus")->text() == QStringLiteral("Backend: OFF"));
}

TEST_CASE("enabling and disabling through Settings moves the indicator at once",
          "[ui_controls][backend]") {
    Harness h;
    h.seed_camera();
    h.store_brazing(false, "");

    auto win = h.make_window();
    REQUIRE(win->displayed_brazing_status() == BrazingStatus::Off);

    auto* dlg = win->findChild<SettingsDialog*>();
    REQUIRE(dlg != nullptr);

    // Enable through the REAL dialog: dirty → Save changes → persisted → signal →
    // MainWindow → CameraView → CameraGrid → indicator. No restart anywhere.
    check(*dlg, "brazingEnabled")->setChecked(true);
    denso::testing::set_brazing_base(*dlg, QString::fromLatin1(kUrlA));
    btn(*dlg, "saveChangesButton")->click();
    CHECK(win->displayed_brazing_status() == BrazingStatus::On);

    // …and back off again.
    check(*dlg, "brazingEnabled")->setChecked(false);
    btn(*dlg, "saveChangesButton")->click();
    CHECK(win->displayed_brazing_status() == BrazingStatus::Off);
    CHECK(btn(*win, "backendStatus")->text() == QStringLiteral("Backend: OFF"));
}

TEST_CASE("a mode switch drives the indicator to OFF", "[ui_controls][backend]") {
    Harness h;
    h.seed_camera();
    h.store_brazing(true, kUrlA);

    auto win = h.make_window();
    REQUIRE(win->displayed_brazing_status() == BrazingStatus::On);

    win->perform_switch(TargetMode::BallLeveler);

    CHECK_FALSE(denso::brazing::load(h.h()).enabled);   // the rule is unchanged
    CHECK(win->displayed_brazing_status() == BrazingStatus::Off);
    CHECK(btn(*win, "backendStatus")->text() == QStringLiteral("Backend: OFF"));
}

TEST_CASE("clicking the indicator opens Settings on the Server page",
          "[ui_controls][backend]") {
    Harness h;
    h.seed_camera();
    h.store_brazing(true, kUrlA);
    auto win = h.make_window();

    auto* dlg = win->findChild<SettingsDialog*>();
    REQUIRE(dlg != nullptr);
    const auto before = denso::brazing::load(h.h());

    btn(*win, "backendStatus")->click();
    spin(30);

    CHECK(dlg->isVisible());
    CHECK(dlg->findChild<QListWidget*>(QStringLiteral("navList"))->currentRow() == 4);
    // It OPENS settings; it must never toggle reporting. A stray touch on a
    // production panel cannot be allowed to stop delivery.
    const auto after = denso::brazing::load(h.h());
    CHECK(after.enabled == before.enabled);
    CHECK(after.base_url == before.base_url);
    CHECK(win->displayed_brazing_status() == BrazingStatus::On);
}

TEST_CASE("repeated saves create no duplicate indicator and no duplicate reports",
          "[ui_controls][backend]") {
    Harness h;
    h.seed_camera();
    h.store_brazing(true, kUrlA);
    auto win = h.make_window();

    auto* view = win->findChild<denso::ui::CameraView*>();
    REQUIRE(view != nullptr);
    int status_reports = 0;
    QObject::connect(view, &denso::ui::CameraView::brazing_status_changed,
                     [&](BrazingStatus) { ++status_reports; });

    auto* dlg = win->findChild<SettingsDialog*>();
    for (int i = 0; i < 3; ++i) {
        // Genuinely dirty each round — the address is retyped with the padding a
        // copy/paste carries, which canonicalizes back to the same base. So the
        // operator really does press Save three times, and the stored
        // configuration really is unchanged: exactly the case that must not
        // churn the sender.
        check(*dlg, "brazingEnabled")->setChecked(true);
        denso::testing::brazing_host(*dlg)->setText(
            QStringLiteral(" 192.168.1.112 "));
        denso::testing::brazing_port(*dlg)->setText(QStringLiteral("8080"));
        REQUIRE(dlg->is_dirty());
        REQUIRE(btn(*dlg, "saveChangesButton")->isEnabled());
        btn(*dlg, "saveChangesButton")->click();
        REQUIRE(denso::brazing::load(h.h()).base_url == kUrlA);
    }

    // ONE widget, however many times the operator saves…
    CHECK(win->findChildren<QPushButton*>(QStringLiteral("backendStatus")).size() == 1);
    // …and an unchanged configuration reports NO transition, so a listener never
    // has to de-duplicate.
    CHECK(status_reports == 0);
    CHECK(win->displayed_brazing_status() == BrazingStatus::On);
}

TEST_CASE("the indicator never claims the backend is connected",
          "[ui_controls][backend]") {
    // There is no health endpoint and no persistent connection — the backend
    // accepts POST /api/brazing/update and nothing else — so "enabled" is the
    // strongest true claim. Saying "Connected" would be an assertion about a
    // server nobody has asked anything.
    Harness h;
    h.seed_camera();
    h.store_brazing(true, kUrlA);
    auto win = h.make_window();

    auto* b = btn(*win, "backendStatus");
    CHECK(b->text() == QStringLiteral("Backend: ON"));
    CHECK_FALSE(b->text().contains(QStringLiteral("Connect"), Qt::CaseInsensitive));
    CHECK_FALSE(b->toolTip().contains(QStringLiteral("Connected"), Qt::CaseInsensitive));
    // The tooltip carries the state, the canonical address and what a click does…
    CHECK(b->toolTip().contains(QStringLiteral("enabled")));
    CHECK(b->toolTip().contains(QString::fromLatin1(kUrlA)));
    CHECK(b->toolTip().contains(QStringLiteral("Server settings")));
    // …and no credentials: normalize_base_url refuses userinfo outright, so the
    // canonical URL cannot carry any.
    CHECK_FALSE(b->toolTip().contains(QLatin1Char('@')));
}

namespace {

/// A transport whose outcome the test chooses, so the ERROR path is driven by a
/// real delivery result and never by a probe of our own.
struct ScriptedTransport : BrazingTransport {
    explicit ScriptedTransport(bool ok) : succeed(ok) {}
    void post(const std::map<int, ZoneValue>&, std::function<void(bool)> done) override {
        if (done) done(succeed);
    }
    bool succeed;
};

}  // namespace

TEST_CASE("a delivery outcome is announced, and is the only source of ERROR",
          "[ui_controls][backend]") {
    // The reporter reports what actually happened to a POST…
    int ok_count = 0;
    int fail_count = 0;
    {
        BrazingReporter failing(std::make_unique<ScriptedTransport>(false));
        QObject::connect(&failing, &BrazingReporter::delivery_failed, [&] { ++fail_count; });
        QObject::connect(&failing, &BrazingReporter::delivery_succeeded, [&] { ++ok_count; });
        failing.submit(std::map<int, ZoneValue>{{1, ZoneValue{42}}});
        CHECK(fail_count == 1);
        CHECK(ok_count == 0);
    }
    {
        BrazingReporter working(std::make_unique<ScriptedTransport>(true));
        QObject::connect(&working, &BrazingReporter::delivery_succeeded, [&] { ++ok_count; });
        QObject::connect(&working, &BrazingReporter::delivery_failed, [&] { ++fail_count; });
        working.submit(std::map<int, ZoneValue>{{1, ZoneValue{42}}});
        CHECK(ok_count == 1);
        CHECK(fail_count == 1);   // unchanged from the failing reporter above
    }

    // …and the grid maps exactly those two signals onto the indicator's ERROR/ON,
    // with no second opinion (no probe, no poll, no health request). Asserted on
    // the source because CameraGrid owns its sender privately — there is
    // deliberately no injection seam for a fake transport in production code.
    const QString src = read_source("/src/app/ui/camera/grid/camera_grid.cpp");
    CHECK(src.contains(QStringLiteral("&BrazingReporter::delivery_failed")));
    CHECK(src.contains(QStringLiteral("set_brazing_status(BrazingStatus::Error)")));
    CHECK(src.contains(QStringLiteral("&BrazingReporter::delivery_succeeded")));
    // …and the grid issues no request of its own to find out. Asserted on CODE,
    // not prose: the word appears in comments explaining what BrazingClient owns,
    // so only a construction or an include would be a real second network path.
    CHECK_FALSE(src.contains(QStringLiteral("new QNetworkAccessManager")));
    CHECK_FALSE(src.contains(QStringLiteral("#include <QNetwork")));
}

// ─────────────────────────────────────────────────────────────────────────────
// C. Refresh Cameras
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Refresh Cameras rebuilds the runtime exactly once",
          "[ui_controls][refresh]") {
    Harness h;
    h.seed_camera();
    auto win = h.make_window();

    const uint64_t reloads = win->camera_view_grid_reload_invocations();
    const uint64_t gen = win->camera_view_grid_generation();
    const size_t streams_before = win->camera_view_stream_count();

    btn(*win, "refreshCamerasButton")->click();
    spin(50);

    CHECK(win->camera_view_grid_reload_invocations() == reloads + 1);
    // The generation advanced, which is precisely what makes every callback the
    // retired workers captured fail callback_is_current() and be dropped.
    CHECK(win->camera_view_grid_generation() > gen);
    // Rebuilt, not added to: one camera still means one runtime.
    CHECK(win->camera_view_stream_count() == streams_before);
    CHECK(win->camera_view_stream_count() == 1);
}

TEST_CASE("Refresh Cameras preserves every piece of persisted configuration",
          "[ui_controls][refresh]") {
    // The whole contract in one case: a refresh is a RUNTIME reload, so nothing
    // it touches may reach the database. Each field below is one the operator
    // would have to redo by hand if a refresh behaved like a reset.
    Harness h;
    const int64_t cam_id = h.seed_camera();
    h.store_brazing(true, kUrlA);
    REQUIRE(denso::mode::save(h.h(), TargetMode::DigitReader));

    // A zoned, decimal-formatted area and a ball calibration row, written
    // directly so the case does not depend on a wizard.
    QSqlQuery q(h.h());
    q.prepare(QStringLiteral(
        "INSERT INTO camera_area (camera_id, name, points, zone, decimal_places) "
        "VALUES (?, 'Zone 1', '[[0.1,0.1],[0.9,0.1],[0.9,0.9],[0.1,0.9]]', 3, 2)"));
    q.addBindValue(static_cast<qlonglong>(cam_id));
    REQUIRE(q.exec());

    QSqlQuery lvl(h.h());
    lvl.prepare(QStringLiteral(
        "INSERT INTO ball_level_zone (camera_id, zone_no, conf, rect_x, rect_y, "
        "rect_w, rect_h, y_100, y_0, hold_ms) "
        "VALUES (?, 1, 0.5, 0.1, 0.1, 0.4, 0.6, 0.15, 0.65, 3000)"));
    lvl.addBindValue(static_cast<qlonglong>(cam_id));
    REQUIRE(lvl.exec());

    const auto snapshot = [&](const QString& sql) {
        QSqlQuery s(h.h());
        REQUIRE(s.exec(sql));
        QStringList rows;
        while (s.next()) {
            QStringList cols;
            for (int i = 0; i < s.record().count(); ++i) cols << s.value(i).toString();
            rows << cols.join(QLatin1Char('|'));
        }
        return rows.join(QLatin1Char('\n'));
    };

    const QString cameras_before = snapshot(QStringLiteral(
        "SELECT id, name, rtsp, active, setup_complete, areas_need_review "
        "FROM camera ORDER BY id"));
    const QString areas_before = snapshot(QStringLiteral(
        "SELECT camera_id, name, points, zone, decimal_places FROM camera_area "
        "ORDER BY id"));
    const QString models_before = snapshot(QStringLiteral(
        "SELECT camera_id, model_id FROM camera_model ORDER BY camera_id, model_id"));
    const QString level_before = snapshot(QStringLiteral(
        "SELECT camera_id, zone_no, y_100, y_0, hold_ms FROM ball_level_zone "
        "ORDER BY camera_id, zone_no"));
    REQUIRE_FALSE(level_before.isEmpty());   // the assertion below must have data
    const auto mode_before = denso::mode::load(h.h());
    const auto brazing_before = denso::brazing::load(h.h());

    auto win = h.make_window();
    btn(*win, "refreshCamerasButton")->click();
    spin(50);

    CHECK(snapshot(QStringLiteral(
              "SELECT id, name, rtsp, active, setup_complete, areas_need_review "
              "FROM camera ORDER BY id")) == cameras_before);
    CHECK(snapshot(QStringLiteral(
              "SELECT camera_id, name, points, zone, decimal_places FROM camera_area "
              "ORDER BY id")) == areas_before);
    CHECK(snapshot(QStringLiteral("SELECT camera_id, model_id FROM camera_model "
                                  "ORDER BY camera_id, model_id")) == models_before);
    CHECK(snapshot(QStringLiteral(
              "SELECT camera_id, zone_no, y_100, y_0, hold_ms FROM ball_level_zone "
              "ORDER BY camera_id, zone_no")) == level_before);
    CHECK(denso::mode::load(h.h()) == mode_before);
    CHECK(denso::brazing::load(h.h()).enabled == brazing_before.enabled);
    CHECK(denso::brazing::load(h.h()).base_url == brazing_before.base_url);
    // A mode reset clears setup_complete; this obviously did not run one.
    CHECK(denso::camera::runtime(h.h()).size() == 1);
}

TEST_CASE("Refresh Cameras leaves backend reporting configured and single",
          "[ui_controls][refresh]") {
    Harness h;
    h.seed_camera();
    h.store_brazing(true, kUrlA);
    auto win = h.make_window();
    REQUIRE(win->displayed_brazing_status() == BrazingStatus::On);

    btn(*win, "refreshCamerasButton")->click();
    spin(50);

    // Reporting comes back up on the SAME configuration, through the same single
    // sender the grid owns — a duplicate would be a second stream of POSTs.
    CHECK(win->displayed_brazing_status() == BrazingStatus::On);
    CHECK(denso::brazing::load(h.h()).enabled);
    CHECK(denso::brazing::load(h.h()).base_url == kUrlA);
    CHECK(win->findChild<denso::ui::CameraView*>()->active_brazing_base_url() == kUrlA);
}

TEST_CASE("Refresh Cameras refuses to run twice at once", "[ui_controls][refresh]") {
    Harness h;
    h.seed_camera();
    auto win = h.make_window();
    const uint64_t reloads = win->camera_view_grid_reload_invocations();

    auto* b = btn(*win, "refreshCamerasButton");
    b->click();
    // The rebuild is deferred one tick, so this is the real window in which a
    // double-click or an impatient second press would land.
    CHECK_FALSE(b->isEnabled());
    CHECK(b->text() == QStringLiteral("Refreshing cameras…"));
    win->refresh_cameras();   // the same entry point the button uses
    b->click();

    spin(50);

    CHECK(win->camera_view_grid_reload_invocations() == reloads + 1);  // exactly one
    CHECK(b->isEnabled());                                             // released again
    CHECK(b->text() == QStringLiteral("Refresh Cameras"));
}

TEST_CASE("the four-tile cap is not reported as a failure", "[ui_controls][refresh]") {
    // Five configured cameras, four tiles. The cap is DELIBERATE, so calling it
    // "4 of 5 cameras started" would train an operator to ignore the one message
    // that is supposed to mean something. The count therefore comes from what the
    // grid ADMITTED, not from counting database rows out here.
    Harness h;
    for (int i = 0; i < 5; ++i) {
        h.seed_camera("Line " + std::to_string(i));
    }
    REQUIRE(denso::camera::runtime(h.h()).size() == 5);

    auto win = h.make_window();
    btn(*win, "refreshCamerasButton")->click();
    spin(60);

    CHECK(win->camera_view_stream_count() == 4);   // the cap held…
    CHECK(label(*win, "refreshStatus")->text().isEmpty());   // …and said nothing
}

TEST_CASE("a camera that builds no runtime is surfaced, not swallowed",
          "[ui_controls][refresh]") {
    // A camera bound to a model the compatibility policy rejects builds no
    // runtime at all (CameraGrid::start_one refuses it). Its sibling keeps
    // running, the application stays up, and the shortfall is stated rather than
    // left for the operator to notice.
    Harness h;
    const int64_t bad = h.seed_camera("Rejected");
    h.seed_camera("Healthy");

    // A catalog row pointing at a model with no manifest entry and no file on
    // disk: nothing can vouch for it, so the policy refuses it.
    QSqlQuery q(h.h());
    REQUIRE(q.exec(QStringLiteral(
        "INSERT INTO model (name, filename, class_names) "
        "VALUES ('ghost', 'ghost.engine', '[\"0\"]')")));
    const qlonglong model_id = q.lastInsertId().toLongLong();
    q.prepare(QStringLiteral(
        "INSERT INTO camera_model (camera_id, model_id) VALUES (?, ?)"));
    q.addBindValue(static_cast<qlonglong>(bad));
    q.addBindValue(model_id);
    REQUIRE(q.exec());

    auto win = h.make_window();
    btn(*win, "refreshCamerasButton")->click();
    spin(60);

    auto* status = label(*win, "refreshStatus");
    REQUIRE(status != nullptr);
    // isVisibleTo(), not isVisible(): this window is never shown in a test, so
    // isVisible() is false for every widget in it and would prove nothing.
    CHECK(status->isVisibleTo(win.get()));
    CHECK(status->text() == QStringLiteral("1 of 2 cameras started"));
    // The sibling is running and the application is still standing.
    CHECK(win->camera_view_stream_count() == 1);
    CHECK(btn(*win, "refreshCamerasButton")->isEnabled());
}

TEST_CASE("a clean refresh reports nothing", "[ui_controls][refresh]") {
    // The counterpart to the case above: a refresh that brought everything up must
    // stay silent, or the message would be noise an operator learns to ignore.
    Harness h;
    h.seed_camera();
    auto win = h.make_window();

    btn(*win, "refreshCamerasButton")->click();
    spin(50);

    CHECK(label(*win, "refreshStatus")->text().isEmpty());
}

TEST_CASE("Refresh Cameras keeps the pending Backend delivery alive",
          "[ui_controls][refresh]") {
    // A camera rebuild is not a mode switch. The sender — and with it any snapshot
    // the server has not acked and the backoff behind it — must survive, so
    // pressing Refresh cannot silently drop a reading that was still being
    // retried. Only teardown() (the mode switch) retires it.
    Harness h;
    h.seed_camera();
    h.store_brazing(true, kUrlA);
    auto win = h.make_window();

    auto* view = win->findChild<denso::ui::CameraView*>();
    REQUIRE(view != nullptr);
    REQUIRE(win->displayed_brazing_status() == BrazingStatus::On);

    int status_reports = 0;
    QObject::connect(view, &denso::ui::CameraView::brazing_status_changed,
                     [&](BrazingStatus) { ++status_reports; });

    btn(*win, "refreshCamerasButton")->click();
    spin(50);

    // The SAME sender, on the same address: it was never destroyed, so it never
    // reported a transition and never discarded what it was holding.
    CHECK(status_reports == 0);
    CHECK(win->displayed_brazing_status() == BrazingStatus::On);
    CHECK(view->active_brazing_base_url() == kUrlA);
}

TEST_CASE("a mode switch still retires the sender", "[ui_controls][refresh]") {
    // The other half of the rule above: what a refresh must NOT do, a switch MUST.
    // The old mode's readings may never reach the server afterwards.
    Harness h;
    h.seed_camera();
    h.store_brazing(true, kUrlA);
    auto win = h.make_window();
    REQUIRE(win->displayed_brazing_status() == BrazingStatus::On);

    win->perform_switch(TargetMode::BallLeveler);

    CHECK(win->displayed_brazing_status() == BrazingStatus::Off);
    CHECK(win->findChild<denso::ui::CameraView*>()->active_brazing_base_url().empty());
}

// ── Dismissal paths and write-failure atomicity ──────────────────────────────

TEST_CASE("Esc and the window manager undo the theme preview too",
          "[ui_controls][settings]") {
    // Wiring only the Cancel BUTTON would leave every other way out of the dialog
    // — Esc, the header's close glyph, the window manager's close box — with the
    // preview still applied and nothing persisted: the running app disagreeing
    // with the database. They all end in QDialog::reject(), which is why the
    // restoration lives in the override.
    Harness h;
    SettingsDialog dlg(h.h());
    dlg.set_theme_dark(true);
    SignalTally sig;
    sig.watch(dlg);

    check(dlg, "darkModeSwitch")->setChecked(false);   // previews light
    REQUIRE(sig.theme_preview == 1);
    REQUIRE(sig.last_preview_dark == false);

    dlg.reject();   // exactly what Esc and the close box do

    CHECK(sig.theme_preview == 2);
    CHECK(sig.last_preview_dark == true);      // restored to the entry theme
    CHECK(sig.theme_commit == 0);              // …and nothing was committed
    // The widget follows, so re-opening cannot show a toggle that disagrees with
    // the theme now on screen.
    CHECK(check(dlg, "darkModeSwitch")->isChecked());
}

TEST_CASE("a failed theme write applies nothing at all", "[ui_controls][settings]") {
    // Save persists EVERYTHING before it applies ANYTHING. A theme write that
    // fails must therefore leave the running application untouched — no backend
    // reload, no theme change, no display transaction — and keep the dialog open
    // with the reason.
    Harness h;
    SettingsDialog dlg(h.h());
    SignalTally sig;
    sig.watch(dlg);
    dlg.set_theme_committer([](bool) { return false; });   // the write fails

    check(dlg, "brazingEnabled")->setChecked(true);
    denso::testing::set_brazing_base(dlg, QString::fromLatin1(kUrlA));
    btn(dlg, "saveChangesButton")->click();

    CHECK(sig.applies() == 0);
    CHECK(dlg.result() != QDialog::Accepted);
    CHECK_FALSE(label(dlg, "brazingStatus")->text().isEmpty());
}

TEST_CASE("the theme is persisted before it is applied", "[ui_controls][settings]") {
    // Ordering, asserted directly: the committer must have run before the apply
    // signal fires. The old dialog did the reverse — the toggle persisted itself
    // the instant it moved, which is what made Cancel meaningless.
    Harness h;
    SettingsDialog dlg(h.h());
    std::vector<QString> order;
    dlg.set_theme_committer([&](bool) {
        order.push_back(QStringLiteral("persist"));
        return true;
    });
    QObject::connect(&dlg, &SettingsDialog::theme_changed,
                     [&](bool) { order.push_back(QStringLiteral("apply")); });
    QObject::connect(&dlg, &SettingsDialog::theme_preview_requested,
                     [&](bool) { order.push_back(QStringLiteral("preview")); });

    check(dlg, "darkModeSwitch")->setChecked(!check(dlg, "darkModeSwitch")->isChecked());
    btn(dlg, "saveChangesButton")->click();

    REQUIRE(order.size() == 3);
    CHECK(order[0] == QStringLiteral("preview"));   // live feedback, no write
    CHECK(order[1] == QStringLiteral("persist"));   // …then the write…
    CHECK(order[2] == QStringLiteral("apply"));     // …then the commit apply
}

TEST_CASE("a failed Save rolls the whole form back, not just the failing page",
          "[ui_controls][settings]") {
    // The two pages are persisted in ONE transaction. Without it, the Server page
    // would already be on disk when the display write failed — the operator is
    // told the Save failed, and a restart comes up half-configured with reporting
    // pointed somewhere they never confirmed.
    Harness h;
    h.store_brazing(false, "");
    SettingsDialog dlg(h.h());
    SignalTally sig;
    sig.watch(dlg);
    dlg.set_theme_committer([](bool) { return false; });   // the SECOND write fails

    check(dlg, "brazingEnabled")->setChecked(true);
    denso::testing::set_brazing_base(dlg, QString::fromLatin1(kUrlB));
    btn(dlg, "saveChangesButton")->click();

    // The FIRST write was rolled back with it: nothing reached the database…
    const auto stored = denso::brazing::load(h.h());
    CHECK_FALSE(stored.enabled);
    CHECK(stored.base_url.empty());
    // …nothing was applied, and the dialog is still open saying so.
    CHECK(sig.applies() == 0);
    CHECK(dlg.result() != QDialog::Accepted);
    CHECK_FALSE(label(dlg, "brazingStatus")->text().isEmpty());
}

// ─────────────────────────────────────────────────────────────────────────────
// D. Settings <-> authoritative Backend configuration
//
// The Switch button lives on the Settings dialog's own Mode page, so a mode
// switch happens with the dialog STILL VISIBLE. showEvent therefore never fires
// again, and the Server checkbox used to sit there ticked while the database,
// the grid and the top bar all said OFF. These pin the checkbox as an editor
// VIEW of the authoritative configuration rather than a state holder of its own.
// ─────────────────────────────────────────────────────────────────────────────

namespace {

/// The Settings dialog MainWindow owns, plus its Server-page widgets.
struct ServerPage {
    SettingsDialog* dlg = nullptr;
    QCheckBox* enabled = nullptr;
    QLineEdit* host = nullptr;
    QLineEdit* port = nullptr;
    QPushButton* save = nullptr;

    explicit ServerPage(MainWindow& w) {
        dlg = w.findChild<SettingsDialog*>();
        REQUIRE(dlg != nullptr);
        enabled = check(*dlg, "brazingEnabled");
        host = denso::testing::brazing_host(*dlg);
        port = denso::testing::brazing_port(*dlg);
        save = btn(*dlg, "saveChangesButton");
        REQUIRE(enabled != nullptr);
        REQUIRE(save != nullptr);
    }

    /// Put a whole base URL into the three controls, and read them back as one.
    /// The fixture's callers are about backend SYNC — that the address survives a
    /// mode switch, a refresh, a cancel — not about the decomposition itself,
    /// which has its own suite. Keeping them written in terms of a base URL is
    /// what makes them still say what they are for.
    void set_base(const QString& base_url) {
        denso::testing::set_brazing_base(*dlg, base_url);
    }
    QString base() const { return denso::testing::brazing_base_text(*dlg); }
};

}  // namespace

TEST_CASE("stored reporting shows a ticked Server checkbox", "[ui_controls][sync]") {
    Harness h;
    h.seed_camera();
    h.store_brazing(true, kUrlA);

    auto win = h.make_window();
    ServerPage p(*win);
    p.dlg->show();

    CHECK(p.enabled->isChecked());
    CHECK(p.base() == QString::fromLatin1(kUrlA));
    CHECK(win->displayed_brazing_status() == BrazingStatus::On);
}

TEST_CASE("a mode switch unticks the checkbox of an OPEN dialog",
          "[ui_controls][sync]") {
    // THE BUG. The operator is standing in Settings — that is where Switch is —
    // so the dialog is visible throughout, showEvent cannot re-fire, and nothing
    // used to push the new configuration into the page.
    Harness h;
    h.seed_camera();
    h.store_brazing(true, kUrlA);

    auto win = h.make_window();
    ServerPage p(*win);
    p.dlg->show();
    REQUIRE(p.enabled->isChecked());
    REQUIRE(win->displayed_brazing_status() == BrazingStatus::On);

    win->perform_switch(TargetMode::BallLeveler);

    // All four now agree, which is the whole point.
    CHECK_FALSE(denso::brazing::load(h.h()).enabled);              // the database…
    CHECK(win->displayed_brazing_status() == BrazingStatus::Off);  // …the top bar…
    CHECK_FALSE(p.enabled->isChecked());                           // …the checkbox.
    // The dialog was open for the whole switch, and a CLEAN commit now dismisses
    // it (section E). The checkbox above was therefore corrected while it was
    // still on screen and has NOT been shown again since — so nothing but the
    // passive re-sync can have unticked it.
    CHECK_FALSE(p.dlg->isVisible());
    // The ADDRESS is preserved by the switch on purpose, so the operator does not
    // have to retype it to re-enable.
    CHECK(denso::brazing::load(h.h()).base_url == kUrlA);
    CHECK(p.base() == QString::fromLatin1(kUrlA));
}

TEST_CASE("the passive re-sync writes nothing and asks for nothing",
          "[ui_controls][sync]") {
    // It is a READER. Emitting brazing_config_changed would make a redisplay look
    // like an operator action and drive a second reconfiguration of a runtime that
    // has already followed the switch through its own path; writing would make the
    // dialog a second authority over the very rows it is displaying.
    Harness h;
    h.seed_camera();
    h.store_brazing(true, kUrlA);

    auto win = h.make_window();
    ServerPage p(*win);
    p.dlg->show();

    int reconfigures = 0;
    QObject::connect(p.dlg, &SettingsDialog::brazing_config_changed,
                     [&] { ++reconfigures; });

    // Everything the switch itself legitimately writes, captured first so the
    // comparison isolates what the RE-SYNC did.
    win->perform_switch(TargetMode::BallLeveler);
    const auto after_switch = denso::brazing::load(h.h());
    reconfigures = 0;   // ignore anything the switch itself may have caused

    p.dlg->show();                       // the clean commit closed it (section E)
    p.dlg->refresh_backend_state();      // the passive path, on its own
    p.dlg->refresh_backend_state();      // …and again: still inert

    CHECK(reconfigures == 0);
    const auto after_sync = denso::brazing::load(h.h());
    CHECK(after_sync.enabled == after_switch.enabled);
    CHECK(after_sync.base_url == after_switch.base_url);
}

TEST_CASE("the passive re-sync never arms Save changes", "[ui_controls][sync]") {
    // An authoritative refresh must not masquerade as an unsaved edit: the
    // operator changed nothing, so there is nothing to save.
    Harness h;
    h.seed_camera();
    h.store_brazing(true, kUrlA);

    auto win = h.make_window();
    ServerPage p(*win);
    p.dlg->show();
    REQUIRE_FALSE(p.dlg->is_dirty());
    REQUIRE_FALSE(p.save->isEnabled());

    win->perform_switch(TargetMode::BallLeveler);
    p.dlg->show();   // the clean commit closed it (section E); reopen to inspect

    CHECK_FALSE(p.enabled->isChecked());
    CHECK_FALSE(p.dlg->is_dirty());
    CHECK_FALSE(p.save->isEnabled());
}

TEST_CASE("reopening Settings after a mode switch shows the new state",
          "[ui_controls][sync]") {
    // The other route to the same page: closed during the switch, opened after.
    Harness h;
    h.seed_camera();
    h.store_brazing(true, kUrlA);

    auto win = h.make_window();
    ServerPage p(*win);
    p.dlg->show();
    p.dlg->reject();                       // closed before the switch

    win->perform_switch(TargetMode::BallLeveler);
    p.dlg->show();                         // …and reopened after it

    CHECK_FALSE(p.enabled->isChecked());
    CHECK(p.base() == QString::fromLatin1(kUrlA));
    CHECK_FALSE(p.save->isEnabled());
}

TEST_CASE("repeated open/close cycles never restore the stale tick",
          "[ui_controls][sync]") {
    // The dialog is created once and reused for the life of the process, so a
    // cached widget value would resurface on some later visit rather than the
    // first. Cycle it.
    Harness h;
    h.seed_camera();
    h.store_brazing(true, kUrlA);

    auto win = h.make_window();
    ServerPage p(*win);
    win->perform_switch(TargetMode::BallLeveler);

    for (int i = 0; i < 4; ++i) {
        p.dlg->show();
        p.dlg->select_server_page();
        CHECK_FALSE(p.enabled->isChecked());
        CHECK(p.base() == QString::fromLatin1(kUrlA));
        CHECK_FALSE(p.save->isEnabled());
        p.dlg->reject();
    }
}

TEST_CASE("the Backend indicator opens Server showing the unticked box",
          "[ui_controls][sync]") {
    Harness h;
    h.seed_camera();
    h.store_brazing(true, kUrlA);

    auto win = h.make_window();
    ServerPage p(*win);
    win->perform_switch(TargetMode::BallLeveler);
    REQUIRE(btn(*win, "backendStatus")->text() == QStringLiteral("Backend: OFF"));

    btn(*win, "backendStatus")->click();
    spin(30);

    CHECK(p.dlg->isVisible());
    CHECK(p.dlg->findChild<QListWidget*>(QStringLiteral("navList"))->currentRow() == 4);
    CHECK_FALSE(p.enabled->isChecked());
    CHECK(p.base() == QString::fromLatin1(kUrlA));
}

TEST_CASE("re-enabling after a mode switch takes effect at once",
          "[ui_controls][sync]") {
    // The operator's recovery path: tick the box the switch cleared, Save, and
    // reporting is running again — no restart, and the address never retyped.
    Harness h;
    h.seed_camera();
    h.store_brazing(true, kUrlA);

    auto win = h.make_window();
    ServerPage p(*win);
    p.dlg->show();
    win->perform_switch(TargetMode::BallLeveler);
    REQUIRE_FALSE(p.enabled->isChecked());
    REQUIRE(win->displayed_brazing_status() == BrazingStatus::Off);

    win->perform_switch(TargetMode::DigitReader);
    p.dlg->show();   // each clean commit closed it (section E)
    REQUIRE_FALSE(p.enabled->isChecked());   // still off after the second switch

    // The switch is DESTRUCTIVE — it clears setup_complete — so no camera is
    // runtime-eligible until the operator re-runs the wizard. Stand in for that
    // here, then rebuild the runtime the way the Refresh button does: without a
    // pipeline there is nothing for a sender to attach to, and the indicator
    // would (correctly) stay OFF however the box is ticked.
    QSqlQuery reconfigure(h.h());
    REQUIRE(reconfigure.exec(QStringLiteral("UPDATE camera SET setup_complete = 1")));
    win->refresh_cameras();
    spin(50);
    REQUIRE(win->camera_view_stream_count() == 1);

    p.enabled->setChecked(true);
    CHECK(p.dlg->is_dirty());                // an operator edit DOES arm Save
    CHECK(p.save->isEnabled());
    p.save->click();

    CHECK(p.enabled->isChecked());           // the tick stays
    CHECK(denso::brazing::load(h.h()).enabled);
    CHECK(denso::brazing::load(h.h()).base_url == kUrlA);   // never retyped
    // …and delivery is running again, with no restart anywhere in this case.
    CHECK(win->displayed_brazing_status() == BrazingStatus::On);
}

TEST_CASE("Refresh Cameras leaves the checkbox alone", "[ui_controls][sync]") {
    // A camera rebuild changes no configuration, so it must not touch the editor
    // view of that configuration either.
    Harness h;
    h.seed_camera();
    h.store_brazing(true, kUrlA);

    auto win = h.make_window();
    ServerPage p(*win);
    p.dlg->show();
    REQUIRE(p.enabled->isChecked());

    btn(*win, "refreshCamerasButton")->click();
    spin(50);

    CHECK(p.enabled->isChecked());
    CHECK(p.base() == QString::fromLatin1(kUrlA));
    CHECK_FALSE(p.dlg->is_dirty());
    CHECK(win->displayed_brazing_status() == BrazingStatus::On);
}

TEST_CASE("a delivery failure does not untick the checkbox", "[ui_controls][sync]") {
    // ERROR is a DELIVERY fact; `enabled` is a CONFIGURATION fact. Conflating
    // them would tell the operator they had turned reporting off when all that
    // happened was one POST failing — and the checkbox is what they would then
    // "fix" by saving, silently rewriting the config.
    Harness h;
    h.seed_camera();
    h.store_brazing(true, kUrlA);

    auto win = h.make_window();
    ServerPage p(*win);
    p.dlg->show();
    REQUIRE(p.enabled->isChecked());

    // Drive the indicator to ERROR the way a real failed POST would.
    auto* view = win->findChild<denso::ui::CameraView*>();
    REQUIRE(view != nullptr);
    emit view->brazing_status_changed(BrazingStatus::Error);

    CHECK(win->displayed_brazing_status() == BrazingStatus::Error);
    CHECK(btn(*win, "backendStatus")->text() == QStringLiteral("Backend: ERROR"));
    CHECK(p.enabled->isChecked());          // configuration is untouched
    CHECK(denso::brazing::load(h.h()).enabled);
    CHECK_FALSE(p.dlg->is_dirty());
}

TEST_CASE("only a real configuration change moves the checkbox",
          "[ui_controls][sync]") {
    // The negative space around the fix: everything that is NOT a mode switch or
    // a Save must leave the tick where it is. Without this, "re-sync on anything
    // that might have changed" would be an easy and wrong generalisation.
    Harness h;
    h.seed_camera();
    h.store_brazing(true, kUrlA);

    auto win = h.make_window();
    ServerPage p(*win);
    p.dlg->show();
    REQUIRE(p.enabled->isChecked());

    btn(*win, "refreshCamerasButton")->click();   // camera rebuild
    spin(50);
    win->refresh_cameras();                       // …and again
    spin(50);
    auto* view = win->findChild<denso::ui::CameraView*>();
    emit view->brazing_status_changed(BrazingStatus::Error);   // delivery fault
    emit view->brazing_status_changed(BrazingStatus::On);      // …and recovery
    p.dlg->select_server_page();                  // navigation
    CHECK(p.enabled->isChecked());

    // A mode switch DOES move it…
    win->perform_switch(TargetMode::BallLeveler);
    CHECK_FALSE(p.enabled->isChecked());

    // …and so does a Save.
    win->perform_switch(TargetMode::DigitReader);
    p.enabled->setChecked(true);
    p.save->click();
    CHECK(p.enabled->isChecked());
    CHECK(denso::brazing::load(h.h()).enabled);
}

TEST_CASE("the re-sync retires the Server page's own unsaved edit",
          "[ui_controls][sync]") {
    // The load-bearing case for making dirty a COMPARISON rather than a flag.
    // The operator ticks the box, then switches mode. The switch overwrites that
    // tick, so the edit no longer exists — and "Save changes" must not stay armed
    // for a form that already matches what is stored. Pressing it would run the
    // whole persist-and-apply sequence, up to and including a display
    // confirm/revert, for no change at all.
    Harness h;
    h.seed_camera();
    h.store_brazing(false, kUrlA);

    auto win = h.make_window();
    ServerPage p(*win);
    p.dlg->show();
    REQUIRE_FALSE(p.enabled->isChecked());

    p.enabled->setChecked(true);          // the ONLY edit on the form
    REQUIRE(p.dlg->is_dirty());
    REQUIRE(p.save->isEnabled());

    // The passive path driven DIRECTLY, against a configuration that changed
    // underneath the dialog. Going through perform_switch would also close the
    // dialog (section E), which discards everything — so the assertion would
    // pass without proving anything about the rebase.
    h.store_brazing(false, kUrlA);
    p.dlg->refresh_backend_state();

    CHECK_FALSE(p.enabled->isChecked());  // the authority overwrote the tick…
    CHECK_FALSE(p.dlg->is_dirty());       // …so there is nothing left to save
    CHECK_FALSE(p.save->isEnabled());
}

TEST_CASE("the re-sync keeps an unsaved edit on another page armed",
          "[ui_controls][sync]") {
    // The other half of the same rule: rebasing the SERVER page must not discard
    // an unrelated pending edit. A blanket `dirty_ = false` would silently throw
    // the operator's display change away.
    Harness h;
    h.seed_camera();
    h.store_brazing(true, kUrlA);

    auto win = h.make_window();
    ServerPage p(*win);
    p.dlg->show();
    auto* dark = check(*p.dlg, "darkModeSwitch");
    REQUIRE(dark != nullptr);

    dark->setChecked(!dark->isChecked());   // an edit on the DISPLAY page
    const bool staged_dark = dark->isChecked();
    REQUIRE(p.dlg->is_dirty());

    // Again the passive path directly: a committed switch would close the dialog
    // and discard the display edit along with everything else, so only the
    // isolated re-sync can show that the rebase is SELECTIVE.
    h.store_brazing(false, kUrlA);
    p.dlg->refresh_backend_state();

    CHECK_FALSE(p.enabled->isChecked());       // the Server page followed…
    CHECK(dark->isChecked() == staged_dark);   // …the display edit is untouched…
    CHECK(p.dlg->is_dirty());                  // …and still saveable.
    CHECK(p.save->isEnabled());
}

TEST_CASE("returning a field to its stored value disarms Save changes",
          "[ui_controls][sync]") {
    // Dirty is "does the form differ from what is stored", so an edit the
    // operator undoes by hand must disarm the action just as the re-sync does.
    // A sticky flag could not express this.
    Harness h;
    h.seed_camera();
    h.store_brazing(true, kUrlA);

    auto win = h.make_window();
    ServerPage p(*win);
    p.dlg->show();
    REQUIRE_FALSE(p.dlg->is_dirty());

    p.enabled->setChecked(false);
    CHECK(p.dlg->is_dirty());
    CHECK(p.save->isEnabled());

    p.enabled->setChecked(true);      // …changed their mind
    CHECK_FALSE(p.dlg->is_dirty());
    CHECK_FALSE(p.save->isEnabled());

    // Same for the address field, including the whitespace that a paste carries.
    p.set_base(QString::fromLatin1(kUrlB));
    CHECK(p.dlg->is_dirty());
    p.set_base(QString::fromLatin1(kUrlA));
    CHECK_FALSE(p.dlg->is_dirty());
}

// ─────────────────────────────────────────────────────────────────────────────
// E. Settings closes itself after a committed mode switch
//
// The Switch button is on this dialog's own Mode page, so after a switch the
// operator is left staring at a form that describes the appliance as it was
// before a destructive reset. A COMMIT returns them to the main screen — any
// commit, including one the window could not finish updating, because the form is
// equally invalid either way and this dialog is application-modal in front of a
// non-modal warning. Nothing else closes it: a cancel, a refusal, a rollback and
// an unresolved transaction all leave it exactly where it was.
// ─────────────────────────────────────────────────────────────────────────────

namespace {

/// Everything Save/apply-shaped the dialog can emit, so "closing emitted nothing"
/// is a claim about the WHOLE set rather than the one signal a case remembered.
struct SwitchTally {
    int switch_requested = 0;
    int brazing = 0;
    int theme_commit = 0;
    int theme_preview = 0;
    bool last_preview_dark = false;
    int display = 0;
    int finished = 0;

    void watch(SettingsDialog& d) {
        QObject::connect(&d, &SettingsDialog::switch_mode_requested,
                         [this](int) { ++switch_requested; });
        QObject::connect(&d, &SettingsDialog::brazing_config_changed,
                         [this] { ++brazing; });
        QObject::connect(&d, &SettingsDialog::theme_changed,
                         [this](bool) { ++theme_commit; });
        QObject::connect(&d, &SettingsDialog::theme_preview_requested,
                         [this](bool dark) { ++theme_preview; last_preview_dark = dark; });
        QObject::connect(&d, &SettingsDialog::apply_display_requested,
                         [this](int, int, int) { ++display; });
        QObject::connect(&d, &QDialog::finished, [this](int) { ++finished; });
    }
    int applies() const { return brazing + theme_commit + display; }
};

/// Arm a one-shot that rejects whatever modal is on screen when it fires. This is
/// how the REAL confirm-and-cancel path is driven: ModeConfirmDialog::exec()
/// spins its own event loop, so the answer has to come from inside it. No desktop
/// interaction — a queued call into the running loop.
void reject_next_modal() {
    QTimer::singleShot(0, [] {
        if (auto* modal = qobject_cast<QDialog*>(QApplication::activeModalWidget())) {
            modal->reject();
        }
    });
}

}  // namespace

TEST_CASE("a committed mode switch closes Settings", "[ui_controls][close]") {
    Harness h;
    h.seed_camera();
    h.store_brazing(true, kUrlA);

    auto win = h.make_window();
    ServerPage p(*win);
    p.dlg->show();
    REQUIRE(p.dlg->isVisible());

    win->perform_switch(TargetMode::BallLeveler);

    CHECK_FALSE(p.dlg->isVisible());
    CHECK(win->current_mode() == TargetMode::BallLeveler);
    CHECK(win->last_switch_error().isEmpty());
}

TEST_CASE("cancelling the confirmation leaves Settings open",
          "[ui_controls][close]") {
    // The real operator path: Switch is pressed, the confirmation appears, and
    // they think better of it. Nothing may change and the dialog must stay put.
    Harness h;
    h.seed_camera();
    h.store_brazing(true, kUrlA);

    auto win = h.make_window();
    ServerPage p(*win);
    p.dlg->show();
    const uint64_t reloads = win->camera_view_grid_reload_invocations();

    reject_next_modal();
    win->on_switch_mode(static_cast<int>(TargetMode::BallLeveler));
    spin(50);

    CHECK(p.dlg->isVisible());                              // still there
    CHECK(win->current_mode() == TargetMode::DigitReader);  // nothing switched
    CHECK(denso::brazing::load(h.h()).enabled);             // …and nothing reset
    CHECK(p.enabled->isChecked());
    CHECK(win->camera_view_grid_reload_invocations() == reloads);
}

TEST_CASE("a rolled-back switch leaves Settings open", "[ui_controls][close]") {
    // The transaction is made to fail deterministically: a trigger aborts the
    // camera UPDATE that switch_and_reset performs, so the whole thing rolls back.
    // Nothing changed, so the operator stays on the form they were using.
    Harness h;
    h.seed_camera();
    h.store_brazing(true, kUrlA);

    QSqlQuery q(h.h());
    REQUIRE(q.exec(QStringLiteral(
        "CREATE TRIGGER block_switch BEFORE UPDATE ON camera "
        "BEGIN SELECT RAISE(ABORT, 'blocked'); END")));

    auto win = h.make_window();
    ServerPage p(*win);
    p.dlg->show();
    SwitchTally sig;
    sig.watch(*p.dlg);

    win->perform_switch(TargetMode::BallLeveler);

    CHECK(p.dlg->isVisible());                              // NOT dismissed
    CHECK(sig.finished == 0);
    CHECK_FALSE(win->last_switch_error().isEmpty());        // …and it said why
    CHECK(win->current_mode() == TargetMode::DigitReader);  // mode unchanged
    CHECK(denso::mode::load(h.h()) == TargetMode::DigitReader);
    // The rollback preserved the Backend configuration too, so the page it is
    // still showing is correct.
    CHECK(denso::brazing::load(h.h()).enabled);
    CHECK(p.enabled->isChecked());

    REQUIRE(q.exec(QStringLiteral("DROP TRIGGER block_switch")));
}

TEST_CASE("a committed switch closes Settings exactly once",
          "[ui_controls][close]") {
    Harness h;
    h.seed_camera();
    h.store_brazing(true, kUrlA);

    auto win = h.make_window();
    ServerPage p(*win);
    p.dlg->show();
    SwitchTally sig;
    sig.watch(*p.dlg);

    win->perform_switch(TargetMode::BallLeveler);
    CHECK(sig.finished == 1);

    // A second switch with the dialog already closed must not close it again —
    // the no-op guard is what keeps "closed once" true rather than "closed on
    // every switch for the life of the process".
    win->perform_switch(TargetMode::DigitReader);
    CHECK(sig.finished == 1);
    CHECK_FALSE(p.dlg->isVisible());
}

TEST_CASE("closing after a switch persists no unrelated edit",
          "[ui_controls][close]") {
    // The dialog is dismissed as a DISCARD, never as a Save. Staged edits the
    // operator never confirmed must die with the form.
    Harness h;
    h.seed_camera();
    h.store_brazing(true, kUrlA);
    const bool dark_before = h.state->dark;

    auto win = h.make_window();
    ServerPage p(*win);
    p.dlg->show();
    SwitchTally sig;
    sig.watch(*p.dlg);

    // Stage edits on three different pages, then switch.
    auto* dark = check(*p.dlg, "darkModeSwitch");
    REQUIRE(dark != nullptr);
    dark->setChecked(!dark->isChecked());
    p.set_base(QString::fromLatin1(kUrlB));
    p.enabled->setChecked(false);
    REQUIRE(p.dlg->is_dirty());

    win->perform_switch(TargetMode::BallLeveler);

    CHECK_FALSE(p.dlg->isVisible());
    // Nothing the operator staged reached the database…
    CHECK(denso::brazing::load(h.h()).base_url == kUrlA);   // NOT kUrlB
    CHECK(denso::settings::load(h.h()).dark == dark_before);
    CHECK(h.state->dark == dark_before);
    // …and no Save/apply signal was emitted on the way out.
    CHECK(sig.applies() == 0);
    CHECK(sig.switch_requested == 0);
}

TEST_CASE("closing after a switch restores an unsaved theme preview",
          "[ui_controls][close]") {
    // The preview was APPLIED to the running app without being persisted. If the
    // dialog vanished without undoing it, the app would be left wearing a theme
    // the database does not hold, and there would be no dialog left to cancel.
    Harness h;
    h.seed_camera();
    h.store_brazing(true, kUrlA);

    auto win = h.make_window();
    ServerPage p(*win);
    p.dlg->set_theme_dark(true);
    p.dlg->show();
    SwitchTally sig;
    sig.watch(*p.dlg);

    auto* dark = check(*p.dlg, "darkModeSwitch");
    dark->setChecked(false);                 // previews light, persists nothing
    REQUIRE(sig.theme_preview == 1);
    REQUIRE(sig.last_preview_dark == false);

    win->perform_switch(TargetMode::BallLeveler);

    CHECK(sig.theme_preview == 2);           // restored…
    CHECK(sig.last_preview_dark == true);    // …to the persisted theme
    CHECK(sig.theme_commit == 0);            // and never committed
    CHECK(dark->isChecked());                // the widget follows
}

TEST_CASE("the dialog reopens clean and showing the destination mode",
          "[ui_controls][close]") {
    Harness h;
    h.seed_camera();
    h.store_brazing(true, kUrlA);

    auto win = h.make_window();
    ServerPage p(*win);
    p.dlg->show();
    p.enabled->setChecked(false);            // an edit, to prove it is discarded
    REQUIRE(p.dlg->is_dirty());

    win->perform_switch(TargetMode::BallLeveler);
    REQUIRE_FALSE(p.dlg->isVisible());
    CHECK_FALSE(p.dlg->is_dirty());          // cleared before it was put away

    p.dlg->show();                           // …and reopened

    auto* modes = p.dlg->findChild<QComboBox*>(QStringLiteral("modeSelect"));
    REQUIRE(modes != nullptr);
    CHECK(modes->currentData().toInt() == static_cast<int>(TargetMode::BallLeveler));
    p.dlg->select_server_page();
    CHECK_FALSE(p.enabled->isChecked());     // the switch's brazing.enabled = 0
    CHECK(p.base() == QString::fromLatin1(kUrlA));   // address preserved
    CHECK_FALSE(p.dlg->is_dirty());
    CHECK_FALSE(p.save->isEnabled());
    CHECK(win->displayed_brazing_status() == BrazingStatus::Off);
}

TEST_CASE("closing after a switch does not disturb Backend live reload",
          "[ui_controls][close]") {
    // The whole recovery path, end to end, over a dialog that closed itself: the
    // operator switches back, re-runs camera setup, re-enables reporting and it
    // starts — with no restart and the address never retyped.
    Harness h;
    h.seed_camera();
    h.store_brazing(true, kUrlA);

    auto win = h.make_window();
    ServerPage p(*win);
    p.dlg->show();
    win->perform_switch(TargetMode::BallLeveler);
    REQUIRE_FALSE(p.dlg->isVisible());
    REQUIRE(win->displayed_brazing_status() == BrazingStatus::Off);

    win->perform_switch(TargetMode::DigitReader);
    // A switch is destructive: setup_complete is cleared, so no camera is
    // runtime-eligible until the wizard is re-run. Stand in for that, then use
    // the Refresh button — the same seam the operator would.
    QSqlQuery reconfigure(h.h());
    REQUIRE(reconfigure.exec(QStringLiteral("UPDATE camera SET setup_complete = 1")));
    btn(*win, "refreshCamerasButton")->click();
    spin(50);
    REQUIRE(win->camera_view_stream_count() == 1);

    p.dlg->show();
    p.dlg->select_server_page();
    REQUIRE_FALSE(p.enabled->isChecked());
    CHECK(p.base() == QString::fromLatin1(kUrlA));

    p.enabled->setChecked(true);
    p.save->click();

    CHECK(denso::brazing::load(h.h()).enabled);
    CHECK(denso::brazing::load(h.h()).base_url == kUrlA);
    CHECK(win->displayed_brazing_status() == BrazingStatus::On);
}

TEST_CASE("a refused switch never reaches the close at all", "[ui_controls][close]") {
    // The gate is `outcome == Committed`, so anything that never produces a commit
    // must leave the dialog alone. on_switch_mode refuses a same-mode request
    // before any confirmation, transaction or lifecycle event exists — proven by
    // the switch observer never firing at all.
    Harness h;
    h.seed_camera();
    h.store_brazing(true, kUrlA);

    auto win = h.make_window();
    ServerPage p(*win);
    p.dlg->show();

    int lifecycle_events = 0;
    win->set_switch_observer([&](MainWindow::SwitchEvent) { ++lifecycle_events; });

    win->on_switch_mode(static_cast<int>(TargetMode::DigitReader));   // already active

    CHECK(lifecycle_events == 0);   // no teardown, no transaction, no reload
    CHECK(p.dlg->isVisible());
    CHECK(p.enabled->isChecked());
    CHECK(denso::mode::load(h.h()) == TargetMode::DigitReader);
}

// MUTATION: "close only when the window ALSO finished updating cleanly" must die.
//
// A commit that could not finish updating the window is still a commit — both
// modes' setup is gone, so the form on screen is just as invalid — and this dialog
// is application-MODAL while the "restart the application" warning is deliberately
// not, so leaving it up would put a critical message behind something the operator
// must dismiss first. The close therefore gates on the COMMIT alone, and runs
// BEFORE that warning.
//
// Asserted on the source because the state cannot be reached from a test: the
// observer seam that could inject a post-commit fault is `noexcept` and swallows
// throws by design, and every other note() source is either pre-commit or
// swallowed by a repo function that returns empty rather than throwing. A guard
// that pins the decision is worth more than no guard at all.
TEST_CASE("the close gate is the commit alone, and precedes the warning",
          "[ui_controls][close]") {
    const QString src = read_source("/src/app/ui/mainwindow.cpp");

    CHECK(src.simplified().contains(QStringLiteral(
        "if (outcome == Outcome::Committed) { try { "
        "settings_->close_after_mode_switch();")));
    // The rejected narrower gate, in the shape it would take if reintroduced.
    // Scoped to the CLOSE statement: `Outcome::Committed && failure.isEmpty()` is
    // also how the logging branch picks between "switched to X" and "COMMITTED but
    // the window could not finish updating", which is a different question and
    // must keep its failure term.
    CHECK_FALSE(src.simplified().contains(QStringLiteral(
        "failure.isEmpty()) { try { settings_->close_after_mode_switch();")));

    // …and the ordering, so the warning lands on the main window unobstructed.
    const int close_at = src.indexOf(QStringLiteral("close_after_mode_switch();"));
    const int warning_at = src.indexOf(QStringLiteral("QStringLiteral(\"Switch Target Mode\")"));
    REQUIRE(close_at > 0);
    REQUIRE(warning_at > 0);
    CHECK(close_at < warning_at);
}
