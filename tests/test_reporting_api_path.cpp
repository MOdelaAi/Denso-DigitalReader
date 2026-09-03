// The reporting API path is configurable from the GUI, and every layer agrees on
// the endpoint it produces.
//
// The path used to be a compile-time constant, so a customer whose backend
// exposed anything else could not be reached without a rebuild. Three layers
// each prove the part only they can see:
//   • SettingsDialog  — a custom path validates, persists, and comes back on the
//     next open; the live "Effective endpoint" preview shows the SAME string the
//     transport will use, never a private concatenation of its own;
//   • CameraGrid      — a path-only change REBUILDS the sender (the identity is
//     the composed endpoint, not the base URL, which would have missed it), and
//     an unusable path starts no sender at all;
//   • BrazingClient   — the endpoint it is constructed with carries the custom
//     path, so the runtime posts where the operator said.
//
// Plus the contract that must NOT have moved: an installation with no configured
// path posts to the historical endpoint, and the payload is untouched — this
// change is about WHERE a report is sent, not what is in it.
//
// Drives the REAL objects over the app library. Nothing here contacts a backend,
// a camera or the Internet: every fixture camera points at a closed localhost
// port and is model-less, so no engine is ever loaded, and no client is ever
// asked to post.
#include <catch2/catch_test_macros.hpp>

#include "brazing_form_util.h"

#include "brazing/brazing_client.h"
#include "brazing/brazing_payload.h"
#include "brazing/config.h"
#include "brazing/url.h"
#include "camera/camera.h"
#include "camera/repo.h"
#include "db/db.h"
#include "detection/engine_registry.h"
#include "paths/paths.h"
#include "ui/camera/grid/camera_grid.h"
#include "ui/settings/settings_dialog.h"

#include <QByteArray>
#include <QCheckBox>
#include <QDir>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSqlQuery>
#include <QString>
#include <QTemporaryDir>

#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>

using denso::ui::BrazingClient;
using denso::ui::CameraGrid;
using denso::ui::EngineRegistry;
using denso::ui::SettingsDialog;
using denso::ui::ZoneValue;

namespace {

constexpr const char* kUrlA = "http://192.168.1.112:8080";
constexpr const char* kCustomPath = "/api/denso/update";
constexpr const char* kDefaultEndpoint =
    "http://192.168.1.112:8080/api/brazing/update";
constexpr const char* kCustomEndpoint = "http://192.168.1.112:8080/api/denso/update";

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
// fast, so no device is opened and no engine is requested.
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
    void store_brazing(bool enabled, const std::string& base_url,
                       const std::string& api_path) {
        denso::brazing::BrazingConfig c;
        c.enabled = enabled;
        c.base_url = base_url;
        c.api_path = api_path;
        REQUIRE(denso::brazing::save(h(), c));
    }

    // The pre-setting shape: the two rows that existed before, and NO
    // brazing.api_path row. This is what an appliance in the field looks like.
    void store_brazing_legacy(bool enabled, const std::string& base_url) {
        QSqlQuery q(h());
        q.prepare(QStringLiteral(
            "INSERT INTO settings (key, value) VALUES (?, ?) "
            "ON CONFLICT(key) DO UPDATE SET value = excluded.value"));
        q.addBindValue(QStringLiteral("brazing.enabled"));
        q.addBindValue(enabled ? QStringLiteral("1") : QStringLiteral("0"));
        REQUIRE(q.exec());
        q.addBindValue(QStringLiteral("brazing.base_url"));
        q.addBindValue(QString::fromStdString(base_url));
        REQUIRE(q.exec());
        QSqlQuery check(h());
        REQUIRE(check.exec(QStringLiteral(
            "SELECT COUNT(*) FROM settings WHERE key = 'brazing.api_path'")));
        REQUIRE(check.next());
        REQUIRE(check.value(0).toInt() == 0);   // the case is not vacuous
    }
};

struct DialogFixture {
    std::optional<denso::db::Db> db;
    std::optional<SettingsDialog> dlg;
    QComboBox* scheme = nullptr;
    QLineEdit* host = nullptr;
    QLineEdit* port = nullptr;
    QLineEdit* path = nullptr;
    QCheckBox* on = nullptr;
    QPushButton* save = nullptr;
    QLabel* endpoint = nullptr;

    DialogFixture() {
        db = denso::db::Db::open_in_memory();
        REQUIRE(db);
        REQUIRE(denso::db::run_migrations(db->handle()));
        build();
    }

    // Construct (or RE-construct) the dialog against the same database, which is
    // how "saved and restored" is actually observed: a second dialog seeds itself
    // from the stored rows with nothing carried over in memory.
    void build() {
        dlg.emplace(db->handle());
        scheme = denso::testing::brazing_scheme(*dlg);
        host = denso::testing::brazing_host(*dlg);
        port = denso::testing::brazing_port(*dlg);
        path = dlg->findChild<QLineEdit*>(QStringLiteral("brazingApiPath"));
        on = dlg->findChild<QCheckBox*>(QStringLiteral("brazingEnabled"));
        save = dlg->findChild<QPushButton*>(QStringLiteral("saveChangesButton"));
        endpoint = dlg->findChild<QLabel*>(QStringLiteral("brazingEndpoint"));
        REQUIRE(path != nullptr);
        REQUIRE(on != nullptr);
        REQUIRE(save != nullptr);
        REQUIRE(endpoint != nullptr);
    }

