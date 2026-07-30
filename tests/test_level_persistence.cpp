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
//   * `switch_mode` is NON-DESTRUCTIVE: it preserves Digital Reader
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

using denso::detection::DetectionModel;
using denso::health::Readiness;
using denso::level::LevelBinding;
using denso::level::LevelCalibration;
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

bool save_float(const Fixture& fx, int64_t camera_id,
                const LevelCalibration& cal, SaveRefusal* refusal = nullptr) {
    return denso::level::save_level_configuration(
        fx.h(), camera_id, {LevelBinding{fx.float_model_id, {0}}}, cal, "rev-1",
        *fx.view, kPlatform, refusal);
}

}  // namespace

// ─── 1-2. migration v13 -> v14 is additive and preserves every digit row ──────

TEST_CASE("schema version is 14", "[level][schema]") {
    CHECK(denso::db::supported_schema_version() == 14);
}

TEST_CASE("v13 -> v14 migrates a populated database and preserves every Digital "
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

    // Reopen and migrate: v13 -> v14.
    {
        auto db = denso::db::Db::open(path);
        REQUIRE(db.has_value());
        REQUIRE(denso::db::read_user_version(db->handle()).value_or(-1) == 13);
        REQUIRE(denso::db::run_migrations(db->handle()));
        CHECK(denso::db::read_user_version(db->handle()).value_or(-1) == 14);

        // MUTATION GUARD: every digit row survives, with its values intact.
        CHECK(row_count(db->handle(), "camera") == 1);
        CHECK(row_count(db->handle(), "camera_model") == 1);
        CHECK(row_count(db->handle(), "camera_model_class") == 1);
        CHECK(row_count(db->handle(), "camera_area") == 1);
        CHECK(row_count(db->handle(), "reading") == 1);
        CHECK(row_count(db->handle(), "ball_level_calibration") == 0);

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
    CHECK(row_count(fx.h(), "ball_level_calibration") == 1);
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
            db->handle(), cam_id, {LevelBinding{model_id, {0}}}, good_calibration(),
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
        CHECK(got->calibration.y_100 == good_calibration().y_100);
        CHECK(got->calibration.y_0 == good_calibration().y_0);
        CHECK(got->calibration.rect_w == good_calibration().rect_w);
        CHECK(got->calibration.conf == good_calibration().conf);
        CHECK(got->calibration.hold_ms == good_calibration().hold_ms);
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
    CHECK(row_count(fx.h(), "ball_level_calibration") == 1);
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
        fx.h(), *cid, {LevelBinding{fx.digit_model_id, {0}}}, good_calibration(),
        "rev-1", *fx.view, kPlatform, &refusal));
    CHECK(refusal.reason_code == "model_mode_incompatible");
    CHECK(refusal.canonical_id == "digitv3");
    CHECK(row_count(fx.h(), "ball_level_calibration") == 0);
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
            fx.h(), *cid, {LevelBinding{*mid, {0}}}, good_calibration(), "rev-1",
            *fx.view, kPlatform, &refusal));
        CHECK(refusal.reason_code == "model_undeclared");
    }
    SECTION("a model_id with no catalog row at all") {
        SaveRefusal refusal;
        CHECK_FALSE(denso::level::save_level_configuration(
            fx.h(), *cid, {LevelBinding{999999, {0}}}, good_calibration(), "rev-1",
            *fx.view, kPlatform, &refusal));
        CHECK(refusal.reason_code == "model_undeclared");
    }
    CHECK(row_count(fx.h(), "ball_level_calibration") == 0);
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
    CHECK(row_count(fx.h(), "ball_level_calibration") == 0);
}

TEST_CASE("save_level_configuration requires exactly one model",
          "[level][save][arity]") {
    Fixture fx;
    const auto cid = denso::camera::insert(fx.h(), ip_camera("Tank 1"));
    REQUIRE(cid.has_value());

    SECTION("zero models") {
        SaveRefusal refusal;
        CHECK_FALSE(denso::level::save_level_configuration(
            fx.h(), *cid, {}, good_calibration(), "rev-1",
            *fx.view, kPlatform, &refusal));
        CHECK(refusal.reason_code == "level_model_count");
    }
    SECTION("two models") {
        // MUTATION GUARD: an ensemble must not be representable here.
        SaveRefusal refusal;
        CHECK_FALSE(denso::level::save_level_configuration(
            fx.h(), *cid,
            {LevelBinding{fx.float_model_id, {0}}, LevelBinding{fx.float_model_id, {0}}},
            good_calibration(), "rev-1", *fx.view,
            kPlatform, &refusal));
        CHECK(refusal.reason_code == "level_model_count");
    }
    CHECK(row_count(fx.h(), "ball_level_calibration") == 0);
}

