#include <catch2/catch_test_macros.hpp>
#include "cli/migrate_coordinator.h"
#include "db/db.h"
#include "detection/repo.h"
#include "models/hashing.h"
#include <QTemporaryDir>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSqlQuery>
#include <filesystem>
using namespace denso;

// Write text to <dir>/<name>. Returns the full path.
static QString put(const QDir& dir, const QString& name, const QByteArray& bytes) {
    QFile f(dir.filePath(name)); REQUIRE(f.open(QIODevice::WriteOnly));
    f.write(bytes); f.close(); return dir.filePath(name);
}
// Seed a temp DB file with camera 1 attached to old_a.engine; returns nothing (file persists).
static void seed_db(const QString& db_path) {
    auto db = db::Db::open(db_path); REQUIRE(db);
    REQUIRE(db::run_migrations(db->handle()));
    QSqlQuery q(db->handle());
    REQUIRE(q.exec("INSERT INTO camera(id,name,camera_type,width,height,fps,"
                   "pitch,roll,rotation,active,setup_complete) "
                   "VALUES(1,'C1','usb',640,480,15,0,0,0,1,1)"));
    detection::DetectionModel om{0,"old","old_a.engine",{"0","1"}};
    auto oid = detection::upsert_model(db->handle(), om); REQUIRE(oid);
    detection::CameraModel cm; cm.camera_id=1; cm.model_id=*oid; cm.classes={{0,0.5f},{1,0.5f}};
    REQUIRE(detection::set_camera_models(db->handle(), 1, {cm}));
    // db (and its connection) drops at scope end, before the coordinator reopens the file.
}
// Build a valid manifest.json string for new_b.engine/new_b.names.json with the given hashes.
static QByteArray manifest_json(const std::string& esha, const std::string& ssha) {
    return QByteArray::fromStdString(std::string(R"({"schema":1,"generations":[{)") +
        R"("name":"new-b","engine":"new_b.engine","engine_sha256":")" + esha + R"(",)" +
        R"("sidecar":"new_b.names.json","sidecar_sha256":")" + ssha + R"(",)" +
        R"("class_names":["0","1"],"built_for":{"trt":"10.3","cuda":"12.6","sm":"87"},)" +
        R"("installed_utc":"2026-07-20T00:00:00Z","state":"installed"}]})");
}

TEST_CASE("run_migrate succeeds end-to-end and swaps the camera's model", "[migrate_coord]") {
    QTemporaryDir tmp; REQUIRE(tmp.isValid());
    QDir dir(tmp.path());
    QString engine_path = put(dir, "new_b.engine", "ENGINE-BYTES");
    QString sidecar_path = put(dir, "new_b.names.json", R"(["0","1"])");
    auto esha = models::file_sha256(engine_path); REQUIRE(esha);
    auto ssha = models::file_sha256(sidecar_path); REQUIRE(ssha);
    put(dir, "manifest.json", manifest_json(*esha, *ssha));

    QString db_path = dir.filePath("denso.db");
    seed_db(db_path);

    cli::MigrateInputs in;
    in.models_dir = dir.path();
    in.db_path = db_path;
    in.old_engine = "old_a.engine";
    in.new_engine = "new_b.engine";
    in.class_map_path = "";
    in.cameras = {1};

    auto out = cli::run_migrate(in);
    REQUIRE(out.exit_code == 0);
    REQUIRE(out.json.contains("\"ok\":true"));

    auto db2 = db::Db::open(db_path); REQUIRE(db2);
    auto cd = detection::detection_for(db2->handle(), 1);
    REQUIRE(cd.models.size() == 1);
    REQUIRE(cd.models[0].filename == "new_b.engine");
}

TEST_CASE("run_migrate rejects a hash mismatch and leaves the DB unchanged", "[migrate_coord]") {
    QTemporaryDir tmp; REQUIRE(tmp.isValid());
    QDir dir(tmp.path());
    put(dir, "new_b.engine", "ENGINE-BYTES");
    put(dir, "new_b.names.json", R"(["0","1"])");
    put(dir, "manifest.json", manifest_json(std::string(64, 'b'), std::string(64, 'b')));

    QString db_path = dir.filePath("denso.db");
    seed_db(db_path);

    cli::MigrateInputs in;
    in.models_dir = dir.path();
    in.db_path = db_path;
    in.old_engine = "old_a.engine";
    in.new_engine = "new_b.engine";
    in.cameras = {1};

    auto out = cli::run_migrate(in);
    REQUIRE(out.exit_code == 7);
    auto doc = QJsonDocument::fromJson(out.json.toUtf8());
    REQUIRE(doc.object()["code"].toString() == "hash-mismatch");

    auto db2 = db::Db::open(db_path); REQUIRE(db2);
    auto cd = detection::detection_for(db2->handle(), 1);
    REQUIRE(cd.models.size() == 1);
    REQUIRE(cd.models[0].filename == "old_a.engine");
}

