// Slice 1 — Ball Leveler persistence + schema v14.
//
// The properties under test:
//   * v13 -> v14 is ADDITIVE. Every Digital Reader row survives byte-identical.
//   * `ball_level_calibration` is the SOLE durable Ball Leveler binding +
//     calibration authority, and holds AT MOST ONE row per camera — enforced by
//     the schema (camera_id PRIMARY KEY), not by a rule a caller can forget.
//   * `save_level_configuration` is the ONE write chokepoint. It resolves the
//     model through the runtime manifest, asks the CENTRAL compatibility policy,
//     requires exactly one model and exactly one class, validates the geometry,
//     and writes binding + calibration in one transaction that rolls back whole.
//   * `switch_and_reset` is DESTRUCTIVE: it clears Digital Reader
//     configuration, Ball Leveler calibration and every camera connection.
//   * A dormant configuration belonging to the INACTIVE mode never degrades the
//     active mode.
//
// Pure denso_core: no widgets, no engine, no camera, no network. The manifest
// fixture writes real artifact bytes so provenance corroborates for real, and the
// rejection causes are produced by DECLARATION, never by a filename convention.
#include <catch2/catch_test_macros.hpp>

#include "brazing/config.h"
#include "camera/camera.h"
#include "camera/repo.h"
#include "db/db.h"
#include "detection/detection.h"
#include "detection/repo.h"
#include "health/integrity.h"
#include "level/calibration.h"
#include "level/repo.h"
#include "mode/config.h"
#include "mode/mode.h"
#include "mode/reset.h"
#include "models/hashing.h"
#include "models/manifest.h"
#include "models/model_identity.h"

#include <QDir>
#include <QFile>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QVariant>

#include <limits>
#include <type_traits>
#include <optional>
#include <string>
#include <vector>
#include "zone_value_compat.h"

using denso::detection::DetectionModel;
using denso::health::Readiness;
using denso::level::LevelBinding;
using denso::level::LevelCalibration;
using denso::level::LevelZone;
using denso::level::SaveRefusal;
using denso::mode::TargetMode;
using denso::models::Manifest;
using denso::models::ManifestView;
using denso::models::ModelGeneration;
using denso::models::PlatformInfo;

namespace {

#ifdef _WIN32
constexpr const char* kExt = ".onnx";
#else
constexpr const char* kExt = ".engine";
#endif

const PlatformInfo kPlatform{"10.3", "12.6", "87"};

std::string write_and_hash(const QString& dir, const std::string& name,
                           const QByteArray& bytes) {
    const QString path = QDir(dir).filePath(QString::fromStdString(name));
    QFile f(path);
    REQUIRE(f.open(QIODevice::WriteOnly));
    REQUIRE(f.write(bytes) == bytes.size());
    f.close();
    const auto h = denso::models::file_sha256(path);
    REQUIRE(h.has_value());
    return *h;
}

struct Decl {
    std::string stem;
    std::string canonical_id;
    std::string family;
    std::string task = "detect";
    int input_size = 640;
    std::vector<std::string> class_names{"Small"};
    denso::models::BuiltFor built_for{"10.3", "12.6", "87"};
};

ModelGeneration declare(const QString& dir, const Decl& d) {
    const QByteArray body = QByteArrayLiteral("model-bytes");
    ModelGeneration g;
    g.declared = true;
    g.name = d.canonical_id;
    g.installed_utc = "2026-07-30T00:00:00Z";
    g.state = "installed";
    g.canonical_id = d.canonical_id;
    g.family = d.family;
    g.task = d.task;
    g.input_size = d.input_size;
    g.class_count = static_cast<int>(d.class_names.size());
    g.class_names = d.class_names;
#ifdef _WIN32
    denso::models::OnnxRuntimeArtifact ort;
    ort.model = d.stem + ".onnx";
    ort.model_sha256 = write_and_hash(dir, ort.model, body);
    ort.class_metadata_source = denso::models::kSourceOnnxMetadataNames;
    g.runtime.onnxruntime = ort;
#else
    denso::models::TensorRtArtifact trt;
    trt.engine = d.stem + ".engine";
    trt.engine_sha256 = write_and_hash(dir, trt.engine, body);
    trt.sidecar = d.stem + ".names.json";
    QByteArray sidecar = "[";
    for (size_t i = 0; i < d.class_names.size(); ++i)
        sidecar += (i ? ",\"" : "\"") + QByteArray::fromStdString(d.class_names[i]) + "\"";
    sidecar += "]";
    trt.sidecar_sha256 = write_and_hash(dir, trt.sidecar, sidecar);
    trt.class_metadata_source = denso::models::kSourceNamesSidecar;
    trt.built_for = d.built_for;
    g.runtime.tensorrt = trt;
#endif
    return g;
}

DetectionModel catalog_row(const std::string& stem,
                           const std::vector<std::string>& classes) {
    DetectionModel m;
    m.name = stem;
    m.filename = stem + kExt;
    m.class_names = classes;
    return m;
}

/// A camera with real connection/capture columns, so a preservation assertion has
/// something to actually compare.
denso::camera::Camera ip_camera(const std::string& name) {
    denso::camera::Camera c;
    c.name = name;
    c.camera_type = "ip";
    c.active = true;
    c.setup_complete = true;
    c.ip = "192.168.1.200";
    c.rtsp = "rtsp://192.168.1.200/cam/realmonitor";
    c.username = "operator";
    c.channel = 1u;
    c.stream = 0u;
    c.manufacturer = "Dahua";
    c.width = 1920;
    c.height = 1080;
    c.fps = 25;
    c.rotation = 90;
    c.pitch = 1.5f;
    c.roll = -2.5f;
    return c;
}

/// A geometrically valid calibration: 100% line ABOVE the 0% line, both strictly
/// inside the rectangle.
LevelCalibration good_calibration() {
    LevelCalibration c;
    c.rect_x = 0.30;
    c.rect_y = 0.10;
    c.rect_w = 0.40;
    c.rect_h = 0.80;
    c.y_100 = 0.20;   // higher on screen == smaller Y
    c.y_0 = 0.80;
    c.conf = 0.55;
    c.hold_ms = 2000;
    return c;
}

/// A fixture holding a migrated DB plus a models dir declaring one Float model
/// and one digit model, with both catalogued.
struct Fixture {
    QTemporaryDir dir;
    std::optional<denso::db::Db> db;
    std::optional<ManifestView> view;
    int64_t float_model_id = 0;
    int64_t digit_model_id = 0;

    Fixture() {
        REQUIRE(dir.isValid());
        db = denso::db::Db::open(QDir(dir.path()).filePath("denso.db"));
        REQUIRE(db.has_value());
        REQUIRE(denso::db::run_migrations(db->handle()));

        Manifest m;
        m.schema = 2;
        m.generations.push_back(
            declare(dir.path(), Decl{"float-small", "float-small", "float_ball",
                                     "detect", 640, {"Small"}, {"10.3", "12.6", "87"}}));
        m.generations.push_back(
            declare(dir.path(), Decl{"digitv3", "digitv3", "digit_numeric", "detect",
                                     640,
                                     {"0", "1", "2", "3", "4", "5", "6", "7", "8", "9"},
                                     {"10.3", "12.6", "87"}}));
        view.emplace(m, dir.path(), denso::models::active_backend());

        const auto fid = denso::detection::upsert_model(
            db->handle(), catalog_row("float-small", {"Small"}));
        REQUIRE(fid.has_value());
        float_model_id = *fid;
        const auto did = denso::detection::upsert_model(
            db->handle(),
            catalog_row("digitv3",
                        {"0", "1", "2", "3", "4", "5", "6", "7", "8", "9"}));
        REQUIRE(did.has_value());
        digit_model_id = *did;
    }

    QSqlDatabase h() const { return db->handle(); }
};

int row_count(const QSqlDatabase& db, const QString& table) {
    QSqlQuery q(db);
    REQUIRE(q.exec(QStringLiteral("SELECT COUNT(*) FROM %1").arg(table)));
    REQUIRE(q.next());
    return q.value(0).toInt();
}

std::optional<int> single_int(const QSqlDatabase& db, const QString& sql) {
    QSqlQuery q(db);
    REQUIRE(q.exec(sql));
    if (!q.next()) return std::nullopt;
    return q.value(0).toInt();
}

bool has_table(const QSqlDatabase& db, const QString& table) {
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "SELECT 1 FROM sqlite_master WHERE type='table' AND name = ?"));
    q.addBindValue(table);
    REQUIRE(q.exec());
    return q.next();
}

/// Build the `camera` + `camera_area` + detection schema EXACTLY as v11 left it:
/// the v4 CREATEs, the v5/v6/v11 ALTERs, the v7 polygon rebuild and the v10 zone
/// column. Nothing above v11 — no `setup_complete`, no receipt table, no Ball
/// table. This is what a real appliance that has never seen v12 has on disk, and
/// building it statement-by-statement is the point: migrating an already-current
/// database only proves the CREATEs are idempotent, never that the upgrade path
/// works.
void build_v11_schema(const QSqlDatabase& db) {
    const auto run = [&db](const char* sql) {
        QSqlQuery q(db);
        REQUIRE(q.exec(QString::fromUtf8(sql)));
    };
    run("CREATE TABLE settings (key TEXT PRIMARY KEY, value TEXT NOT NULL)");
    run("CREATE TABLE net_config (iface TEXT PRIMARY KEY, mode TEXT NOT NULL,"
        " ip TEXT, prefix INTEGER, gateway TEXT, dns1 TEXT, dns2 TEXT,"
        " ssid TEXT, security TEXT)");
    run("CREATE TABLE camera (id INTEGER PRIMARY KEY, name TEXT NOT NULL,"
        " camera_type TEXT NOT NULL, active INTEGER NOT NULL, cam_index INTEGER,"
        " ip TEXT, rtsp TEXT, username TEXT, width INTEGER NOT NULL,"
        " height INTEGER NOT NULL, fps INTEGER NOT NULL, pitch REAL NOT NULL,"
        " roll REAL NOT NULL, rotation INTEGER NOT NULL)");
    run("ALTER TABLE camera ADD COLUMN password TEXT");
    run("ALTER TABLE camera ADD COLUMN channel INTEGER");
    run("ALTER TABLE camera ADD COLUMN stream INTEGER");
    run("ALTER TABLE camera ADD COLUMN manufacturer TEXT");
    run("ALTER TABLE camera ADD COLUMN areas_need_review INTEGER NOT NULL DEFAULT 0");
    run("CREATE TABLE camera_area (id INTEGER PRIMARY KEY,"
        " camera_id INTEGER NOT NULL REFERENCES camera(id), name TEXT NOT NULL,"
        " points TEXT NOT NULL)");
    run("CREATE INDEX idx_camera_area_camera ON camera_area(camera_id)");
    run("ALTER TABLE camera_area ADD COLUMN zone INTEGER");
    run("CREATE TABLE model (id INTEGER PRIMARY KEY, name TEXT NOT NULL,"
        " filename TEXT NOT NULL UNIQUE, class_names TEXT NOT NULL)");
    run("CREATE TABLE camera_model (id INTEGER PRIMARY KEY,"
        " camera_id INTEGER NOT NULL REFERENCES camera(id),"
        " model_id INTEGER NOT NULL REFERENCES model(id))");
    run("CREATE TABLE camera_model_class (id INTEGER PRIMARY KEY,"
        " camera_model_id INTEGER NOT NULL REFERENCES camera_model(id),"
        " class_id INTEGER NOT NULL, conf REAL NOT NULL)");
    run("CREATE TABLE reading (id INTEGER PRIMARY KEY,"
        " camera_id INTEGER NOT NULL REFERENCES camera(id), ts_ms INTEGER NOT NULL,"
        " value TEXT NOT NULL, conf REAL NOT NULL)");
}