    /// Fill the three address controls from one base URL, for the cases that are
    /// about the API PATH rather than about the address decomposition.
    void set_base(const QString& base_url) {
        denso::testing::set_brazing_base(*dlg, base_url);
    }
};

} // namespace

// ── SettingsDialog: the field, the round trip, the preview ───────────────────

TEST_CASE("the Server page opens showing the default reporting API path",
          "[api_path_ui]") {
    // A fresh database has no brazing.api_path row. The field must still show the
    // path the application will use, not an empty box the operator has to guess
    // the meaning of.
    DialogFixture f;
    CHECK(f.path->text() ==
          QString::fromLatin1(denso::brazing::kDefaultApiPath));
}

TEST_CASE("a custom reporting API path is saved and restored", "[api_path_ui]") {
    DialogFixture f;
    f.on->setChecked(true);
    f.set_base(QString::fromLatin1(kUrlA));
    f.path->setText(QString::fromLatin1(kCustomPath));
    f.save->click();

    const auto stored = denso::brazing::load(f.db->handle());
    CHECK(stored.enabled);
    CHECK(stored.base_url == kUrlA);
    CHECK(stored.api_path == kCustomPath);

    // …and a NEW dialog over the same database comes up on the stored value, with
    // nothing carried over in memory.
    f.build();
    CHECK(f.path->text() == QString::fromLatin1(kCustomPath));
    CHECK(denso::testing::brazing_base_text(*f.dlg) == QString::fromLatin1(kUrlA));
}

TEST_CASE("a missing leading slash is normalized on save", "[api_path_ui]") {
    DialogFixture f;
    f.on->setChecked(true);
    f.set_base(QString::fromLatin1(kUrlA));
    f.path->setText(QStringLiteral("api/denso/update"));
    f.save->click();

    CHECK(denso::brazing::load(f.db->handle()).api_path == kCustomPath);
    // The field shows what was actually STORED — silently persisting something
    // other than what the operator is looking at is how the doubled-path defect
    // stayed invisible for so long.
    CHECK(f.path->text() == QString::fromLatin1(kCustomPath));
}

TEST_CASE("a blank reporting API path is saved as the default", "[api_path_ui]") {
    // Clearing the field is not a way to break reporting. It resolves to the
    // shipped default — the same rule that makes a missing row work — and the
    // field is re-seeded so the operator can SEE that is what happened.
    DialogFixture f;
    f.on->setChecked(true);
    f.set_base(QString::fromLatin1(kUrlA));
    f.path->setText(QStringLiteral("   "));
    f.save->click();

    CHECK(denso::brazing::load(f.db->handle()).api_path ==
          denso::brazing::kDefaultApiPath);
    CHECK(f.path->text() ==
          QString::fromLatin1(denso::brazing::kDefaultApiPath));
    CHECK(f.endpoint->text() == QString::fromLatin1(kDefaultEndpoint));
}

TEST_CASE("a full server URL in the path field is refused and persists nothing",
          "[api_path_ui]") {
    // THE misuse this field invites: pasting the whole endpoint into the wrong
    // box. Host and scheme belong to Server base URL, and accepting them here
    // would leave a server address stored in two settings that could disagree.
    DialogFixture f;
    f.on->setChecked(true);
    f.set_base(QString::fromLatin1(kUrlA));
    f.path->setText(QStringLiteral("http://another-server/api/update"));

    int emitted = 0;
    QObject::connect(&*f.dlg, &SettingsDialog::brazing_config_changed,
                     [&] { ++emitted; });
    f.save->click();

    CHECK(emitted == 0);                                     // no reconfiguration
    const auto stored = denso::brazing::load(f.db->handle());
    CHECK_FALSE(stored.enabled);                             // nothing was written
    CHECK(stored.base_url.empty());
    CHECK(stored.api_path == denso::brazing::kDefaultApiPath);
    CHECK(f.path->property("invalid").toBool());             // the red field…
    auto* status = f.dlg->findChild<QLabel*>(QStringLiteral("brazingStatus"));
    REQUIRE(status != nullptr);
    CHECK_FALSE(status->text().isEmpty());                   // …and a visible reason
    // The rejected text is left alone — never silently rewritten into something
    // that would post somewhere the operator did not ask for.
    CHECK(f.path->text() == QStringLiteral("http://another-server/api/update"));
}

TEST_CASE("an https URL in the path field is refused too", "[api_path_ui]") {
    DialogFixture f;
    f.on->setChecked(true);
    f.set_base(QString::fromLatin1(kUrlA));
    f.path->setText(QStringLiteral("https://another-server/api/update"));
    f.save->click();

    CHECK(denso::brazing::load(f.db->handle()).base_url.empty());
    CHECK(f.path->property("invalid").toBool());
}