TEST_CASE("run_migrate fails when the engine file is missing and leaves the DB unchanged", "[migrate_coord]") {
    QTemporaryDir tmp; REQUIRE(tmp.isValid());
    QDir dir(tmp.path());
    QString engine_path = put(dir, "new_b.engine", "ENGINE-BYTES");
    QString sidecar_path = put(dir, "new_b.names.json", R"(["0","1"])");
    auto esha = models::file_sha256(engine_path); REQUIRE(esha);
    auto ssha = models::file_sha256(sidecar_path); REQUIRE(ssha);
    put(dir, "manifest.json", manifest_json(*esha, *ssha));
    REQUIRE(QFile::remove(engine_path));  // delete AFTER hashing/manifest write

    QString db_path = dir.filePath("denso.db");
    seed_db(db_path);

    cli::MigrateInputs in;
    in.models_dir = dir.path();
    in.db_path = db_path;
    in.old_engine = "old_a.engine";
    in.new_engine = "new_b.engine";
    in.cameras = {1};

    auto out = cli::run_migrate(in);
    REQUIRE(out.exit_code != 0);
    REQUIRE(out.exit_code == 6);  // missing engine -> empty canonical path -> path-escape
    auto doc = QJsonDocument::fromJson(out.json.toUtf8());
    REQUIRE(doc.object()["code"].toString() == "path-escape");

    auto db2 = db::Db::open(db_path); REQUIRE(db2);
    auto cd = detection::detection_for(db2->handle(), 1);
    REQUIRE(cd.models.size() == 1);
    REQUIRE(cd.models[0].filename == "old_a.engine");
}

TEST_CASE("run_migrate rejects a malformed manifest", "[migrate_coord]") {
    QTemporaryDir tmp; REQUIRE(tmp.isValid());
    QDir dir(tmp.path());
    put(dir, "manifest.json", "not json");

    QString db_path = dir.filePath("denso.db");
    seed_db(db_path);

    cli::MigrateInputs in;
    in.models_dir = dir.path();
    in.db_path = db_path;
    in.old_engine = "old_a.engine";
    in.new_engine = "new_b.engine";
    in.cameras = {1};

    auto out = cli::run_migrate(in);
    REQUIRE(out.exit_code == 4);
    auto doc = QJsonDocument::fromJson(out.json.toUtf8());
    REQUIRE(doc.object()["code"].toString() == "bad-manifest");
}

TEST_CASE("run_migrate rejects a malformed class-map", "[migrate_coord]") {
    QTemporaryDir tmp; REQUIRE(tmp.isValid());
    QDir dir(tmp.path());
    QString engine_path = put(dir, "new_b.engine", "ENGINE-BYTES");
    QString sidecar_path = put(dir, "new_b.names.json", R"(["0","1"])");
    auto esha = models::file_sha256(engine_path); REQUIRE(esha);
    auto ssha = models::file_sha256(sidecar_path); REQUIRE(ssha);
    put(dir, "manifest.json", manifest_json(*esha, *ssha));
    QString class_map_path = put(dir, "class_map.json", "not json");

    QString db_path = dir.filePath("denso.db");
    seed_db(db_path);

    cli::MigrateInputs in;
    in.models_dir = dir.path();
    in.db_path = db_path;
    in.old_engine = "old_a.engine";
    in.new_engine = "new_b.engine";
    in.class_map_path = class_map_path;
    in.cameras = {1};

    auto out = cli::run_migrate(in);
    REQUIRE(out.exit_code == 2);
    auto doc = QJsonDocument::fromJson(out.json.toUtf8());
    REQUIRE(doc.object()["code"].toString() == "bad-class-map");
}

TEST_CASE("run_migrate rejects an absent generation", "[migrate_coord]") {
    QTemporaryDir tmp; REQUIRE(tmp.isValid());
    QDir dir(tmp.path());
    QString engine_path = put(dir, "new_b.engine", "ENGINE-BYTES");
    QString sidecar_path = put(dir, "new_b.names.json", R"(["0","1"])");
    auto esha = models::file_sha256(engine_path); REQUIRE(esha);
    auto ssha = models::file_sha256(sidecar_path); REQUIRE(ssha);
    put(dir, "manifest.json", manifest_json(*esha, *ssha));

    QString db_path = dir.filePath("denso.db");
    seed_db(db_path);

    cli::MigrateInputs in;
    in.models_dir = dir.path();
    in.db_path = db_path;
    in.old_engine = "old_a.engine";
    in.new_engine = "ghost.engine";
    in.cameras = {1};

    auto out = cli::run_migrate(in);
    REQUIRE(out.exit_code == 5);
    auto doc = QJsonDocument::fromJson(out.json.toUtf8());
    REQUIRE(doc.object()["code"].toString() == "no-such-generation");
}