/// Take a fully-migrated database back to the v14 SHAPE by removing exactly what
/// v15 adds. Dropping the two v15 tables is not a simulation of v14 — it IS v14:
/// v14 is defined as every migration up to and including `ball_level_calibration`
/// and nothing after it.
void demote_to_v14(const QSqlDatabase& db) {
    const auto run = [&db](const char* sql) {
        QSqlQuery q(db);
        REQUIRE(q.exec(QString::fromUtf8(sql)));
    };
    run("DROP TABLE IF EXISTS ball_level_zone");
    run("DROP TABLE IF EXISTS ball_level_binding");
    run("PRAGMA user_version = 14");
}

/// Insert a v14-era Ball calibration directly, the way the shipped v14 build did.
void insert_v14_calibration(const QSqlDatabase& db, int64_t camera_id,
                            int64_t model_id, double y_0) {
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "INSERT INTO ball_level_calibration (camera_id, model_id, class_id, conf,"
        " rect_x, rect_y, rect_w, rect_h, y_100, y_0, hold_ms, view_revision)"
        " VALUES (?, ?, 0, 0.55, 0.30, 0.10, 0.40, 0.80, 0.20, ?, 2000, 'rev-v14')"));
    q.addBindValue(static_cast<qlonglong>(camera_id));
    q.addBindValue(static_cast<qlonglong>(model_id));
    q.addBindValue(y_0);
    REQUIRE(q.exec());
}

/// Wrap ONE calibration as the single-zone set the multi-zone chokepoint takes.
/// Most of these cases are about the MODEL binding, not the zone set, so they
/// state the zone set once here rather than restating it at every call.
std::vector<LevelZone> one_zone(const LevelCalibration& c, int zone_no = 1) {
    return {LevelZone{zone_no, c}};
}

bool save_float(const Fixture& fx, int64_t camera_id,
                const LevelCalibration& cal, SaveRefusal* refusal = nullptr) {
    return denso::level::save_level_configuration(
        fx.h(), camera_id, {LevelBinding{fx.float_model_id, {0}}}, one_zone(cal), "rev-1",
        *fx.view, kPlatform, refusal);
}

}  // namespace

// ─── 1-2. migration v13 -> v14 is additive and preserves every digit row ──────

TEST_CASE("schema version is 17", "[level][schema]") {
    CHECK(denso::db::supported_schema_version() == 18);
}

TEST_CASE("v13 -> v18 migrates a populated database and preserves every Digital "
          "Reader row",
          "[level][schema][migration]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString path = QDir(dir.path()).filePath("denso.db");

    // Build a v13 database with real digit-reader content, then stamp it back to
    // v13 so the upgrade path is exercised for real rather than simulated.
    int64_t cam_id = 0;
    int64_t model_id = 0;
    {
        auto db = denso::db::Db::open(path);
        REQUIRE(db.has_value());
        REQUIRE(denso::db::run_migrations(db->handle()));

        const auto cid = denso::camera::insert(db->handle(), ip_camera("Line 1"));
        REQUIRE(cid.has_value());
        cam_id = *cid;
        const auto mid = denso::detection::upsert_model(
            db->handle(), catalog_row("digitv3", {"0", "1", "2"}));
        REQUIRE(mid.has_value());
        model_id = *mid;

        // Attachment + class row + ROI area + a reading, written directly so the
        // fixture does not depend on the compatibility gate.
        QSqlQuery a(db->handle());
        a.prepare(QStringLiteral(
            "INSERT INTO camera_model (camera_id, model_id) VALUES (?, ?)"));
        a.addBindValue(static_cast<qlonglong>(cam_id));
        a.addBindValue(static_cast<qlonglong>(model_id));
        REQUIRE(a.exec());
        const qlonglong cmid = a.lastInsertId().toLongLong();
        QSqlQuery c(db->handle());
        c.prepare(QStringLiteral(
            "INSERT INTO camera_model_class (camera_model_id, class_id, conf) "
            "VALUES (?, ?, ?)"));
        c.addBindValue(cmid);
        c.addBindValue(3);
        c.addBindValue(0.77);
        REQUIRE(c.exec());
        QSqlQuery ar(db->handle());
        ar.prepare(QStringLiteral(
            "INSERT INTO camera_area (camera_id, name, points, zone) "
            "VALUES (?, 'Zone A', '0.1,0.1;0.9,0.1;0.9,0.9', 4)"));
        ar.addBindValue(static_cast<qlonglong>(cam_id));
        REQUIRE(ar.exec());
        QSqlQuery rd(db->handle());
        rd.prepare(QStringLiteral(
            "INSERT INTO reading (camera_id, ts_ms, value, conf) "
            "VALUES (?, 1700000000000, '1234', 0.9)"));
        rd.addBindValue(static_cast<qlonglong>(cam_id));
        REQUIRE(rd.exec());

        REQUIRE(QSqlQuery(db->handle()).exec(QStringLiteral("PRAGMA user_version = 13")));
    }

    // Reopen and migrate: v13 -> v16.
    {
        auto db = denso::db::Db::open(path);
        REQUIRE(db.has_value());
        REQUIRE(denso::db::read_user_version(db->handle()).value_or(-1) == 13);
        REQUIRE(denso::db::run_migrations(db->handle()));
        CHECK(denso::db::read_user_version(db->handle()).value_or(-1) == 18);

        // MUTATION GUARD: every digit row survives, with its values intact.
        CHECK(row_count(db->handle(), "camera") == 1);
        CHECK(row_count(db->handle(), "camera_model") == 1);
        CHECK(row_count(db->handle(), "camera_model_class") == 1);
        CHECK(row_count(db->handle(), "camera_area") == 1);
        CHECK(row_count(db->handle(), "reading") == 1);
        CHECK(row_count(db->handle(), "ball_level_calibration") == 0);
        // The v15 tables are created even with nothing to backfill, so the
        // running app never meets a half-present schema.
        CHECK(has_table(db->handle(), "ball_level_binding"));
        CHECK(has_table(db->handle(), "ball_level_zone"));
        CHECK(row_count(db->handle(), "ball_level_binding") == 0);
        CHECK(row_count(db->handle(), "ball_level_zone") == 0);

        const auto cams = denso::camera::all(db->handle());
        REQUIRE(cams.size() == 1);
        const denso::camera::Camera& c = cams.front();
        const denso::camera::Camera want = ip_camera("Line 1");
        CHECK(c.name == want.name);
        CHECK(c.camera_type == want.camera_type);
        CHECK(c.ip == want.ip);
        CHECK(c.rtsp == want.rtsp);
        CHECK(c.username == want.username);
        CHECK(c.width == want.width);
        CHECK(c.height == want.height);
        CHECK(c.fps == want.fps);
        CHECK(c.rotation == want.rotation);
        CHECK(c.setup_complete);

        QSqlQuery q(db->handle());
        REQUIRE(q.exec(QStringLiteral(
            "SELECT class_id, conf FROM camera_model_class")));
        REQUIRE(q.next());
        CHECK(q.value(0).toInt() == 3);
        CHECK(q.value(1).toDouble() == 0.77);
    }
}

// ─── 2b. the v15 backfill may not assume schema it did not create ────────────
//
// The regression these pin: `migrate_ball_calibration_to_zones` read
// `camera_area.zone` unconditionally. That column arrives in v10, over a table
// created in v4 and rebuilt in v7 — none of which EXECUTES on a database
// entering at v11, because the `version < N` blocks are conditional. The entry
// `user_version` therefore says nothing about what is actually on disk, and a
// migration that infers schema from it fails at the first query. A failed
// `run_migrations` is a bricked upgrade: the appliance refuses to boot.