TEST_CASE("the effective-endpoint preview is what the runtime would use",
          "[api_path_ui]") {
    // The preview is the operator's only confirmation before Save. Asserting it
    // equals brazing::endpoint_url of the same two fields is what keeps it from
    // drifting into a private concatenation that looks right and posts wrong.
    DialogFixture f;
    f.set_base(QString::fromLatin1(kUrlA));
    f.path->setText(QString::fromLatin1(kCustomPath));
    CHECK(f.endpoint->text() == QString::fromLatin1(kCustomEndpoint));
    CHECK(f.endpoint->text() ==
          QString::fromStdString(denso::brazing::endpoint_url(
              denso::testing::brazing_base_text(*f.dlg).toStdString(),
              f.path->text().toStdString())));

    // …and it is LIVE on both fields, before anything is saved.
    f.path->setText(QStringLiteral("/api/v1/zones/update"));
    CHECK(f.endpoint->text() ==
          QStringLiteral("http://192.168.1.112:8080/api/v1/zones/update"));
    f.set_base(QStringLiteral("http://192.168.1.113:9090"));
    CHECK(f.endpoint->text() ==
          QStringLiteral("http://192.168.1.113:9090/api/v1/zones/update"));
}

TEST_CASE("the preview never shows a malformed endpoint", "[api_path_ui]") {
    // The two malformations this whole authority exists to prevent, entered the
    // way an operator would actually produce them.
    DialogFixture f;
    f.set_base(QStringLiteral("http://192.168.1.112:8080/"));
    f.path->setText(QString::fromLatin1(kCustomPath));
    CHECK(f.endpoint->text() == QString::fromLatin1(kCustomEndpoint));   // not "//"

    f.path->setText(QStringLiteral("api/denso/update"));
    CHECK(f.endpoint->text() == QString::fromLatin1(kCustomEndpoint));   // not "8080api"

    // And a value that yields no endpoint says which half is wrong rather than
    // showing a partial URL.
    f.path->setText(QStringLiteral("http://another-server/api/update"));
    CHECK_FALSE(f.endpoint->text().isEmpty());
    CHECK_FALSE(f.endpoint->text().contains(QStringLiteral("://another-server")));

    f.path->setText(QString::fromLatin1(kCustomPath));
    f.host->setText(QString());
    f.port->setText(QString());
    CHECK_FALSE(f.endpoint->text().isEmpty());   // "no server address" is stated
    CHECK_FALSE(f.endpoint->text().startsWith(QStringLiteral("http")));
}

TEST_CASE("editing only the reporting API path arms Save", "[api_path_ui]") {
    // Dirty is a comparison over the whole form; a field left out of FormState
    // would leave the primary action greyed out with a real edit pending.
    DialogFixture f;
    REQUIRE_FALSE(f.save->isEnabled());
    f.path->setText(QString::fromLatin1(kCustomPath));
    CHECK(f.save->isEnabled());
}

// ── CameraGrid: the live reporting-stack swap ────────────────────────────────

TEST_CASE("the runtime posts to the configured reporting API path",
          "[api_path_runtime]") {
    Harness h;
    h.seed_camera();
    h.store_brazing(true, kUrlA, kCustomPath);

    CameraGrid grid(h.h(), h.engines, /*warmup*/ nullptr);
    grid.reload();

    REQUIRE(grid.has_brazing_sender());
    CHECK(grid.active_brazing_base_url() == kUrlA);
    CHECK(grid.active_brazing_api_path() == kCustomPath);
    CHECK(grid.active_brazing_endpoint() == kCustomEndpoint);
}

TEST_CASE("changing ONLY the reporting API path replaces the sender",
          "[api_path_runtime]") {
    // The defect this guards: the sender's identity used to be the base URL, so a
    // path-only edit would compare equal, the Save would be treated as inert, and
    // the appliance would go on posting to the OLD path with the UI showing the
    // new one.
    Harness h;
    h.seed_camera();
    h.store_brazing(true, kUrlA, denso::brazing::kDefaultApiPath);

    CameraGrid grid(h.h(), h.engines, /*warmup*/ nullptr);
    grid.reload();
    REQUIRE(grid.brazing_sender_builds() == 1);
    REQUIRE(grid.active_brazing_endpoint() == kDefaultEndpoint);

    h.store_brazing(true, kUrlA, kCustomPath);   // same server, new path
    grid.apply_brazing_config();

    // REPLACED, not added: one live sender, built a second time, on the new path.
    CHECK(grid.has_brazing_sender());
    CHECK(grid.brazing_sender_builds() == 2);
    CHECK(grid.active_brazing_endpoint() == kCustomEndpoint);
}

TEST_CASE("re-saving an unchanged custom path is still inert",
          "[api_path_runtime]") {
    // The other half of comparing on the endpoint: it must not have become
    // trigger-happy. A rebuild on every Save silently resets delivery state and
    // re-sends values the server already acked.
    Harness h;
    h.seed_camera();
    h.store_brazing(true, kUrlA, kCustomPath);

    CameraGrid grid(h.h(), h.engines, /*warmup*/ nullptr);
    grid.reload();
    REQUIRE(grid.brazing_sender_builds() == 1);

    grid.apply_brazing_config();
    grid.apply_brazing_config();

    CHECK(grid.brazing_sender_builds() == 1);
    CHECK(grid.has_brazing_sender());
}