TEST_CASE("save_level_configuration requires exactly one class",
          "[level][save][arity]") {
    Fixture fx;
    const auto cid = denso::camera::insert(fx.h(), ip_camera("Tank 1"));
    REQUIRE(cid.has_value());

    SECTION("zero classes") {
        SaveRefusal refusal;
        CHECK_FALSE(denso::level::save_level_configuration(
            fx.h(), *cid, {LevelBinding{fx.float_model_id, {}}}, good_calibration(),
            "rev-1", *fx.view, kPlatform, &refusal));
        CHECK(refusal.reason_code == "level_class_count");
    }
    SECTION("two classes") {
        SaveRefusal refusal;
        CHECK_FALSE(denso::level::save_level_configuration(
            fx.h(), *cid, {LevelBinding{fx.float_model_id, {0, 1}}},
            good_calibration(), "rev-1", *fx.view,
            kPlatform, &refusal));
        CHECK(refusal.reason_code == "level_class_count");
    }
    CHECK(row_count(fx.h(), "ball_level_calibration") == 0);
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
        CHECK(row_count(fx.h(), "ball_level_calibration") == 0);
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
    CHECK(got->calibration.y_100 == good_calibration().y_100);
    CHECK(got->calibration.y_0 == good_calibration().y_0);
    CHECK(row_count(fx.h(), "ball_level_calibration") == 1);
}

// ─── 14-17. non-destructive switch_mode ──────────────────────────────────────

TEST_CASE("switch_mode preserves Digital Reader configuration, Ball Leveler "
          "calibration and every camera connection",
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
    const auto r1 = denso::mode::switch_mode(fx.h(), TargetMode::BallLeveler);
    REQUIRE(r1.ok);
    CHECK(denso::mode::load(fx.h()) == TargetMode::BallLeveler);

    // MUTATION GUARD: nothing may be deleted by a switch.
    CHECK(row_count(fx.h(), "camera") == 1);
    CHECK(row_count(fx.h(), "camera_model") == 1);
    CHECK(row_count(fx.h(), "camera_model_class") == 1);
    CHECK(row_count(fx.h(), "camera_area") == 1);
    CHECK(row_count(fx.h(), "reading") == 1);
    CHECK(row_count(fx.h(), "ball_level_calibration") == 1);

    // Camera connection columns and setup flags survive untouched.
    const auto cams = denso::camera::all(fx.h());
    REQUIRE(cams.size() == 1);
    CHECK(cams.front().rtsp == ip_camera("Line 1").rtsp);
    CHECK(cams.front().username == ip_camera("Line 1").username);
    CHECK(cams.front().rotation == 90);
    CHECK(cams.front().setup_complete);

    // Reporting disabled, address kept.
    const auto bz = denso::brazing::load(fx.h());
    CHECK_FALSE(bz.enabled);
    CHECK(bz.base_url == "http://backend.example/api");

    // ball_leveler -> digit_reader: the Ball calibration must survive too.
    const auto r2 = denso::mode::switch_mode(fx.h(), TargetMode::DigitReader);
    REQUIRE(r2.ok);
    CHECK(denso::mode::load(fx.h()) == TargetMode::DigitReader);
    CHECK(row_count(fx.h(), "ball_level_calibration") == 1);
    CHECK(row_count(fx.h(), "camera_model") == 1);
    CHECK(row_count(fx.h(), "camera_area") == 1);
    const auto still = denso::level::level_config_for(fx.h(), *cid);
    REQUIRE(still.has_value());
    CHECK(still->calibration.y_0 == good_calibration().y_0);
}