TEST_CASE("v11 -> v18 upgrades a real legacy database and preserves every row",
          "[level][schema][migration]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString path = QDir(dir.path()).filePath("denso.db");

    {
        auto db = denso::db::Db::open(path);
        REQUIRE(db.has_value());
        build_v11_schema(db->handle());

        const auto run = [&](const char* sql) {
            QSqlQuery q(db->handle());
            REQUIRE(q.exec(QString::fromUtf8(sql)));
        };
        run("INSERT INTO camera (name, camera_type, active, cam_index, width,"
            " height, fps, pitch, roll, rotation) VALUES"
            " ('Legacy Line', 'usb', 1, 0, 1280, 720, 30, 0.0, 0.0, 0)");
        run("INSERT INTO model (name, filename, class_names)"
            " VALUES ('digitv3', 'digitv3.engine', '[\"0\",\"1\"]')");
        run("INSERT INTO camera_model (camera_id, model_id) VALUES (1, 1)");
        run("INSERT INTO camera_model_class (camera_model_id, class_id, conf)"
            " VALUES (1, 7, 0.61)");
        run("INSERT INTO camera_area (camera_id, name, points, zone)"
            " VALUES (1, 'Zone A', '0.1,0.1;0.9,0.1;0.9,0.9', 3)");
        run("INSERT INTO reading (camera_id, ts_ms, value, conf)"
            " VALUES (1, 1700000000000, '4321', 0.88)");
        run("INSERT INTO settings (key, value) VALUES ('backend_url', 'http://x')");
        run("PRAGMA user_version = 11");
    }

    {
        auto db = denso::db::Db::open(path);
        REQUIRE(db.has_value());
        REQUIRE(denso::db::read_user_version(db->handle()).value_or(-1) == 11);

        REQUIRE(denso::db::run_migrations(db->handle()));
        CHECK(denso::db::read_user_version(db->handle()).value_or(-1) == 18);

        // Every v11 row survives, values intact.
        CHECK(row_count(db->handle(), "camera") == 1);
        CHECK(row_count(db->handle(), "camera_model") == 1);
        CHECK(row_count(db->handle(), "camera_model_class") == 1);
        CHECK(row_count(db->handle(), "camera_area") == 1);
        CHECK(row_count(db->handle(), "reading") == 1);
        CHECK(row_count(db->handle(), "settings") == 1);
        CHECK(single_int(db->handle(),
                         QStringLiteral("SELECT zone FROM camera_area")) == 3);
        CHECK(single_int(db->handle(),
                         QStringLiteral("SELECT class_id FROM camera_model_class")) == 7);

        // v12 grandfathering still applies, and the v15 tables now exist and are
        // empty — this machine never had a Ball calibration to carry forward.
        const auto cams = denso::camera::all(db->handle());
        REQUIRE(cams.size() == 1);
    CHECK(cams.front().setup_complete);
        CHECK(has_table(db->handle(), "ball_level_binding"));
        CHECK(has_table(db->handle(), "ball_level_zone"));
        CHECK(row_count(db->handle(), "ball_level_binding") == 0);
        CHECK(row_count(db->handle(), "ball_level_zone") == 0);
    }
}

TEST_CASE("a populated v14 Ball calibration becomes one binding plus one zone",
          "[level][schema][migration]") {
    Fixture fx;
    const QString path = QDir(fx.dir.path()).filePath("denso.db");
    const auto cid = denso::camera::insert(fx.h(), ip_camera("Tank 1"));
    REQUIRE(cid.has_value());
    const int64_t model_id = fx.float_model_id;

    insert_v14_calibration(fx.h(), *cid, model_id, 0.80);
    demote_to_v14(fx.h());
    fx.db.reset();   // close before reopening the same file

    auto db = denso::db::Db::open(path);
    REQUIRE(db.has_value());
    REQUIRE(denso::db::read_user_version(db->handle()).value_or(-1) == 14);
    REQUIRE(denso::db::run_migrations(db->handle()));
    CHECK(denso::db::read_user_version(db->handle()).value_or(-1) == 18);

    // Exactly one binding, carrying the camera-level model identity.
    REQUIRE(row_count(db->handle(), "ball_level_binding") == 1);
    QSqlQuery b(db->handle());
    REQUIRE(b.exec(QStringLiteral(
        "SELECT camera_id, model_id, class_id, view_revision FROM ball_level_binding")));
    REQUIRE(b.next());
    CHECK(b.value(0).toLongLong() == *cid);
    CHECK(b.value(1).toLongLong() == model_id);
    CHECK(b.value(2).toInt() == 0);
    CHECK(b.value(3).toString() == QStringLiteral("rev-v14"));

    // Exactly one zone, at Zone 1 — the documented case for a Ball-only machine
    // with nothing else claiming a number — carrying the v14 geometry verbatim.
    REQUIRE(row_count(db->handle(), "ball_level_zone") == 1);
    QSqlQuery z(db->handle());
    REQUIRE(z.exec(QStringLiteral(
        "SELECT camera_id, zone_no, conf, rect_x, rect_y, rect_w, rect_h,"
        " y_100, y_0, hold_ms FROM ball_level_zone")));
    REQUIRE(z.next());
    CHECK(z.value(0).toLongLong() == *cid);
    CHECK(z.value(1).toInt() == 1);
    CHECK(z.value(2).toDouble() == 0.55);
    CHECK(z.value(3).toDouble() == 0.30);
    CHECK(z.value(4).toDouble() == 0.10);
    CHECK(z.value(5).toDouble() == 0.40);
    CHECK(z.value(6).toDouble() == 0.80);
    CHECK(z.value(7).toDouble() == 0.20);
    CHECK(z.value(8).toDouble() == 0.80);
    CHECK(z.value(9).toInt() == 2000);

    // The legacy row is RETAINED untouched — the migration drops nothing.
    CHECK(row_count(db->handle(), "ball_level_calibration") == 1);
    CHECK(single_int(db->handle(),
                     QStringLiteral("SELECT hold_ms FROM ball_level_calibration")) == 2000);
}

TEST_CASE("a migrated v14 calibration takes the lowest zone number free "
          "MACHINE-WIDE, across both modes",
          "[level][schema][migration][zones]") {
    Fixture fx;
    const QString path = QDir(fx.dir.path()).filePath("denso.db");
    const auto digit_cam = denso::camera::insert(fx.h(), ip_camera("Digit line"));
    const auto ball_a = denso::camera::insert(fx.h(), ip_camera("Tank A"));
    const auto ball_b = denso::camera::insert(fx.h(), ip_camera("Tank B"));
    REQUIRE(digit_cam.has_value());
    REQUIRE(ball_a.has_value());
    REQUIRE(ball_b.has_value());

    // The digit reader already reports zones 1 and 3. `build_brazing_payload`
    // keys by zone number ALONE and carries no camera identity, so handing a
    // migrated Ball camera zone 1 would silently make two cameras write one
    // backend field.
    const auto claim = [&](int64_t cam, int zone) {
        QSqlQuery q(fx.h());
        q.prepare(QStringLiteral(
            "INSERT INTO camera_area (camera_id, name, points, zone)"
            " VALUES (?, 'A', '0.1,0.1;0.9,0.1;0.9,0.9', ?)"));
        q.addBindValue(static_cast<qlonglong>(cam));
        q.addBindValue(zone);
        REQUIRE(q.exec());
    };
    claim(*digit_cam, 1);
    claim(*digit_cam, 3);

    insert_v14_calibration(fx.h(), *ball_a, fx.float_model_id, 0.80);
    insert_v14_calibration(fx.h(), *ball_b, fx.float_model_id, 0.75);
    demote_to_v14(fx.h());
    fx.db.reset();

    auto db = denso::db::Db::open(path);
    REQUIRE(db.has_value());
    REQUIRE(denso::db::run_migrations(db->handle()));

    // 1 and 3 are taken by the digit ROIs, so the two Ball cameras take 2 and 4
    // in camera_id order. The digit claims are untouched.
    QSqlQuery z(db->handle());
    REQUIRE(z.exec(QStringLiteral(
        "SELECT camera_id, zone_no FROM ball_level_zone ORDER BY camera_id")));
    REQUIRE(z.next());
    CHECK(z.value(0).toLongLong() == *ball_a);
    CHECK(z.value(1).toInt() == 2);
    REQUIRE(z.next());
    CHECK(z.value(0).toLongLong() == *ball_b);
    CHECK(z.value(1).toInt() == 4);
    CHECK_FALSE(z.next());

    CHECK(row_count(db->handle(), "camera_area") == 2);
    QSqlQuery a(db->handle());
    REQUIRE(a.exec(QStringLiteral("SELECT zone FROM camera_area ORDER BY zone")));
    REQUIRE(a.next());
    CHECK(a.value(0).toInt() == 1);
    REQUIRE(a.next());
    CHECK(a.value(0).toInt() == 3);

    // No number is claimed twice anywhere on the machine.
    CHECK(single_int(db->handle(), QStringLiteral(
              "SELECT COUNT(*) FROM (SELECT zone AS z FROM camera_area"
              " WHERE zone IS NOT NULL UNION ALL"
              " SELECT zone_no FROM ball_level_zone) GROUP BY z"
              " HAVING COUNT(*) > 1")) == std::nullopt);
}

TEST_CASE("re-running the migration after a restart adds no second zone",
          "[level][schema][migration][idempotent]") {
    Fixture fx;
    const QString path = QDir(fx.dir.path()).filePath("denso.db");
    const auto cid = denso::camera::insert(fx.h(), ip_camera("Tank 1"));
    REQUIRE(cid.has_value());
    insert_v14_calibration(fx.h(), *cid, fx.float_model_id, 0.80);
    demote_to_v14(fx.h());
    fx.db.reset();

    {
        auto db = denso::db::Db::open(path);
        REQUIRE(db.has_value());
        REQUIRE(denso::db::run_migrations(db->handle()));
        REQUIRE(row_count(db->handle(), "ball_level_zone") == 1);
    }

    // Restart: the appliance runs migrations on EVERY boot.
    for (int boot = 0; boot < 2; ++boot) {
        auto db = denso::db::Db::open(path);
        REQUIRE(db.has_value());
        REQUIRE(denso::db::run_migrations(db->handle()));
        CHECK(row_count(db->handle(), "ball_level_binding") == 1);
        CHECK(row_count(db->handle(), "ball_level_zone") == 1);
        CHECK(single_int(db->handle(),
                         QStringLiteral("SELECT zone_no FROM ball_level_zone")) == 1);
    }

    // And a re-entry that finds the stamp MISSING — an earlier run interrupted
    // between the inserts and the `PRAGMA user_version` write — must still not
    // hand this camera a second tank. INSERT OR IGNORE only ignores a collision
    // on (camera_id, zone_no), so re-deriving a DIFFERENT number would insert,
    // not ignore.
    {
        auto db = denso::db::Db::open(path);
        REQUIRE(db.has_value());
        QSqlQuery q(db->handle());
        REQUIRE(q.exec(QStringLiteral("PRAGMA user_version = 14")));
        REQUIRE(denso::db::run_migrations(db->handle()));
        CHECK(row_count(db->handle(), "ball_level_zone") == 1);
        CHECK(single_int(db->handle(),
                         QStringLiteral("SELECT zone_no FROM ball_level_zone")) == 1);
    }
}