TEST_CASE("an unusable stored reporting API path starts no sender",
          "[api_path_runtime]") {
    // Fail closed, exactly as an unusable base URL does: no sender, no guessing,
    // and the top-bar indicator reads Off rather than claiming readings are going
    // out. Only an externally written row can produce this — the UI refuses it.
    Harness h;
    h.seed_camera();
    h.store_brazing(true, kUrlA, "http://another-server/api/update");

    CameraGrid grid(h.h(), h.engines, /*warmup*/ nullptr);
    grid.reload();

    CHECK_FALSE(grid.has_brazing_sender());
    CHECK(grid.active_brazing_endpoint().empty());
    // Aggregation continues: the on-screen readings are a LOCAL check and must
    // not depend on the backend being configured correctly.
    CHECK(grid.has_live_streams());
}

// ── Backward compatibility: an installation that never configured a path ─────

TEST_CASE("a configuration with no API path row keeps the exact old endpoint",
          "[api_path_runtime]") {
    // THE mandatory case. An appliance upgraded in the field has no
    // brazing.api_path row, must need no operator action, and must go on posting
    // byte-for-byte where it always did.
    Harness h;
    h.seed_camera();
    h.store_brazing_legacy(true, kUrlA);

    CameraGrid grid(h.h(), h.engines, /*warmup*/ nullptr);
    grid.reload();

    REQUIRE(grid.has_brazing_sender());
    CHECK(grid.active_brazing_endpoint() == kDefaultEndpoint);
}

TEST_CASE("the Settings page shows an un-migrated configuration truthfully",
          "[api_path_ui]") {
    // The other half of the same case: the operator opening Settings on an
    // upgraded appliance sees the endpoint that is really in use, not a blank box.
    auto db = denso::db::Db::open_in_memory();
    REQUIRE(db);
    REQUIRE(denso::db::run_migrations(db->handle()));
    QSqlQuery q(db->handle());
    REQUIRE(q.exec(QStringLiteral(
        "INSERT INTO settings (key, value) VALUES ('brazing.enabled', '1')")));
    REQUIRE(q.exec(QStringLiteral(
        "INSERT INTO settings (key, value) "
        "VALUES ('brazing.base_url', 'http://192.168.1.112:8080')")));

    SettingsDialog dlg(db->handle());
    auto* path = dlg.findChild<QLineEdit*>(QStringLiteral("brazingApiPath"));
    auto* endpoint = dlg.findChild<QLabel*>(QStringLiteral("brazingEndpoint"));
    auto* save = dlg.findChild<QPushButton*>(QStringLiteral("saveChangesButton"));
    REQUIRE(path != nullptr);
    REQUIRE(endpoint != nullptr);
    REQUIRE(save != nullptr);

    CHECK(path->text() == QString::fromLatin1(denso::brazing::kDefaultApiPath));
    CHECK(endpoint->text() == QString::fromLatin1(kDefaultEndpoint));
    // Seeding is not an edit: merely opening the page must not arm the primary
    // action, or every operator would be invited to "save" a change they did not
    // make.
    CHECK_FALSE(save->isEnabled());
}

// ── The transport, and the contract that did NOT change ──────────────────────

TEST_CASE("BrazingClient composes the configured path into its endpoint",
          "[api_path_runtime]") {
    // Fixed at construction, so no request can begin against a path the operator
    // has since replaced.
    BrazingClient custom(kUrlA, kCustomPath);
    CHECK(custom.endpoint() == QString::fromLatin1(kCustomEndpoint));

    // The defaulted overload is the pre-existing behaviour, unchanged.
    BrazingClient defaulted(kUrlA);
    CHECK(defaulted.endpoint() == QString::fromLatin1(kDefaultEndpoint));

    // An unusable path yields NO endpoint, so post() declines rather than
    // inventing a destination.
    BrazingClient broken(kUrlA, "http://another-server/api/update");
    CHECK(broken.endpoint().isEmpty());
}

TEST_CASE("the reporting payload is unchanged by the path setting",
          "[api_path_runtime]") {
    // This change is about WHERE a report is sent, not what is in it. The body is
    // built with no knowledge of the endpoint at all — asserted here so a future
    // edit cannot quietly make the payload depend on the configured path.
    std::map<int, ZoneValue> zones;
    zones.emplace(1, ZoneValue{300, 2});
    zones.emplace(99, ZoneValue{42, 0});
    const std::string body = denso::ui::build_brazing_payload(zones);
    CHECK(body == "{\"zone1\":3.00,\"zone99\":42}");
}

// ── The operator acceptance scenarios, driven end to end ─────────────────────
//
// These mirror the manual GUI checklist exactly — same values, same order of
// actions — so the deterministic evidence and the operator's own run are about
// the same thing. They do NOT replace the manual acceptance; they make it a
// confirmation rather than the first time anything was checked.