TEST_CASE("run_migrate rejects a sidecar symlinked outside models_dir", "[migrate_coord]") {
    QTemporaryDir tmp; REQUIRE(tmp.isValid());
    QTemporaryDir outside; REQUIRE(outside.isValid());
    QDir dir(tmp.path());
    QDir out_dir(outside.path());

    QString engine_path = put(dir, "new_b.engine", "ENGINE-BYTES");
    QString real_sidecar_path = put(out_dir, "real_sidecar.json", R"(["0","1"])");
    auto esha = models::file_sha256(engine_path); REQUIRE(esha);
    auto ssha = models::file_sha256(real_sidecar_path); REQUIRE(ssha);

    const std::filesystem::path link =
        std::filesystem::path(dir.filePath("new_b.names.json").toStdString());
    const std::filesystem::path target =
        std::filesystem::path(real_sidecar_path.toStdString());
    bool symlink_ok = true;
    try {
        std::filesystem::create_symlink(target, link);
    } catch (const std::filesystem::filesystem_error&) {
        symlink_ok = false;
    }
    if (!symlink_ok) {
        SKIP("symlinks unavailable on this host");
    }

    put(dir, "manifest.json", manifest_json(*esha, *ssha));

    QString db_path = dir.filePath("denso.db");
    seed_db(db_path);

    cli::MigrateInputs in;
    in.models_dir = dir.path();
    in.db_path = db_path;
    in.old_engine = "old_a.engine";
    in.new_engine = "new_b.engine";
    in.cameras = {1};

    auto out = cli::run_migrate(in);
    REQUIRE(out.exit_code == 6);
    auto doc = QJsonDocument::fromJson(out.json.toUtf8());
    REQUIRE(doc.object()["code"].toString() == "path-escape");
}

TEST_CASE("run_migrate auto-migrates a pre-v13 database before swapping", "[migrate_coord]") {
    QTemporaryDir tmp; REQUIRE(tmp.isValid());
    QDir dir(tmp.path());
    QString engine_path = put(dir, "new_b.engine", "ENGINE-BYTES");
    QString sidecar_path = put(dir, "new_b.names.json", R"(["0","1"])");
    auto esha = models::file_sha256(engine_path); REQUIRE(esha);
    auto ssha = models::file_sha256(sidecar_path); REQUIRE(ssha);
    put(dir, "manifest.json", manifest_json(*esha, *ssha));

    QString db_path = dir.filePath("denso.db");
    seed_db(db_path);
    {
        // Simulate a pre-v13 DB (before the model_migration_receipt table existed).
        auto db = db::Db::open(db_path); REQUIRE(db);
        QSqlQuery q(db->handle());
        REQUIRE(q.exec("DROP TABLE model_migration_receipt"));
        REQUIRE(q.exec("PRAGMA user_version=12"));
    }

    cli::MigrateInputs in;
    in.models_dir = dir.path();
    in.db_path = db_path;
    in.old_engine = "old_a.engine";
    in.new_engine = "new_b.engine";
    in.cameras = {1};

    auto out = cli::run_migrate(in);
    REQUIRE(out.exit_code == 0);

    auto db2 = db::Db::open(db_path); REQUIRE(db2);
    QSqlQuery q(db2->handle());
    REQUIRE(q.exec("SELECT COUNT(*) FROM model_migration_receipt"));
    REQUIRE(q.next());
    REQUIRE(q.value(0).toInt() == 1);
}

TEST_CASE("run_migrate refuses an unattached camera and leaves the DB unchanged", "[migrate_coord]") {
    QTemporaryDir tmp; REQUIRE(tmp.isValid());
    QDir dir(tmp.path());
    QString engine_path = put(dir, "new_b.engine", "ENGINE-BYTES");
    QString sidecar_path = put(dir, "new_b.names.json", R"(["0","1"])");
    auto esha = models::file_sha256(engine_path); REQUIRE(esha);
    auto ssha = models::file_sha256(sidecar_path); REQUIRE(ssha);
    put(dir, "manifest.json", manifest_json(*esha, *ssha));

    QString db_path = dir.filePath("denso.db");
    seed_db(db_path);  // only camera 1 attached

    cli::MigrateInputs in;
    in.models_dir = dir.path();
    in.db_path = db_path;
    in.old_engine = "old_a.engine";
    in.new_engine = "new_b.engine";
    in.cameras = {1, 2};  // camera 2 is not attached

    auto out = cli::run_migrate(in);
    REQUIRE(out.exit_code == 8);
    auto doc = QJsonDocument::fromJson(out.json.toUtf8());
    REQUIRE(doc.object()["code"].toString() == "migrate-failed");

    auto db2 = db::Db::open(db_path); REQUIRE(db2);
    QSqlQuery q(db2->handle());
    REQUIRE(q.exec("SELECT COUNT(*) FROM model_migration_receipt"));
    REQUIRE(q.next());
    REQUIRE(q.value(0).toInt() == 0);
    auto cd = detection::detection_for(db2->handle(), 1);
    REQUIRE(cd.models.size() == 1);
    REQUIRE(cd.models[0].filename == "old_a.engine");
}