TEST_CASE("a migration interrupted before the version stamp resumes on restart",
          "[level][schema][migration][idempotent]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString path = QDir(dir.path()).filePath("denso.db");

    // A v11 database on which the upgrade got as far as v12's ALTER and then lost
    // power. The column is added; `user_version` is still 11, because it is
    // stamped ONCE at the very end of run_migrations.
    {
        auto db = denso::db::Db::open(path);
        REQUIRE(db.has_value());
        build_v11_schema(db->handle());
        const auto run = [&](const char* sql) {
            QSqlQuery q(db->handle());
            REQUIRE(q.exec(QString::fromUtf8(sql)));
        };
        run("INSERT INTO camera (name, camera_type, active, cam_index, width,"
            " height, fps, pitch, roll, rotation) VALUES"
            " ('Survivor', 'usb', 1, 0, 1280, 720, 30, 0.0, 0.0, 0)");
        run("ALTER TABLE camera ADD COLUMN setup_complete INTEGER NOT NULL DEFAULT 1");
        run("PRAGMA user_version = 11");
    }

    // ALTER TABLE ADD COLUMN has no IF NOT EXISTS form, so re-running v12's
    // statement raises "duplicate column name" and, unguarded, fails the whole
    // migration — the appliance would refuse to boot on a database that is fine.
    auto db = denso::db::Db::open(path);
    REQUIRE(db.has_value());
    REQUIRE(denso::db::run_migrations(db->handle()));
    CHECK(denso::db::read_user_version(db->handle()).value_or(-1) == 18);

    const auto cams = denso::camera::all(db->handle());
    REQUIRE(cams.size() == 1);
    CHECK(cams.front().name == "Survivor");
    CHECK(cams.front().setup_complete);
}

TEST_CASE("the v15 block repairs the digit zone schema it depends on",
          "[level][schema][migration]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString path = QDir(dir.path()).filePath("denso.db");

    // A v11-stamped database whose camera_area never made it — the shape the
    // conditional `version < N` blocks can leave behind. The backfill needs
    // camera_area.zone to know which numbers the digit reader owns, so v15
    // ENSURES it rather than reading its absence as "nothing is claimed": that
    // reading would hand migrated Ball cameras numbers digit ROIs may own.
    {
        auto db = denso::db::Db::open(path);
        REQUIRE(db.has_value());
        const auto run = [&](const char* sql) {
            QSqlQuery q(db->handle());
            REQUIRE(q.exec(QString::fromUtf8(sql)));
        };
        run("CREATE TABLE camera (id INTEGER PRIMARY KEY, name TEXT NOT NULL,"
            " camera_type TEXT NOT NULL, active INTEGER NOT NULL,"
            " cam_index INTEGER, ip TEXT, rtsp TEXT, username TEXT,"
            " width INTEGER NOT NULL, height INTEGER NOT NULL, fps INTEGER NOT NULL,"
            " pitch REAL NOT NULL, roll REAL NOT NULL, rotation INTEGER NOT NULL)");
        run("ALTER TABLE camera ADD COLUMN areas_need_review INTEGER NOT NULL DEFAULT 0");
        run("PRAGMA user_version = 11");
    }

    auto db = denso::db::Db::open(path);
    REQUIRE(db.has_value());
    REQUIRE(denso::db::run_migrations(db->handle()));
    CHECK(denso::db::read_user_version(db->handle()).value_or(-1) == 18);

    // Repaired, not merely tolerated: the table AND its zone column are there,
    // so the digit reader has somewhere to record a claim after the upgrade.
    REQUIRE(has_table(db->handle(), "camera_area"));
    QSqlQuery q(db->handle());
    REQUIRE(q.exec(QStringLiteral("PRAGMA table_info(camera_area)")));
    bool has_zone = false;
    while (q.next()) {
        if (q.value(1).toString() == QStringLiteral("zone")) has_zone = true;
    }
    CHECK(has_zone);
}

TEST_CASE("a Ball save refuses when the zone-ownership question cannot be answered",
          "[level][zones][fail-closed]") {
    Fixture fx;
    const auto cid = denso::camera::insert(fx.h(), ip_camera("Tank 1"));
    REQUIRE(cid.has_value());

    // Break the OTHER mode's half of the one ownership query. The picker may
    // fail open — it only greys out numbers — but a SAVE that reads "nothing is
    // taken" from a database it could not read would grant a number another
    // camera already reports, and zone numbers are the backend payload keys.
    {
        QSqlQuery q(fx.h());
        REQUIRE(q.exec(QStringLiteral("DROP TABLE ball_level_zone")));
    }
    CHECK_FALSE(denso::camera::try_zones_owned_by_other_cameras(fx.h(), *cid)
                    .has_value());
    // The picker overload still answers, so the page can render.
    CHECK(denso::camera::zones_owned_by_other_cameras(fx.h(), *cid).empty());

    denso::level::SaveRefusal refusal;
    CHECK_FALSE(save_float(fx, *cid, good_calibration(), &refusal));
    // A write failure, NOT an invented policy reason.
    CHECK(refusal.reason_code.empty());
}

// ─── 3. one Ball Leveler row per camera, enforced by the schema ───────────────

TEST_CASE("ball_level_calibration accepts only one row per camera",
          "[level][schema][one-per-camera]") {
    Fixture fx;
    const auto cid = denso::camera::insert(fx.h(), ip_camera("Tank 1"));
    REQUIRE(cid.has_value());

    // MUTATION GUARD: a second row for the same camera must be impossible at the
    // SQL layer, so no caller can create a second measurement by bypassing the
    // repository.
    const auto insert_raw = [&](double y0) {
        QSqlQuery q(fx.h());
        q.prepare(QStringLiteral(
            "INSERT INTO ball_level_calibration (camera_id, model_id, class_id, "
            "conf, rect_x, rect_y, rect_w, rect_h, y_100, y_0, hold_ms, "
            "view_revision) VALUES (?, ?, 0, 0.5, 0.3, 0.1, 0.4, 0.8, 0.2, ?, "
            "2000, 'rev-1')"));
        q.addBindValue(static_cast<qlonglong>(*cid));
        q.addBindValue(static_cast<qlonglong>(fx.float_model_id));
        q.addBindValue(y0);
        return q.exec();
    };
    CHECK(insert_raw(0.80));
    CHECK_FALSE(insert_raw(0.70));      // PRIMARY KEY conflict
    // The LEGACY v14 table, deliberately retained and untouched by v15 — this
    // still pins its one-row-per-camera shape, because the v15 backfill reads it.
    CHECK(row_count(fx.h(), "ball_level_calibration") == 1);
}

// ─── 3b. one MODEL per camera, many zones, enforced by the schema ─────────────

TEST_CASE("ball_level_binding is one row per camera and ball_level_zone is keyed "
          "by (camera_id, zone_no)",
          "[level][schema][one-per-camera]") {
    Fixture fx;
    const auto cid = denso::camera::insert(fx.h(), ip_camera("Tank 1"));
    REQUIRE(cid.has_value());

    // MUTATION GUARD: a second MODEL for one camera must be impossible at the SQL
    // layer. "One camera, one Float model" is then true by CONSTRUCTION, not by a
    // rule a caller can forget — which is exactly why the model lives in its own
    // table instead of being repeated in every zone row.
    const auto bind_raw = [&](int64_t model_id) {
        QSqlQuery q(fx.h());
        q.prepare(QStringLiteral(
            "INSERT INTO ball_level_binding (camera_id, model_id, class_id, "
            "view_revision) VALUES (?, ?, 0, 'rev-1')"));
        q.addBindValue(static_cast<qlonglong>(*cid));
        q.addBindValue(static_cast<qlonglong>(model_id));
        return q.exec();
    };
    CHECK(bind_raw(fx.float_model_id));
    CHECK_FALSE(bind_raw(fx.digit_model_id));   // PRIMARY KEY conflict
    CHECK(row_count(fx.h(), "ball_level_binding") == 1);

    const auto zone_raw = [&](int zone_no) {
        QSqlQuery q(fx.h());
        q.prepare(QStringLiteral(
            "INSERT INTO ball_level_zone (camera_id, zone_no, conf, rect_x, "
            "rect_y, rect_w, rect_h, y_100, y_0, hold_ms) "
            "VALUES (?, ?, 0.5, 0.3, 0.1, 0.4, 0.8, 0.2, 0.8, 2000)"));
        q.addBindValue(static_cast<qlonglong>(*cid));
        q.addBindValue(zone_no);
        return q.exec();
    };
    // Several zones for ONE camera are the point of v15...
    CHECK(zone_raw(1));
    CHECK(zone_raw(2));
    CHECK(zone_raw(3));
    // ...but the same zone number twice on one camera is not.
    CHECK_FALSE(zone_raw(2));
    CHECK(row_count(fx.h(), "ball_level_zone") == 3);
}

TEST_CASE("the calibration table is keyed by camera, not by a surrogate id",
          "[level][schema][camera-key]") {
    Fixture fx;
    // MUTATION GUARD: a surrogate `id INTEGER PRIMARY KEY` with camera_id as an
    // ordinary column would let two rows coexist. Assert the key is camera_id.
    QSqlQuery q(fx.h());
    REQUIRE(q.exec(QStringLiteral("PRAGMA table_info(ball_level_calibration)")));
    bool camera_id_is_pk = false;
    int pk_columns = 0;
    while (q.next()) {
        const QString name = q.value(1).toString();
        const int pk = q.value(5).toInt();
        if (pk > 0) {
            ++pk_columns;
            if (name == QLatin1String("camera_id")) camera_id_is_pk = true;
        }
        CHECK(name != QLatin1String("id"));
    }
    CHECK(camera_id_is_pk);
    CHECK(pk_columns == 1);
}

// ─── 4. restart persistence ──────────────────────────────────────────────────