TEST_CASE("SCENARIO custom path: api/denso/update is normalized, previewed, "
          "saved and survives closing Settings",
          "[api_path_ui]") {
    DialogFixture f;
    f.on->setChecked(true);
    f.set_base(QString::fromLatin1(kUrlA));

    // Typed WITHOUT the leading slash, exactly as an operator would.
    f.path->setText(QStringLiteral("api/denso/update"));
    // The preview repairs it live, before anything is saved.
    CHECK(f.endpoint->text() == QString::fromLatin1(kCustomEndpoint));

    f.save->click();

    // Normalized on the way to the database…
    CHECK(denso::brazing::load(f.db->handle()).api_path == kCustomPath);
    // …the field now shows what was really stored…
    CHECK(f.path->text() == QString::fromLatin1(kCustomPath));
    // …and the preview still names the endpoint that will be used.
    CHECK(f.endpoint->text() == QString::fromLatin1(kCustomEndpoint));

    // CLOSE and REOPEN the same dialog — the operator's actual gesture, and the
    // path that runs showEvent() then reload_server_page(). This is stronger than
    // constructing a second dialog: it proves the re-seed reads the database
    // rather than leaving whatever happened to be in the widgets.
    f.dlg->show();
    f.dlg->hide();
    f.dlg->show();

    CHECK(f.path->text() == QString::fromLatin1(kCustomPath));
    CHECK(denso::testing::brazing_base_text(*f.dlg) == QString::fromLatin1(kUrlA));
    CHECK(f.endpoint->text() == QString::fromLatin1(kCustomEndpoint));
    // Reopening is not an edit: the primary action must not be armed.
    CHECK_FALSE(f.save->isEnabled());

    // And a completely fresh dialog over the same database agrees.
    f.build();
    CHECK(f.path->text() == QString::fromLatin1(kCustomPath));
    CHECK(f.endpoint->text() == QString::fromLatin1(kCustomEndpoint));
}

TEST_CASE("SCENARIO trailing-slash base: exactly one joining slash",
          "[api_path_ui]") {
    DialogFixture f;
    f.on->setChecked(true);
    f.set_base(QStringLiteral("http://192.168.1.112:8080/"));
    f.path->setText(QString::fromLatin1(kCustomPath));

    // Neither a doubled slash nor a welded host, in the preview…
    CHECK(f.endpoint->text() == QString::fromLatin1(kCustomEndpoint));
    CHECK_FALSE(f.endpoint->text().contains(QStringLiteral("8080//")));

    f.save->click();

    // …nor in what the transport composes from what was stored.
    const auto stored = denso::brazing::load(f.db->handle());
    CHECK(stored.base_url == kUrlA);                       // the slash is gone
    CHECK(denso::brazing::endpoint_url(stored.base_url, stored.api_path) ==
          kCustomEndpoint);
    // Stated as a count so no future normalization can pass here by accident:
    // the only doubled slash in the endpoint is the one inside the scheme.
    CHECK(QString::fromLatin1(kCustomEndpoint).count(QStringLiteral("//")) == 1);
}

TEST_CASE("SCENARIO invalid path: a rejected value never becomes the runtime "
          "endpoint",
          "[api_path_runtime]") {
    // The load-bearing half of "rejected clearly": not merely that the message
    // appears, but that the RUNNING appliance is untouched — it keeps delivering
    // to the endpoint it was already using, and adopts neither the refused value
    // nor a fallback.
    Harness h;
    h.seed_camera();
    h.store_brazing(true, kUrlA, kCustomPath);

    CameraGrid grid(h.h(), h.engines, /*warmup*/ nullptr);
    grid.reload();
    REQUIRE(grid.active_brazing_endpoint() == kCustomEndpoint);
    REQUIRE(grid.brazing_sender_builds() == 1);

    SettingsDialog dlg(h.h());
    QObject::connect(&dlg, &SettingsDialog::brazing_config_changed, &grid,
                     &CameraGrid::apply_brazing_config);   // the MainWindow wiring
    auto* path = dlg.findChild<QLineEdit*>(QStringLiteral("brazingApiPath"));
    auto* save = dlg.findChild<QPushButton*>(QStringLiteral("saveChangesButton"));
    auto* status = dlg.findChild<QLabel*>(QStringLiteral("brazingStatus"));
    REQUIRE(path != nullptr);
    REQUIRE(save != nullptr);
    REQUIRE(status != nullptr);

    path->setText(QStringLiteral("http://other-server/api/update"));
    save->click();

    // Rejected clearly…
    CHECK(path->property("invalid").toBool());
    CHECK_FALSE(status->text().isEmpty());
    // …nothing persisted…
    CHECK(denso::brazing::load(h.h()).api_path == kCustomPath);
    // …and the live sender is the SAME one, still on the old endpoint. Not
    // rebuilt, not retargeted, not stopped.
    CHECK(grid.brazing_sender_builds() == 1);
    CHECK(grid.active_brazing_endpoint() == kCustomEndpoint);
    CHECK(grid.active_brazing_endpoint().find("other-server") ==
          std::string::npos);
}

TEST_CASE("SCENARIO whitespace-only path: the blank-to-default UX, end to end",
          "[api_path_ui]") {
    DialogFixture f;
    f.on->setChecked(true);
    f.set_base(QString::fromLatin1(kUrlA));
    f.path->setText(QString::fromLatin1(kCustomPath));
    f.save->click();
    REQUIRE(denso::brazing::load(f.db->handle()).api_path == kCustomPath);

    // Now clear it to whitespace. The preview answers IMMEDIATELY — this is what
    // makes "blank means the default" a visible outcome rather than a silent one.
    f.path->setText(QStringLiteral("   "));
    CHECK(f.endpoint->text() == QString::fromLatin1(kDefaultEndpoint));
    CHECK(f.save->isEnabled());          // it is a real edit, and it is armed

    f.save->click();

    CHECK(denso::brazing::load(f.db->handle()).api_path ==
          denso::brazing::kDefaultApiPath);
    // The field is re-seeded, so the operator sees the resolved value rather than
    // the blank they typed.
    CHECK(f.path->text() ==
          QString::fromLatin1(denso::brazing::kDefaultApiPath));
    CHECK(f.endpoint->text() == QString::fromLatin1(kDefaultEndpoint));
    // Never a bare base URL, which is what a blank path would compose to if it
    // were taken literally.
    CHECK(f.endpoint->text() != QString::fromLatin1(kUrlA));
}