TEST_CASE("a failed mode-switch transaction leaves the previous mode unchanged",
          "[level][switch][rollback]") {
    Fixture fx;
    REQUIRE(denso::mode::save(fx.h(), TargetMode::DigitReader));

    // Break the settings table so the mode write inside the transaction fails.
    // MUTATION GUARD: mode.target must be written INSIDE the transaction, so a
    // failure cannot leave the persisted mode advanced.
    REQUIRE(QSqlQuery(fx.h()).exec(QStringLiteral("DROP TABLE settings")));
    const auto r = denso::mode::switch_mode(fx.h(), TargetMode::BallLeveler);
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
        QStringLiteral("DROP TABLE ball_level_calibration")));
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
        QStringLiteral("DROP TABLE ball_level_calibration")));
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
        QStringLiteral("DROP TABLE ball_level_calibration")));

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
        QStringLiteral("DROP TABLE ball_level_calibration")));
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
        QStringLiteral("DROP TABLE ball_level_calibration")));
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
            good_calibration(), "rev-1", *fx.view, kPlatform, &refusal));
        CHECK(refusal.reason_code == "level_class_unknown");
        CHECK(refusal.canonical_id == "float-small");
        CHECK(row_count(fx.h(), "ball_level_calibration") == 0);
    }

    SECTION("a negative id") {
        SaveRefusal refusal;
        CHECK_FALSE(denso::level::save_level_configuration(
            fx.h(), *cid, {LevelBinding{fx.float_model_id, {-1}}},
            good_calibration(), "rev-1", *fx.view, kPlatform, &refusal));
        CHECK(refusal.reason_code == "level_class_unknown");
        CHECK(row_count(fx.h(), "ball_level_calibration") == 0);
    }

    SECTION("the one declared id is accepted") {
        CHECK(denso::level::save_level_configuration(
            fx.h(), *cid, {LevelBinding{fx.float_model_id, {0}}},
            good_calibration(), "rev-1", *fx.view, kPlatform, nullptr));
        CHECK(row_count(fx.h(), "ball_level_calibration") == 1);
    }
}

TEST_CASE("an unknown class performs NO partial write and spares the good row",
          "[level][save][blocker3][atomic]") {
    Fixture fx;
    const auto cid = denso::camera::insert(fx.h(), ip_camera("Tank 1"));
    REQUIRE(cid.has_value());
    REQUIRE(save_float(fx, *cid, good_calibration()));   // a good row exists

    CHECK_FALSE(denso::level::save_level_configuration(
        fx.h(), *cid, {LevelBinding{fx.float_model_id, {42}}}, good_calibration(),
        "rev-CLOBBER", *fx.view, kPlatform, nullptr));

    // MUTATION GUARD: the rejection must roll back whole - the operator's stored
    // configuration is not collateral damage for a bad request.
    CHECK(row_count(fx.h(), "ball_level_calibration") == 1);
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
            const LevelCalibration&, const std::string&, const ManifestView&,
            const PlatformInfo&, SaveRefusal*>,
        "save_level_configuration must take NO caller-supplied mode");

    SaveRefusal refusal;
    // digitv3 is refused unconditionally: the only mode ever asked is BallLeveler.
    CHECK_FALSE(denso::level::save_level_configuration(
        fx.h(), *cid, {LevelBinding{fx.digit_model_id, {0}}}, good_calibration(),
        "rev-1", *fx.view, kPlatform, &refusal));
    CHECK(refusal.reason_code == "model_mode_incompatible");
    CHECK(refusal.canonical_id == "digitv3");
    CHECK(row_count(fx.h(), "ball_level_calibration") == 0);

    // ...and the Float model IS evaluated as a Ball Leveler model.
    CHECK(save_float(fx, *cid, good_calibration()));
    CHECK(row_count(fx.h(), "ball_level_calibration") == 1);
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

        const auto r = denso::mode::switch_mode(fx.h(), TargetMode::BallLeveler);
        CHECK_FALSE(r.ok);
        CHECK_FALSE(r.error.empty());
        // The mode did NOT advance, even though its own statement succeeded.
        CHECK(denso::mode::load(fx.h()) == TargetMode::DigitReader);
    };

    SECTION("the brazing row does not exist yet (INSERT arm)") { run(false); }
    SECTION("the brazing row already exists (UPDATE arm)")     { run(true); }
}