TEST_CASE("a saved Ball Leveler configuration survives a restart",
          "[level][restart]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString path = QDir(dir.path()).filePath("denso.db");
    int64_t cam_id = 0;
    int64_t model_id = 0;

    Manifest m;
    m.schema = 2;
    m.generations.push_back(
        declare(dir.path(), Decl{"float-small", "float-small", "float_ball"}));
    ManifestView view(m, dir.path(), denso::models::active_backend());

    {
        auto db = denso::db::Db::open(path);
        REQUIRE(db.has_value());
        REQUIRE(denso::db::run_migrations(db->handle()));
        const auto cid = denso::camera::insert(db->handle(), ip_camera("Tank 1"));
        REQUIRE(cid.has_value());
        cam_id = *cid;
        const auto mid = denso::detection::upsert_model(
            db->handle(), catalog_row("float-small", {"Small"}));
        REQUIRE(mid.has_value());
        model_id = *mid;
        REQUIRE(denso::level::save_level_configuration(
            db->handle(), cam_id, {LevelBinding{model_id, {0}}}, one_zone(good_calibration()),
            "rev-7", view, kPlatform, nullptr));
    }
    {
        // A whole new process would reopen and re-migrate; do exactly that.
        auto db = denso::db::Db::open(path);
        REQUIRE(db.has_value());
        REQUIRE(denso::db::run_migrations(db->handle()));
        const auto got = denso::level::level_config_for(db->handle(), cam_id);
        REQUIRE(got.has_value());
        CHECK(got->camera_id == cam_id);
        CHECK(got->model_id == model_id);
        CHECK(got->class_id == 0);
        CHECK(got->view_revision == "rev-7");
        CHECK(got->zones.at(0).calibration.y_100 == good_calibration().y_100);
        CHECK(got->zones.at(0).calibration.y_0 == good_calibration().y_0);
        CHECK(got->zones.at(0).calibration.rect_w == good_calibration().rect_w);
        CHECK(got->zones.at(0).calibration.conf == good_calibration().conf);
        CHECK(got->zones.at(0).calibration.hold_ms == good_calibration().hold_ms);
    }
}

// ─── 5-13. the save chokepoint ───────────────────────────────────────────────

TEST_CASE("save_level_configuration accepts one compatible Float model with one "
          "class",
          "[level][save]") {
    Fixture fx;
    const auto cid = denso::camera::insert(fx.h(), ip_camera("Tank 1"));
    REQUIRE(cid.has_value());
    CHECK(save_float(fx, *cid, good_calibration()));
    CHECK(row_count(fx.h(), "ball_level_zone") == 1);
    // The binding lives ONLY here — never in camera_model.
    CHECK(row_count(fx.h(), "camera_model") == 0);
}

TEST_CASE("save_level_configuration rejects digitv3 in ball_leveler",
          "[level][save][compat]") {
    Fixture fx;
    const auto cid = denso::camera::insert(fx.h(), ip_camera("Tank 1"));
    REQUIRE(cid.has_value());
    SaveRefusal refusal;
    // MUTATION GUARD: the digit model must be refused by the CENTRAL policy.
    CHECK_FALSE(denso::level::save_level_configuration(
        fx.h(), *cid, {LevelBinding{fx.digit_model_id, {0}}}, one_zone(good_calibration()),
        "rev-1", *fx.view, kPlatform, &refusal));
    CHECK(refusal.reason_code == "model_mode_incompatible");
    CHECK(refusal.canonical_id == "digitv3");
    CHECK(row_count(fx.h(), "ball_level_zone") == 0);
}

TEST_CASE("save_level_configuration rejects an undeclared or unknown model",
          "[level][save][compat]") {
    Fixture fx;
    const auto cid = denso::camera::insert(fx.h(), ip_camera("Tank 1"));
    REQUIRE(cid.has_value());

    SECTION("a catalogued model the manifest does not declare") {
        const auto mid = denso::detection::upsert_model(
            fx.h(), catalog_row("mystery", {"X"}));
        REQUIRE(mid.has_value());
        SaveRefusal refusal;
        CHECK_FALSE(denso::level::save_level_configuration(
            fx.h(), *cid, {LevelBinding{*mid, {0}}}, one_zone(good_calibration()), "rev-1",
            *fx.view, kPlatform, &refusal));
        CHECK(refusal.reason_code == "model_undeclared");
    }
    SECTION("a model_id with no catalog row at all") {
        SaveRefusal refusal;
        CHECK_FALSE(denso::level::save_level_configuration(
            fx.h(), *cid, {LevelBinding{999999, {0}}}, one_zone(good_calibration()), "rev-1",
            *fx.view, kPlatform, &refusal));
        CHECK(refusal.reason_code == "model_undeclared");
    }
    CHECK(row_count(fx.h(), "ball_level_zone") == 0);
}

TEST_CASE("save_level_configuration rejects a provenance-failed model",
          "[level][save][compat]") {
    Fixture fx;
    const auto cid = denso::camera::insert(fx.h(), ip_camera("Tank 1"));
    REQUIRE(cid.has_value());
    // Physically corrupt the declared artifact so its recorded hash no longer
    // matches — a real provenance fault, not an edited field.
    const QString p =
        QDir(fx.dir.path()).filePath(QString::fromLatin1("float-small") + kExt);
    QFile f(p);
    REQUIRE(f.open(QIODevice::WriteOnly));
    REQUIRE(f.write("tampered-bytes-of-different-length") > 0);
    f.close();

    SaveRefusal refusal;
    CHECK_FALSE(save_float(fx, *cid, good_calibration(), &refusal));
    CHECK(refusal.reason_code == "model_provenance_failed");
    CHECK(row_count(fx.h(), "ball_level_zone") == 0);
}

TEST_CASE("save_level_configuration requires exactly one model",
          "[level][save][arity]") {
    Fixture fx;
    const auto cid = denso::camera::insert(fx.h(), ip_camera("Tank 1"));
    REQUIRE(cid.has_value());

    SECTION("zero models") {
        SaveRefusal refusal;
        CHECK_FALSE(denso::level::save_level_configuration(
            fx.h(), *cid, {}, one_zone(good_calibration()), "rev-1",
            *fx.view, kPlatform, &refusal));
        CHECK(refusal.reason_code == "level_model_count");
    }
    SECTION("two models") {
        // MUTATION GUARD: an ensemble must not be representable here.
        SaveRefusal refusal;
        CHECK_FALSE(denso::level::save_level_configuration(
            fx.h(), *cid,
            {LevelBinding{fx.float_model_id, {0}}, LevelBinding{fx.float_model_id, {0}}},
            one_zone(good_calibration()), "rev-1", *fx.view,
            kPlatform, &refusal));
        CHECK(refusal.reason_code == "level_model_count");
    }
    CHECK(row_count(fx.h(), "ball_level_zone") == 0);
}

TEST_CASE("save_level_configuration requires exactly one class",
          "[level][save][arity]") {
    Fixture fx;
    const auto cid = denso::camera::insert(fx.h(), ip_camera("Tank 1"));
    REQUIRE(cid.has_value());

    SECTION("zero classes") {
        SaveRefusal refusal;
        CHECK_FALSE(denso::level::save_level_configuration(
            fx.h(), *cid, {LevelBinding{fx.float_model_id, {}}}, one_zone(good_calibration()),
            "rev-1", *fx.view, kPlatform, &refusal));
        CHECK(refusal.reason_code == "level_class_count");
    }
    SECTION("two classes") {
        SaveRefusal refusal;
        CHECK_FALSE(denso::level::save_level_configuration(
            fx.h(), *cid, {LevelBinding{fx.float_model_id, {0, 1}}},
            one_zone(good_calibration()), "rev-1", *fx.view,
            kPlatform, &refusal));
        CHECK(refusal.reason_code == "level_class_count");
    }
    CHECK(row_count(fx.h(), "ball_level_zone") == 0);
}

TEST_CASE("save_level_configuration rejects invalid calibration geometry",
          "[level][save][geometry]") {
    Fixture fx;
    const auto cid = denso::camera::insert(fx.h(), ip_camera("Tank 1"));
    REQUIRE(cid.has_value());

    struct Bad {
        const char* what;
        const char* reason;
        LevelCalibration cal;
    };
    std::vector<Bad> cases;
    {   // MUTATION GUARD: reversed reference lines must never be accepted.
        LevelCalibration c = good_calibration();
        c.y_100 = 0.80;
        c.y_0 = 0.20;
        cases.push_back({"reversed lines", "calib_lines_reversed", c});
    }
    {   LevelCalibration c = good_calibration();
        c.y_100 = 0.50;
        c.y_0 = 0.50;
        cases.push_back({"coincident lines", "calib_lines_reversed", c});
    }
    {   LevelCalibration c = good_calibration();
        c.y_100 = 0.500;
        c.y_0 = 0.505;
        cases.push_back({"span too small", "calib_span_too_small", c});
    }
    {   LevelCalibration c = good_calibration();
        c.rect_w = 0.0;
        cases.push_back({"zero-width rect", "calib_rect_degenerate", c});
    }
    {   LevelCalibration c = good_calibration();
        c.rect_h = -0.5;
        cases.push_back({"negative-height rect", "calib_rect_degenerate", c});
    }
    {   LevelCalibration c = good_calibration();
        c.y_100 = 0.05;   // above rect_y == 0.10
        cases.push_back({"100% line outside rect", "calib_line_outside_rect", c});
    }
    {   LevelCalibration c = good_calibration();
        c.y_0 = 0.95;     // below rect_y + rect_h == 0.90
        cases.push_back({"0% line outside rect", "calib_line_outside_rect", c});
    }
    {   LevelCalibration c = good_calibration();
        c.y_0 = std::numeric_limits<double>::quiet_NaN();
        cases.push_back({"NaN line", "calib_not_finite", c});
    }
    {   LevelCalibration c = good_calibration();
        c.rect_h = std::numeric_limits<double>::infinity();
        cases.push_back({"infinite rect", "calib_not_finite", c});
    }
    {   LevelCalibration c = good_calibration();
        c.conf = 1.5;
        cases.push_back({"confidence above 1", "calib_conf_out_of_range", c});
    }
    {   LevelCalibration c = good_calibration();
        c.hold_ms = -1;
        cases.push_back({"negative hold", "calib_hold_invalid", c});
    }

    for (const Bad& b : cases) {
        SaveRefusal refusal;
        CHECK_FALSE(save_float(fx, *cid, b.cal, &refusal));
        CHECK(refusal.reason_code == std::string(b.reason));
        CHECK(row_count(fx.h(), "ball_level_zone") == 0);
    }
}