TEST_CASE("SCENARIO runtime switch: saving ONLY a new path retires the old "
          "sender through the real dialog signal",
          "[api_path_runtime]") {
    // THE most important scenario: the appliance must not keep posting to the old
    // endpoint. Driven through the same chain MainWindow wires in production —
    // Save, then brazing_config_changed, then CameraGrid::apply_brazing_config —
    // rather than by calling the grid directly, so the signal, the persist and
    // the swap are proven together.
    Harness h;
    h.seed_camera();
    h.store_brazing(true, kUrlA, denso::brazing::kDefaultApiPath);

    CameraGrid grid(h.h(), h.engines, /*warmup*/ nullptr);
    grid.reload();
    REQUIRE(grid.brazing_sender_builds() == 1);
    REQUIRE(grid.active_brazing_endpoint() == kDefaultEndpoint);

    SettingsDialog dlg(h.h());
    QObject::connect(&dlg, &SettingsDialog::brazing_config_changed, &grid,
                     &CameraGrid::apply_brazing_config);
    auto* host = denso::testing::brazing_host(dlg);
    auto* port = denso::testing::brazing_port(dlg);
    auto* path = dlg.findChild<QLineEdit*>(QStringLiteral("brazingApiPath"));
    auto* save = dlg.findChild<QPushButton*>(QStringLiteral("saveChangesButton"));
    REQUIRE(path != nullptr);
    REQUIRE(save != nullptr);
    // The address controls are NOT touched anywhere in this case — captured here
    // so the assertions below rest on an observed fact, not on an intention.
    REQUIRE(host->text() == QStringLiteral("192.168.1.112"));
    REQUIRE(port->text() == QStringLiteral("8080"));

    path->setText(QString::fromLatin1(kCustomPath));      // only the path moves
    save->click();

    CHECK(host->text() == QStringLiteral("192.168.1.112"));   // still untouched
    CHECK(port->text() == QStringLiteral("8080"));

    // The base URL never moved, so a sender identity keyed on the base would have
    // compared equal here and left the old endpoint live. It is keyed on the
    // composed endpoint, so the sender was retired and replaced.
    CHECK(denso::brazing::load(h.h()).base_url == kUrlA);
    CHECK(denso::brazing::load(h.h()).api_path == kCustomPath);
    CHECK(grid.brazing_sender_builds() == 2);
    CHECK(grid.has_brazing_sender());
    CHECK(grid.active_brazing_endpoint() == kCustomEndpoint);
    // Said explicitly, because "does not continue using the old endpoint" is the
    // requirement, and equality with the new one does not on its own exclude it.
    CHECK(grid.active_brazing_endpoint() != kDefaultEndpoint);
    CHECK(grid.active_brazing_api_path() != denso::brazing::kDefaultApiPath);
}

// ── Regression: a placeholder that masqueraded as a configured value ─────────
//
// REPORTED: the Server page visibly contained "http://192.168.1.112:8080" while
// the page said "No server base URL — nothing will be sent." and Save refused
// with "Enter the server base URL before enabling reporting."
//
// MEASURED CAUSE, not inferred: the field's text() was EMPTY and its
// placeholderText() was the string "http://192.168.1.112:8080". Qt draws a
// placeholder greyed INSIDE the box, so on the panel an empty required field read
// as a filled one. The validation and the preview were both correct; the
// presentation was not. Nothing about the state machine was broken, which is
// exactly why hiding the message would have been the wrong fix — it would have
// left an empty address silently accepted.
//
// THE RULE, pinned below so no future field can reintroduce it:
//   a placeholder may display a usable value ONLY if leaving that field blank
//   really does produce that value.
// The reporting API path satisfies it (blank resolves to the default it shows).
// The server address cannot, so its placeholder must not be a usable address.

TEST_CASE("REGRESSION no Server field shows a placeholder that would be a value",
          "[api_path_ui][regression]") {
    DialogFixture f;

    // The address: blank means "no server", so a placeholder that composes into a
    // working endpoint would be a lie the operator cannot see through.
    const QString host_ph = f.host->placeholderText();
    CHECK_FALSE(host_ph.isEmpty());          // it must still guide
    denso::brazing::BaseUrlParts as_typed;
    as_typed.host = host_ph.toStdString();
    CHECK_FALSE(denso::brazing::compose_base_url(as_typed).ok);

    // The port: blank means the protocol's default, so its placeholder must not
    // read as a port number either.
    const QString port_ph = f.port->placeholderText();
    CHECK_FALSE(port_ph.isEmpty());
    denso::brazing::BaseUrlParts port_typed;
    port_typed.host = "192.168.1.112";
    port_typed.port = port_ph.toStdString();
    CHECK_FALSE(denso::brazing::compose_base_url(port_typed).ok);

    // The API path is the ONE field allowed to show a usable value, because blank
    // genuinely resolves to exactly it. Asserted rather than exempted, so the
    // permission stays tied to the reason for it.
    const QString path_ph = f.path->placeholderText();
    const denso::brazing::ApiPathResult shown =
        denso::brazing::normalize_api_path(path_ph.toStdString());
    REQUIRE(shown.ok);
    const denso::brazing::ApiPathResult blank = denso::brazing::normalize_api_path("");
    REQUIRE(blank.ok);
    CHECK(shown.api_path == blank.api_path);
}