TEST_CASE("a rejected save performs no partial write and leaves an existing "
          "configuration untouched",
          "[level][save][rollback]") {
    Fixture fx;
    const auto cid = denso::camera::insert(fx.h(), ip_camera("Tank 1"));
    REQUIRE(cid.has_value());
    REQUIRE(save_float(fx, *cid, good_calibration()));

    // MUTATION GUARD: a refused overwrite must not delete or partially rewrite
    // the good row that is already there.
    LevelCalibration bad = good_calibration();
    bad.y_100 = 0.9;
    bad.y_0 = 0.1;
    CHECK_FALSE(save_float(fx, *cid, bad));

    const auto got = denso::level::level_config_for(fx.h(), *cid);
    REQUIRE(got.has_value());
    CHECK(got->zones.at(0).calibration.y_100 == good_calibration().y_100);
    CHECK(got->zones.at(0).calibration.y_0 == good_calibration().y_0);
    CHECK(row_count(fx.h(), "ball_level_zone") == 1);
}

// ─── 14-17. destructive switch_and_reset ────────────────────────────────────

TEST_CASE("switch_and_reset clears both modes' configuration and keeps every "
          "camera connection",
          "[level][switch][preserve]") {
    Fixture fx;
    const auto cid = denso::camera::insert(fx.h(), ip_camera("Line 1"));
    REQUIRE(cid.has_value());

    // Digit-side configuration, written directly (the gate is not under test).
    QSqlQuery a(fx.h());
    a.prepare(QStringLiteral(
        "INSERT INTO camera_model (camera_id, model_id) VALUES (?, ?)"));
    a.addBindValue(static_cast<qlonglong>(*cid));
    a.addBindValue(static_cast<qlonglong>(fx.digit_model_id));
    REQUIRE(a.exec());
    const qlonglong cmid = a.lastInsertId().toLongLong();
    QSqlQuery c(fx.h());
    c.prepare(QStringLiteral(
        "INSERT INTO camera_model_class (camera_model_id, class_id, conf) "
        "VALUES (?, 5, 0.61)"));
    c.addBindValue(cmid);
    REQUIRE(c.exec());
    QSqlQuery ar(fx.h());
    ar.prepare(QStringLiteral(
        "INSERT INTO camera_area (camera_id, name, points, zone) "
        "VALUES (?, 'Zone A', '0.1,0.1;0.9,0.1;0.9,0.9', 4)"));
    ar.addBindValue(static_cast<qlonglong>(*cid));
    REQUIRE(ar.exec());
    QSqlQuery rd(fx.h());
    rd.prepare(QStringLiteral(
        "INSERT INTO reading (camera_id, ts_ms, value, conf) "
        "VALUES (?, 1700000000000, '1234', 0.9)"));
    rd.addBindValue(static_cast<qlonglong>(*cid));
    REQUIRE(rd.exec());

    // Ball-side calibration.
    REQUIRE(save_float(fx, *cid, good_calibration()));

    // brazing::save returns void (write errors are deliberately swallowed there),
    // so assert the round-trip instead of a return value.
    denso::brazing::save(
        fx.h(), denso::brazing::BrazingConfig{true, "http://backend.example/api"});
    REQUIRE(denso::brazing::load(fx.h()).enabled);

    // digit_reader -> ball_leveler
    const auto r1 = denso::mode::switch_and_reset(fx.h(), TargetMode::BallLeveler);
    REQUIRE(r1.ok);
    CHECK(denso::mode::load(fx.h()) == TargetMode::BallLeveler);

    // MUTATION GUARD: nothing may be deleted by a switch.
    // The camera ROW survives; everything that CONFIGURED it does not. Both
    // modes are cleared, so the destination genuinely opens unconfigured rather
    // than inheriting a binding from two switches ago.
    CHECK(row_count(fx.h(), "camera") == 1);
    CHECK(row_count(fx.h(), "camera_model") == 0);
    CHECK(row_count(fx.h(), "camera_model_class") == 0);
    CHECK(row_count(fx.h(), "camera_area") == 0);
    CHECK(row_count(fx.h(), "reading") == 0);
    CHECK(row_count(fx.h(), "ball_level_zone") == 0);
    CHECK(row_count(fx.h(), "ball_level_binding") == 0);
    CHECK(row_count(fx.h(), "ball_level_calibration") == 0);

    // Camera connection columns and setup flags survive untouched.
    const auto cams = denso::camera::all(fx.h());
    REQUIRE(cams.size() == 1);
    CHECK(cams.front().rtsp == ip_camera("Line 1").rtsp);
    CHECK(cams.front().username == ip_camera("Line 1").username);
    CHECK(cams.front().rotation == 90);
    CHECK_FALSE(cams.front().setup_complete);  // reset: the mode starts unconfigured

    // Reporting disabled, address kept.
    const auto bz = denso::brazing::load(fx.h());
    CHECK_FALSE(bz.enabled);
    CHECK(bz.base_url == "http://backend.example/api");

    // ball_leveler -> digit_reader: the Ball calibration must survive too.
    const auto r2 = denso::mode::switch_and_reset(fx.h(), TargetMode::DigitReader);
    REQUIRE(r2.ok);
    CHECK(denso::mode::load(fx.h()) == TargetMode::DigitReader);
    // Switching BACK restores nothing: there is no round trip, by design.
    CHECK(row_count(fx.h(), "ball_level_zone") == 0);
    CHECK(row_count(fx.h(), "camera_model") == 0);
    CHECK(row_count(fx.h(), "camera_area") == 0);
    // The Ball calibration is gone with the rest: nothing survives to be read
    // back, which is exactly what the confirmation told the operator.
    const auto still = denso::level::level_config_for(fx.h(), *cid);
    CHECK_FALSE(still.has_value());
}

TEST_CASE("a failed mode-switch transaction leaves the previous mode unchanged",
          "[level][switch][rollback]") {
    Fixture fx;
    REQUIRE(denso::mode::save(fx.h(), TargetMode::DigitReader));

    // Break the settings table so the mode write inside the transaction fails.
    // MUTATION GUARD: mode.target must be written INSIDE the transaction, so a
    // failure cannot leave the persisted mode advanced.
    REQUIRE(QSqlQuery(fx.h()).exec(QStringLiteral("DROP TABLE settings")));
    const auto r = denso::mode::switch_and_reset(fx.h(), TargetMode::BallLeveler);
    CHECK_FALSE(r.ok);
    CHECK_FALSE(r.error.empty());

    REQUIRE(QSqlQuery(fx.h()).exec(QStringLiteral(
        "CREATE TABLE settings (key TEXT PRIMARY KEY, value TEXT NOT NULL)")));
    // Nothing was committed, so the mode resolves to the fail-safe default.
    CHECK(denso::mode::load(fx.h()) == TargetMode::DigitReader);
}

// ─── 18. mode_setup_required is driven by real calibration ───────────────────

TEST_CASE("mode_setup_required for ball_leveler reflects calibration rather than "
          "always returning true",
          "[level][setup-required]") {
    Fixture fx;
    const auto cid = denso::camera::insert(fx.h(), ip_camera("Tank 1"));
    REQUIRE(cid.has_value());

    // MUTATION GUARD: the old build hardcoded `true` for ball_leveler forever.
    const auto before =
        denso::mode::mode_setup_required(fx.h(), TargetMode::BallLeveler);
    REQUIRE(before.has_value());
    CHECK(*before);

    REQUIRE(save_float(fx, *cid, good_calibration()));
    const auto after =
        denso::mode::mode_setup_required(fx.h(), TargetMode::BallLeveler);
    REQUIRE(after.has_value());
    CHECK_FALSE(*after);
}

// ─── 19-20. inactive-mode configuration never degrades the active mode ───────

TEST_CASE("a dormant Digital Reader attachment does not degrade a configured "
          "ball_leveler appliance",
          "[level][integrity][isolation]") {
    Fixture fx;
    const auto cid = denso::camera::insert(fx.h(), ip_camera("Tank 1"));
    REQUIRE(cid.has_value());

    // Dormant digit attachment (valid for digit_reader, wrong for ball_leveler).
    QSqlQuery a(fx.h());
    a.prepare(QStringLiteral(
        "INSERT INTO camera_model (camera_id, model_id) VALUES (?, ?)"));
    a.addBindValue(static_cast<qlonglong>(*cid));
    a.addBindValue(static_cast<qlonglong>(fx.digit_model_id));
    REQUIRE(a.exec());

    REQUIRE(save_float(fx, *cid, good_calibration()));
    REQUIRE(denso::mode::save(fx.h(), TargetMode::BallLeveler));

    // MUTATION GUARD: judging the dormant digit row against ball_leveler would
    // report model_mode_incompatible and degrade a healthy appliance.
    const auto v = denso::health::evaluate_integrity(
        fx.h(), fx.dir.path(), TargetMode::BallLeveler, *fx.view, kPlatform);
    CHECK(v.status == Readiness::Ready);
    CHECK(v.issues.empty());
}

TEST_CASE("a dormant Ball Leveler calibration does not degrade a configured "
          "digit_reader appliance",
          "[level][integrity][isolation]") {
    Fixture fx;
    const auto cid = denso::camera::insert(fx.h(), ip_camera("Line 1"));
    REQUIRE(cid.has_value());

    REQUIRE(save_float(fx, *cid, good_calibration()));   // dormant Ball config

    // Valid digit attachment for the active mode.
    QSqlQuery a(fx.h());
    a.prepare(QStringLiteral(
        "INSERT INTO camera_model (camera_id, model_id) VALUES (?, ?)"));
    a.addBindValue(static_cast<qlonglong>(*cid));
    a.addBindValue(static_cast<qlonglong>(fx.digit_model_id));
    REQUIRE(a.exec());
    REQUIRE(denso::mode::save(fx.h(), TargetMode::DigitReader));

    const auto v = denso::health::evaluate_integrity(
        fx.h(), fx.dir.path(), TargetMode::DigitReader, *fx.view, kPlatform);
    CHECK(v.status == Readiness::Ready);
    CHECK(v.issues.empty());
}

TEST_CASE("a ball_leveler camera with missing calibration is Degraded, not "
          "Blocked",
          "[level][integrity][degraded]") {
    Fixture fx;
    const auto cid = denso::camera::insert(fx.h(), ip_camera("Tank 1"));
    REQUIRE(cid.has_value());
    REQUIRE(denso::mode::save(fx.h(), TargetMode::BallLeveler));

    const auto v = denso::health::evaluate_integrity(
        fx.h(), fx.dir.path(), TargetMode::BallLeveler, *fx.view, kPlatform);
    CHECK(v.status == Readiness::Degraded);
    CHECK(denso::health::exit_code_for(v.status) == 10);
}

// ═══════════════════════════════════════════════════════════════════════════
// Codex blockers 1-4. Each TEST_CASE below names the mutation it kills.
// ═══════════════════════════════════════════════════════════════════════════

// ─── Blocker 1. A failed query is NOT an absent row ──────────────────────────

TEST_CASE("try_level_config_for distinguishes no-row, a row, and query failure",
          "[level][fallible][blocker1]") {
    Fixture fx;
    const auto cid = denso::camera::insert(fx.h(), ip_camera("Tank 1"));
    REQUIRE(cid.has_value());

    // (a) query succeeds, NO row -> outer engaged, inner empty.
    const auto none = denso::level::try_level_config_for(fx.h(), *cid);
    REQUIRE(none.has_value());          // the query itself worked
    CHECK_FALSE(none->has_value());     // ...and honestly reported "no row"

    // (b) query succeeds, a row -> outer engaged, inner engaged.
    REQUIRE(save_float(fx, *cid, good_calibration()));
    const auto some = denso::level::try_level_config_for(fx.h(), *cid);
    REQUIRE(some.has_value());
    REQUIRE(some->has_value());
    CHECK((*some)->camera_id == *cid);

    // (c) query FAILS -> outer disengaged. MUTATION GUARD: returning nullopt for
    // a broken table (as the pre-fix build did) makes (a) and (c) identical and
    // reports a corrupt database as an uncalibrated camera.
    REQUIRE(QSqlQuery(fx.h()).exec(
        QStringLiteral("DROP TABLE ball_level_binding")));
    CHECK_FALSE(denso::level::try_level_config_for(fx.h(), *cid).has_value());
}

TEST_CASE("a broken ball_level_calibration is Blocked/78, not Degraded/10",
          "[level][integrity][blocker1]") {
    Fixture fx;
    const auto cid = denso::camera::insert(fx.h(), ip_camera("Tank 1"));
    REQUIRE(cid.has_value());
    REQUIRE(save_float(fx, *cid, good_calibration()));
    REQUIRE(denso::mode::save(fx.h(), TargetMode::BallLeveler));

    // Sanity: healthy first.
    REQUIRE(denso::health::evaluate_integrity(fx.h(), fx.dir.path(),
                                              TargetMode::BallLeveler, *fx.view,
                                              kPlatform)
                .status == Readiness::Ready);

    // MUTATION GUARD: a missing table is INFRASTRUCTURE, not a per-camera gap.
    REQUIRE(QSqlQuery(fx.h()).exec(
        QStringLiteral("DROP TABLE ball_level_binding")));
    const auto v = denso::health::evaluate_integrity(
        fx.h(), fx.dir.path(), TargetMode::BallLeveler, *fx.view, kPlatform);
    CHECK(v.status == Readiness::Blocked);
    CHECK(denso::health::exit_code_for(v.status) == 78);
    CHECK_FALSE(v.blockers.empty());
}

TEST_CASE("an EMPTY fleet cannot hide a broken ball_level_calibration",
          "[level][integrity][blocker1]") {
    Fixture fx;   // deliberately NO camera at all
    REQUIRE(denso::mode::save(fx.h(), TargetMode::BallLeveler));
    REQUIRE(QSqlQuery(fx.h()).exec(
        QStringLiteral("DROP TABLE ball_level_binding")));

    // MUTATION GUARD: querying the table only inside the per-camera loop means an
    // appliance with no active camera never touches it and reports READY on a
    // corrupt database. The probe must be unconditional.
    const auto v = denso::health::evaluate_integrity(
        fx.h(), fx.dir.path(), TargetMode::BallLeveler, *fx.view, kPlatform);
    CHECK(v.status == Readiness::Blocked);
    CHECK(denso::health::exit_code_for(v.status) == 78);
}

TEST_CASE("an uncalibrated camera on a HEALTHY schema stays Degraded/10",
          "[level][integrity][blocker1]") {
    Fixture fx;
    const auto cid = denso::camera::insert(fx.h(), ip_camera("Tank 1"));
    REQUIRE(cid.has_value());
    REQUIRE(denso::mode::save(fx.h(), TargetMode::BallLeveler));

    // The counterpart of the test above: absence must NOT be promoted to Blocked
    // now that failure is. Degraded/10 is the correct answer for a setup gap.
    const auto v = denso::health::evaluate_integrity(
        fx.h(), fx.dir.path(), TargetMode::BallLeveler, *fx.view, kPlatform);
    CHECK(v.status == Readiness::Degraded);
    CHECK(denso::health::exit_code_for(v.status) == 10);
    CHECK(v.blockers.empty());
    REQUIRE(v.issues.size() == 1);
    CHECK(v.issues.front().policy_reason == QStringLiteral("level_calibration_missing"));
}

// ─── Blocker 2. mode_setup_required never guesses ────────────────────────────

TEST_CASE("try_cameras_with_valid_config reports query failure as nullopt",
          "[level][fallible][blocker2]") {
    Fixture fx;
    const auto cid = denso::camera::insert(fx.h(), ip_camera("Tank 1"));
    REQUIRE(cid.has_value());

    const auto empty = denso::level::try_cameras_with_valid_config(fx.h());
    REQUIRE(empty.has_value());
    CHECK(empty->empty());

    REQUIRE(save_float(fx, *cid, good_calibration()));
    const auto one = denso::level::try_cameras_with_valid_config(fx.h());
    REQUIRE(one.has_value());
    CHECK(one->size() == 1);

    // MUTATION GUARD: swallowing the failure as `{}` makes a broken table look
    // exactly like a fresh install.
    REQUIRE(QSqlQuery(fx.h()).exec(
        QStringLiteral("DROP TABLE ball_level_binding")));
    CHECK_FALSE(denso::level::try_cameras_with_valid_config(fx.h()).has_value());
}

TEST_CASE("mode_setup_required propagates undeterminable rather than guessing",
          "[level][setup-required][blocker2]") {
    Fixture fx;
    const auto cid = denso::camera::insert(fx.h(), ip_camera("Tank 1"));
    REQUIRE(cid.has_value());
    REQUIRE(save_float(fx, *cid, good_calibration()));

    // Configured -> setup NOT required.
    const auto ok = denso::mode::mode_setup_required(fx.h(), TargetMode::BallLeveler);
    REQUIRE(ok.has_value());
    CHECK_FALSE(*ok);

    // Break the REAL query. MUTATION GUARD: a preliminary `SELECT 1` probe is not
    // a substitute - the answer must come from the query actually used, and a
    // failure must be nullopt, never `true`.
    REQUIRE(QSqlQuery(fx.h()).exec(
        QStringLiteral("DROP TABLE ball_level_binding")));
    const auto broken =
        denso::mode::mode_setup_required(fx.h(), TargetMode::BallLeveler);
    CHECK_FALSE(broken.has_value());
}

// ─── Blocker 3. the class must be DECLARED by the model ──────────────────────

TEST_CASE("save_level_configuration rejects a class the model does not declare",
          "[level][save][blocker3]") {
    Fixture fx;
    const auto cid = denso::camera::insert(fx.h(), ip_camera("Tank 1"));
    REQUIRE(cid.has_value());
    // float-small declares exactly one class ("Small"), so 0 is the only member.

    SECTION("an out-of-range id") {
        SaveRefusal refusal;
        // MUTATION GUARD: validating class COUNT but not MEMBERSHIP lets 999
        // persist as a durable binding no runtime can ever resolve.
        CHECK_FALSE(denso::level::save_level_configuration(
            fx.h(), *cid, {LevelBinding{fx.float_model_id, {999}}},
            one_zone(good_calibration()), "rev-1", *fx.view, kPlatform, &refusal));
        CHECK(refusal.reason_code == "level_class_unknown");
        CHECK(refusal.canonical_id == "float-small");
        CHECK(row_count(fx.h(), "ball_level_zone") == 0);
    }

    SECTION("a negative id") {
        SaveRefusal refusal;
        CHECK_FALSE(denso::level::save_level_configuration(
            fx.h(), *cid, {LevelBinding{fx.float_model_id, {-1}}},
            one_zone(good_calibration()), "rev-1", *fx.view, kPlatform, &refusal));
        CHECK(refusal.reason_code == "level_class_unknown");
        CHECK(row_count(fx.h(), "ball_level_zone") == 0);
    }

    SECTION("the one declared id is accepted") {
        CHECK(denso::level::save_level_configuration(
            fx.h(), *cid, {LevelBinding{fx.float_model_id, {0}}},
            one_zone(good_calibration()), "rev-1", *fx.view, kPlatform, nullptr));
        CHECK(row_count(fx.h(), "ball_level_zone") == 1);
    }
}

// ─── the shared 1..99 namespace, from the Ball side ──────────────────────────
//
// The Ball chokepoint must apply the SAME bound as the digit one, or the two
// modes would disagree about what the one machine-wide namespace contains. It
// asks camera::zone_in_range rather than re-spelling the bound, so the floor it
// enforces is the one `ball_level_zone`'s own `CHECK (zone_no >= 1)` has carried
// since v15 — which is exactly why v17 needs no rebuild on this side.