TEST_CASE("REGRESSION an empty address is stated as empty, not shown as filled",
          "[api_path_ui][regression]") {
    // The operator's actual sequence: open Settings on a fresh appliance, tick the
    // box, press Save. What they must NOT see is a box that looks populated next
    // to a message saying nothing is configured.
    DialogFixture f;
    REQUIRE(f.host->text().isEmpty());          // nothing is configured…

    // …and the preview says so by naming the FIELD the operator has to fill,
    // rather than a "base URL" control that no longer exists.
    CHECK(f.endpoint->text().contains(QStringLiteral("server address")));
    CHECK_FALSE(f.endpoint->text().startsWith(QStringLiteral("http")));

    f.on->setChecked(true);
    f.save->click();

    auto* status = f.dlg->findChild<QLabel*>(QStringLiteral("brazingStatus"));
    REQUIRE(status != nullptr);
    CHECK(status->text().contains(QStringLiteral("server address")));
    // The empty field is REDDENED, which is what distinguishes it from a filled
    // one at a glance on the panel — the greyed placeholder never did.
    CHECK(f.host->property("invalid").toBool());
    CHECK_FALSE(denso::brazing::load(f.db->handle()).enabled);   // and nothing saved
}

TEST_CASE("REGRESSION a filled address is never treated as absent",
          "[api_path_ui][regression]") {
    // The complement, and the assertion that would have failed had the cause been
    // what it first looked like — a populated field the validator could not see.
    DialogFixture f;
    f.host->setText(QStringLiteral("192.168.1.112"));
    f.port->setText(QStringLiteral("8080"));

    CHECK(f.endpoint->text() == QString::fromLatin1(kDefaultEndpoint));
    CHECK_FALSE(f.endpoint->text().contains(QStringLiteral("server address")));

    f.on->setChecked(true);
    f.save->click();

    CHECK(denso::brazing::load(f.db->handle()).enabled);
    CHECK(denso::brazing::load(f.db->handle()).base_url == kUrlA);
    CHECK_FALSE(f.host->property("invalid").toBool());
}

// ── The decomposed form, driven as the operator will ─────────────────────────

TEST_CASE("SCENARIO the decomposed form composes the endpoint the request uses",
          "[api_path_ui]") {
    // The shape from the UI sketch, filled in field by field.
    DialogFixture f;
    f.on->setChecked(true);
    const int https_row = f.scheme->findData(QStringLiteral("http"));
    REQUIRE(https_row >= 0);
    f.scheme->setCurrentIndex(https_row);
    f.host->setText(QStringLiteral("192.168.1.112"));
    f.port->setText(QStringLiteral("8080"));
    f.path->setText(QStringLiteral("/api/iei/update"));

    CHECK(f.endpoint->text() ==
          QStringLiteral("http://192.168.1.112:8080/api/iei/update"));

    f.save->click();

    // ONE canonical row, exactly as before the form was decomposed.
    const auto stored = denso::brazing::load(f.db->handle());
    CHECK(stored.base_url == "http://192.168.1.112:8080");
    CHECK(stored.api_path == "/api/iei/update");
    CHECK(denso::brazing::endpoint_url(stored.base_url, stored.api_path) ==
          "http://192.168.1.112:8080/api/iei/update");

    // …and it comes back into the three controls on a fresh open.
    f.build();
    CHECK(f.scheme->currentData().toString() == QStringLiteral("http"));
    CHECK(f.host->text() == QStringLiteral("192.168.1.112"));
    CHECK(f.port->text() == QStringLiteral("8080"));
    CHECK(f.path->text() == QStringLiteral("/api/iei/update"));
}

TEST_CASE("SCENARIO switching the protocol changes only the scheme",
          "[api_path_ui]") {
    DialogFixture f;
    f.on->setChecked(true);
    f.host->setText(QStringLiteral("server.example.com"));
    f.port->setText(QStringLiteral("8443"));
    const int https_row = f.scheme->findData(QStringLiteral("https"));
    REQUIRE(https_row >= 0);
    f.scheme->setCurrentIndex(https_row);

    CHECK(f.endpoint->text() ==
          QStringLiteral("https://server.example.com:8443/api/brazing/update"));
    CHECK(f.save->isEnabled());   // the combo arms the primary action like a field

    f.save->click();
    CHECK(denso::brazing::load(f.db->handle()).base_url ==
          "https://server.example.com:8443");
}

TEST_CASE("SCENARIO an out-of-range port is refused and reddens the port",
          "[api_path_ui]") {
    DialogFixture f;
    f.on->setChecked(true);
    f.host->setText(QStringLiteral("192.168.1.112"));
    f.port->setText(QStringLiteral("70000"));

    CHECK_FALSE(f.endpoint->text().startsWith(QStringLiteral("http")));

    f.save->click();

    CHECK(denso::brazing::load(f.db->handle()).base_url.empty());
    // The PORT is marked, not the address — the operator is pointed at the field
    // that is actually wrong.
    CHECK(f.port->property("invalid").toBool());
    CHECK_FALSE(f.host->property("invalid").toBool());
}