TEST_CASE("save_level_configuration enforces the 1..99 zone range",
          "[level][save][zone_namespace]") {
    Fixture fx;
    const auto cid = denso::camera::insert(fx.h(), ip_camera("Tank 1"));
    REQUIRE(cid.has_value());

    SECTION("a negative zone is refused, with the offending number named") {
        SaveRefusal refusal;
        CHECK_FALSE(denso::level::save_level_configuration(
            fx.h(), *cid, {LevelBinding{fx.float_model_id, {0}}},
            one_zone(good_calibration(), -1), "rev-1", *fx.view, kPlatform, &refusal));
        CHECK(refusal.reason_code == "level_zone_out_of_range");
        REQUIRE(refusal.zone_no.has_value());
        CHECK(*refusal.zone_no == -1);
        CHECK(row_count(fx.h(), "ball_level_zone") == 0);
    }

    SECTION("a zone above the ceiling is refused") {
        SaveRefusal refusal;
        CHECK_FALSE(denso::level::save_level_configuration(
            fx.h(), *cid, {LevelBinding{fx.float_model_id, {0}}},
            one_zone(good_calibration(), 100), "rev-1", *fx.view, kPlatform, &refusal));
        CHECK(refusal.reason_code == "level_zone_out_of_range");
        REQUIRE(refusal.zone_no.has_value());
        CHECK(*refusal.zone_no == 100);
        CHECK(row_count(fx.h(), "ball_level_zone") == 0);
    }

    SECTION("Zone 0 is refused, by the application and by the DDL alike") {
        SaveRefusal refusal;
        CHECK_FALSE(denso::level::save_level_configuration(
            fx.h(), *cid, {LevelBinding{fx.float_model_id, {0}}},
            one_zone(good_calibration(), 0), "rev-1", *fx.view, kPlatform, &refusal));
        CHECK(refusal.reason_code == "level_zone_out_of_range");
        REQUIRE(refusal.zone_no.has_value());
        // Engaged and holding 0: a refusal ABOUT zone 0 is a different fact from
        // a camera-scoped refusal naming no zone, which is why this field is an
        // optional and not a 0 sentinel.
        CHECK(*refusal.zone_no == 0);
        CHECK(row_count(fx.h(), "ball_level_zone") == 0);
    }

    SECTION("Zone 1 is accepted and stored as 1") {
        REQUIRE(denso::level::save_level_configuration(
            fx.h(), *cid, {LevelBinding{fx.float_model_id, {0}}},
            one_zone(good_calibration(), 1), "rev-1", *fx.view, kPlatform, nullptr));
        const auto stored = denso::level::level_config_for(fx.h(), *cid);
        REQUIRE(stored.has_value());
        REQUIRE(stored->zones.size() == 1);
        CHECK(stored->zones.at(0).zone_no == 1);
    }

    SECTION("Zone 99 is accepted and stored as 99") {
        REQUIRE(denso::level::save_level_configuration(
            fx.h(), *cid, {LevelBinding{fx.float_model_id, {0}}},
            one_zone(good_calibration(), 99), "rev-1", *fx.view, kPlatform, nullptr));
        const auto stored = denso::level::level_config_for(fx.h(), *cid);
        REQUIRE(stored.has_value());
        REQUIRE(stored->zones.size() == 1);
        CHECK(stored->zones.at(0).zone_no == 99);
    }
}

TEST_CASE("the zone namespace is shared across BOTH modes",
          "[level][save][zone_namespace]") {
    Fixture fx;
    const auto digit_cam = denso::camera::insert(fx.h(), ip_camera("Line 1"));
    const auto ball_cam = denso::camera::insert(fx.h(), ip_camera("Tank 1"));
    REQUIRE(digit_cam.has_value());
    REQUIRE(ball_cam.has_value());

    // A digit ROI takes Zone 1 — the lowest zone there is, and the number the
    // Ball allocator would also reach for first, so the two modes race for it.
    denso::camera::CameraArea a;
    a.name = "digit-one";
    a.zone = 1;
    a.points = {{0.1f, 0.1f}, {0.9f, 0.1f}, {0.5f, 0.9f}};
    REQUIRE(denso::camera::replace_areas(fx.h(), *digit_cam, {a}));

    // ...so the Ball camera cannot have it. If the ownership query stopped
    // spanning both tables, both modes would post a different reading under the
    // same "zone1" key.
    SaveRefusal refusal;
    CHECK_FALSE(denso::level::save_level_configuration(
        fx.h(), *ball_cam, {LevelBinding{fx.float_model_id, {0}}},
        one_zone(good_calibration(), 1), "rev-1", *fx.view, kPlatform, &refusal));
    CHECK(refusal.reason_code == "level_zone_taken");
    REQUIRE(refusal.zone_no.has_value());
    CHECK(*refusal.zone_no == 1);

    // And the reverse direction: a Ball Zone 5 blocks a digit ROI on 5.
    REQUIRE(denso::level::save_level_configuration(
        fx.h(), *ball_cam, {LevelBinding{fx.float_model_id, {0}}},
        one_zone(good_calibration(), 5), "rev-1", *fx.view, kPlatform, nullptr));
    denso::camera::CameraArea five;
    five.name = "digit-five";
    five.zone = 5;
    five.points = {{0.1f, 0.1f}, {0.9f, 0.1f}, {0.5f, 0.9f}};
    CHECK_FALSE(denso::camera::replace_areas(fx.h(), *digit_cam, {five}));
}

TEST_CASE("an unknown class performs NO partial write and spares the good row",
          "[level][save][blocker3][atomic]") {
    Fixture fx;
    const auto cid = denso::camera::insert(fx.h(), ip_camera("Tank 1"));
    REQUIRE(cid.has_value());
    REQUIRE(save_float(fx, *cid, good_calibration()));   // a good row exists

    CHECK_FALSE(denso::level::save_level_configuration(
        fx.h(), *cid, {LevelBinding{fx.float_model_id, {42}}}, one_zone(good_calibration()),
        "rev-CLOBBER", *fx.view, kPlatform, nullptr));

    // MUTATION GUARD: the rejection must roll back whole - the operator's stored
    // configuration is not collateral damage for a bad request.
    CHECK(row_count(fx.h(), "ball_level_zone") == 1);
    const auto still = denso::level::level_config_for(fx.h(), *cid);
    REQUIRE(still.has_value());
    CHECK(still->class_id == 0);
    CHECK(still->view_revision == "rev-1");
}

// ─── Blocker 4. the Ball table IS the mode ───────────────────────────────────

TEST_CASE("Ball persistence always authorises as BallLeveler, whatever a caller "
          "wants",
          "[level][save][blocker4]") {
    Fixture fx;
    const auto cid = denso::camera::insert(fx.h(), ip_camera("Tank 1"));
    REQUIRE(cid.has_value());

    // There is NO mode parameter to pass any more - the signature itself makes a
    // caller-chosen mode unsayable, which is the strongest form this guard can
    // take. The static_assert pins that: the 8-argument form must be the call.
    // MUTATION GUARD: restoring the parameter would let DigitReader authorise a
    // digit model into the Ball table's sole binding slot.
    static_assert(
        std::is_invocable_r_v<
            bool, decltype(denso::level::save_level_configuration)&,
            const QSqlDatabase&, int64_t, const std::vector<LevelBinding>&,
            const std::vector<LevelZone>&, const std::string&, const ManifestView&,
            const PlatformInfo&, SaveRefusal*>,
        "save_level_configuration must take NO caller-supplied mode");

    SaveRefusal refusal;
    // digitv3 is refused unconditionally: the only mode ever asked is BallLeveler.
    CHECK_FALSE(denso::level::save_level_configuration(
        fx.h(), *cid, {LevelBinding{fx.digit_model_id, {0}}}, one_zone(good_calibration()),
        "rev-1", *fx.view, kPlatform, &refusal));
    CHECK(refusal.reason_code == "model_mode_incompatible");
    CHECK(refusal.canonical_id == "digitv3");
    CHECK(row_count(fx.h(), "ball_level_zone") == 0);

    // ...and the Float model IS evaluated as a Ball Leveler model.
    CHECK(save_float(fx, *cid, good_calibration()));
    CHECK(row_count(fx.h(), "ball_level_zone") == 1);
}


// ─── Codex finding 3: the SECOND settings upsert must roll the FIRST back ────

TEST_CASE("a failure in the brazing upsert rolls back the mode.target write",
          "[level][switch][rollback]") {
    // The deleted test_mode_reset.cpp failure matrix injected at EVERY statement,
    // including brazing.enabled. The trigger-on-any-settings-write replacements
    // all abort on the FIRST statement (mode.target), so this second-statement
    // path had no coverage. It is the one that actually proves atomicity: if
    // mode.target were committed separately, the mode would advance while
    // reporting stayed on.
    //
    // MUTATION GUARD: this is mutation 10 (mode.target outside the transaction)
    // expressed as a permanent test rather than a one-off corruption cycle.
    auto run = [](bool brazing_row_exists) {
        Fixture fx;
        REQUIRE(denso::mode::save(fx.h(), TargetMode::DigitReader));
        if (brazing_row_exists) {
            // Force the UPDATE arm of the upsert.
            QSqlQuery seed(fx.h());
            REQUIRE(seed.exec(QStringLiteral(
                "INSERT INTO settings (key, value) VALUES ('brazing.enabled','1') "
                "ON CONFLICT(key) DO UPDATE SET value = excluded.value")));
        }
        // Abort ONLY the brazing write, so mode.target succeeds first and must be
        // rolled back by the failure that follows it.
        //
        // Exactly ONE trigger per section, deliberately. For SQLite
        // `INSERT ... ON CONFLICT DO UPDATE`, the BEFORE INSERT trigger fires
        // BEFORE conflict resolution, so arming both would abort the seeded case
        // in the INSERT trigger and the upsert's UPDATE arm would never run - the
        // test would silently exercise the same path twice.
        REQUIRE(QSqlQuery(fx.h()).exec(
            brazing_row_exists
                ? QStringLiteral("CREATE TRIGGER brz_u BEFORE UPDATE ON settings "
                                 "WHEN NEW.key = 'brazing.enabled' "
                                 "BEGIN SELECT RAISE(ABORT,'injected'); END")
                : QStringLiteral("CREATE TRIGGER brz_i BEFORE INSERT ON settings "
                                 "WHEN NEW.key = 'brazing.enabled' "
                                 "BEGIN SELECT RAISE(ABORT,'injected'); END")));

        const auto r = denso::mode::switch_and_reset(fx.h(), TargetMode::BallLeveler);
        CHECK_FALSE(r.ok);
        CHECK_FALSE(r.error.empty());
        // The mode did NOT advance, even though its own statement succeeded.
        CHECK(denso::mode::load(fx.h()) == TargetMode::DigitReader);
    };

    SECTION("the brazing row does not exist yet (INSERT arm)") { run(false); }
    SECTION("the brazing row already exists (UPDATE arm)")     { run(true); }
}