TEST_CASE("SCENARIO a bad address reddens the address, not the port",
          "[api_path_ui]") {
    DialogFixture f;
    f.on->setChecked(true);
    f.host->setText(QStringLiteral("http://192.168.1.112:8080"));
    f.port->setText(QStringLiteral("8080"));

    f.save->click();

    CHECK(denso::brazing::load(f.db->handle()).base_url.empty());
    CHECK(f.host->property("invalid").toBool());
    CHECK_FALSE(f.port->property("invalid").toBool());
}

TEST_CASE("SCENARIO a blank port stores an address with no port", "[api_path_ui]") {
    // Required for backward compatibility, and a real deployment shape: a
    // customer behind a reverse proxy on the default port has no port to type.
    DialogFixture f;
    f.on->setChecked(true);
    const int https_row = f.scheme->findData(QStringLiteral("https"));
    REQUIRE(https_row >= 0);
    f.scheme->setCurrentIndex(https_row);
    f.host->setText(QStringLiteral("server.example.com"));
    f.port->clear();

    CHECK(f.endpoint->text() ==
          QStringLiteral("https://server.example.com/api/brazing/update"));

    f.save->click();
    CHECK(denso::brazing::load(f.db->handle()).base_url ==
          "https://server.example.com");
    f.build();
    CHECK(f.port->text().isEmpty());       // and it stays blank on reopen
    CHECK(f.host->text() == QStringLiteral("server.example.com"));
}

// ── The refusal is stated once, not twice ────────────────────────────────────
//
// A refused Save puts the reason on the status line, and the endpoint preview has
// been showing that same reason live since the keystroke that caused it. Rendered
// stacked one above the other, one problem reads as two. The preview stands aside
// for exactly as long as the status line carries the same words; the actionable
// line stays, because it is the one attached to the action just taken.

TEST_CASE("REGRESSION a Save refusal is not rendered twice", "[api_path_ui]") {
    DialogFixture f;
    auto* status = f.dlg->findChild<QLabel*>(QStringLiteral("brazingStatus"));
    REQUIRE(status != nullptr);

    f.on->setChecked(true);
    f.host->setText(QStringLiteral("192.168.1.112"));
    f.port->setText(QStringLiteral("70000"));

    // WHILE EDITING the preview carries the reason — that live feedback is the
    // whole point of the field and must survive this cleanup.
    REQUIRE_FALSE(status->isVisible());
    const QString live = f.endpoint->text();
    CHECK(live.contains(QStringLiteral("65535")));

    f.save->click();

    // AFTER the refusal exactly one of the two labels says it.
    CHECK(status->text() == live);              // …the actionable one keeps it…
    CHECK(f.endpoint->text() != status->text());  // …and the preview does not repeat it
    CHECK_FALSE(f.endpoint->text().contains(QStringLiteral("65535")));
}

TEST_CASE("the preview returns as soon as it stops repeating the status",
          "[api_path_ui]") {
    // Standing aside is for the duration of the duplication only. The moment the
    // operator corrects the field, the preview has something of its own to say
    // and says it — without waiting for another Save.
    DialogFixture f;
    f.on->setChecked(true);
    f.host->setText(QStringLiteral("192.168.1.112"));
    f.port->setText(QStringLiteral("70000"));
    f.save->click();
    REQUIRE_FALSE(f.endpoint->text().contains(QStringLiteral("65535")));

    f.port->setText(QStringLiteral("8080"));
    CHECK(f.endpoint->text() == QString::fromLatin1(kDefaultEndpoint));
}

TEST_CASE("a successful Save shows the endpoint AND the confirmation",
          "[api_path_ui]") {
    // The two labels say different things here, so both belong on screen: one
    // confirms the action, the other states where readings will go. Suppression
    // must not have become "hide the preview whenever a status is visible".
    DialogFixture f;
    auto* status = f.dlg->findChild<QLabel*>(QStringLiteral("brazingStatus"));
    REQUIRE(status != nullptr);

    f.on->setChecked(true);
    f.host->setText(QStringLiteral("192.168.1.112"));
    f.port->setText(QStringLiteral("8080"));
    f.save->click();

    CHECK(f.endpoint->text() == QString::fromLatin1(kDefaultEndpoint));
    CHECK_FALSE(status->text().isEmpty());
    CHECK(status->text() != f.endpoint->text());
}

TEST_CASE("the unset-address notice is not duplicated either", "[api_path_ui]") {
    // The other message the two labels can both want to show. Their wordings
    // differ today, so nothing is suppressed — asserted so that a future edit
    // aligning the two texts cannot silently reintroduce the duplication.
    DialogFixture f;
    auto* status = f.dlg->findChild<QLabel*>(QStringLiteral("brazingStatus"));
    REQUIRE(status != nullptr);

    f.on->setChecked(true);
    f.save->click();   // no address at all

    CHECK_FALSE(status->text().isEmpty());
    CHECK(f.endpoint->text() != status->text());
}
