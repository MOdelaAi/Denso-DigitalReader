#include <catch2/catch_test_macros.hpp>
#include "cli/migrate_coordinator.h"
#include "db/db.h"
#include "detection/repo.h"
#include "models/hashing.h"
#include <QTemporaryDir>
#include <QDir>
#include <QFile>
#include <QJsonArray>
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
    // Success carries the full machine-readable schema: ok + a stable "code" slug
    // + the swapped engine + the repointed cameras.
    auto oj = QJsonDocument::fromJson(out.json.toUtf8()).object();
    REQUIRE(oj["ok"].toBool() == true);
    REQUIRE(oj["code"].toString() == "ok");
    REQUIRE(oj["new_engine"].toString() == "new_b.engine");
    REQUIRE(oj["affected_cameras"].toArray().size() == 1);
    REQUIRE(oj["affected_cameras"].toArray().at(0).toInt() == 1);

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

// Small fixture shared by the class-map tests: a valid models_dir (engine+sidecar
// hashed into the manifest) + a seeded DB. Fills `in`; the caller sets class_map_path.
static void setup_valid(QDir& dir, QString& db_path, cli::MigrateInputs& in) {
    QString engine_path = put(dir, "new_b.engine", "ENGINE-BYTES");
    QString sidecar_path = put(dir, "new_b.names.json", R"(["0","1"])");
    auto esha = models::file_sha256(engine_path); REQUIRE(esha);
    auto ssha = models::file_sha256(sidecar_path); REQUIRE(ssha);
    put(dir, "manifest.json", manifest_json(*esha, *ssha));
    db_path = dir.filePath("denso.db");
    seed_db(db_path);
    in.models_dir = dir.path(); in.db_path = db_path;
    in.old_engine = "old_a.engine"; in.new_engine = "new_b.engine"; in.cameras = {1};
}

TEST_CASE("run_migrate applies a valid class-map remap through to migrate_model", "[migrate_coord]") {
    QTemporaryDir tmp; REQUIRE(tmp.isValid());
    QDir dir(tmp.path()); QString db_path; cli::MigrateInputs in;
    setup_valid(dir, db_path, in);
    // Swap the two class names by NAME: old "0"->new "1", old "1"->new "0".
    in.class_map_path = put(dir, "class_map.json", R"({"0":"1","1":"0"})");

    auto out = cli::run_migrate(in);
    REQUIRE(out.exit_code == 0);
    // Proof the remap reached migrate_model: the receipt's forward map is the swap.
    auto db2 = db::Db::open(db_path); REQUIRE(db2);
    QSqlQuery q(db2->handle());
    REQUIRE(q.exec("SELECT forward_map FROM model_migration_receipt"));
    REQUIRE(q.next());
    REQUIRE(q.value(0).toString() == "{\"0\":1,\"1\":0}");
}

TEST_CASE("run_migrate rejects a class-map whose root is not an object", "[migrate_coord]") {
    QTemporaryDir tmp; REQUIRE(tmp.isValid());
    QDir dir(tmp.path()); QString db_path; cli::MigrateInputs in;
    setup_valid(dir, db_path, in);
    in.class_map_path = put(dir, "class_map.json", "[]");   // array, not object
    auto out = cli::run_migrate(in);
    REQUIRE(out.exit_code == 2);
    REQUIRE(QJsonDocument::fromJson(out.json.toUtf8()).object()["code"].toString() == "bad-class-map");
}

TEST_CASE("run_migrate rejects a class-map with a non-string value", "[migrate_coord]") {
    QTemporaryDir tmp; REQUIRE(tmp.isValid());
    QDir dir(tmp.path()); QString db_path; cli::MigrateInputs in;
    setup_valid(dir, db_path, in);
    in.class_map_path = put(dir, "class_map.json", R"({"0":5})");   // number value
    auto out = cli::run_migrate(in);
    REQUIRE(out.exit_code == 2);
    REQUIRE(QJsonDocument::fromJson(out.json.toUtf8()).object()["code"].toString() == "bad-class-map");
}

TEST_CASE("run_migrate rejects an unreadable class-map path", "[migrate_coord]") {
    QTemporaryDir tmp; REQUIRE(tmp.isValid());
    QDir dir(tmp.path()); QString db_path; cli::MigrateInputs in;
    setup_valid(dir, db_path, in);
    in.class_map_path = dir.filePath("does_not_exist.json");   // never created
    auto out = cli::run_migrate(in);
    REQUIRE(out.exit_code == 2);
    REQUIRE(QJsonDocument::fromJson(out.json.toUtf8()).object()["code"].toString() == "bad-class-map");
}

TEST_CASE("run_migrate rejects a sidecar-only hash mismatch", "[migrate_coord]") {
    QTemporaryDir tmp; REQUIRE(tmp.isValid());
    QDir dir(tmp.path());
    QString engine_path = put(dir, "new_b.engine", "ENGINE-BYTES");
    put(dir, "new_b.names.json", R"(["0","1"])");
    auto esha = models::file_sha256(engine_path); REQUIRE(esha);
    // Correct engine hash, WRONG sidecar hash — exercises the sidecar branch.
    put(dir, "manifest.json", manifest_json(*esha, std::string(64, 'c')));
    QString db_path = dir.filePath("denso.db"); seed_db(db_path);
    cli::MigrateInputs in;
    in.models_dir = dir.path(); in.db_path = db_path;
    in.old_engine = "old_a.engine"; in.new_engine = "new_b.engine"; in.cameras = {1};
    auto out = cli::run_migrate(in);
    REQUIRE(out.exit_code == 7);
    auto doc = QJsonDocument::fromJson(out.json.toUtf8());
    REQUIRE(doc.object()["code"].toString() == "hash-mismatch");
    REQUIRE(doc.object()["error"].toString().contains("sidecar"));
}

TEST_CASE("run_migrate reports a DB that cannot be opened", "[migrate_coord]") {
    QTemporaryDir tmp; REQUIRE(tmp.isValid());
    QDir dir(tmp.path());
    QString engine_path = put(dir, "new_b.engine", "ENGINE-BYTES");
    QString sidecar_path = put(dir, "new_b.names.json", R"(["0","1"])");
    auto esha = models::file_sha256(engine_path); REQUIRE(esha);
    auto ssha = models::file_sha256(sidecar_path); REQUIRE(ssha);
    put(dir, "manifest.json", manifest_json(*esha, *ssha));
    cli::MigrateInputs in;
    in.models_dir = dir.path();
    in.db_path = dir.filePath("no_such_subdir/denso.db");  // parent dir absent -> open fails
    in.old_engine = "old_a.engine"; in.new_engine = "new_b.engine"; in.cameras = {1};
    auto out = cli::run_migrate(in);
    REQUIRE(out.exit_code == 3);
    REQUIRE(QJsonDocument::fromJson(out.json.toUtf8()).object()["code"].toString() == "db-open-failed");
}

// ─── schema-2 parity (Slice 1) ───────────────────────────────────────────────
//
// Schema 2 nests the TensorRT artifact under runtime.tensorrt, leaving the root
// engine/sidecar/hash fields empty. The coordinator reads them through the
// schema-aware accessors, so a schema-2 generation must behave EXACTLY like the
// equivalent schema-1 one: same path-escape guard, same hash comparisons, same
// exit codes. Reading the raw fields would have silently compared against "".

// The same generation as manifest_json() above, expressed at schema 2.
static QByteArray manifest_json_v2(const std::string& esha, const std::string& ssha) {
    const std::string prov =
        std::string(R"("provenance":{"source_pt":"new_b.pt","source_pt_sha256":")") +
        std::string(64, 'd') + R"(","onnx":"new_b.onnx","onnx_sha256":")" +
        std::string(64, 'a') + R"(","onnx_opset":12,"training_ultralytics":"8.4.33",)" +
        R"("export_ultralytics":"8.4.33","batch":1,"dynamic":false,"nms":false,)" +
        R"("precision":"fp16","jetpack":"6.2","export_onnx_command":"e",)" +
        R"("export_engine_command":"t"})";
    return QByteArray::fromStdString(
        std::string(R"({"schema":2,"generations":[{)") +
        R"("name":"new-b","canonical_id":"new-b","family":"digit_numeric",)" +
        R"("task":"detect","input_size":640,"class_names":["0","1"],"class_count":2,)" +
        R"("runtime":{"tensorrt":{"engine":"new_b.engine","engine_sha256":")" + esha +
        R"(","sidecar":"new_b.names.json","sidecar_sha256":")" + ssha +
        R"(","class_metadata_source":"names_sidecar",)" +
        R"("built_for":{"trt":"10.3","cuda":"12.6","sm":"87"}}},)" + prov +
        R"(,"installed_utc":"2026-07-22T00:00:00Z","state":"installed"}]})");
}

TEST_CASE("run_migrate succeeds against a schema 2 manifest", "[migrate_coord]") {
    QTemporaryDir tmp; REQUIRE(tmp.isValid());
    QDir dir(tmp.path());
    QString engine_path = put(dir, "new_b.engine", "ENGINE-BYTES");
    QString sidecar_path = put(dir, "new_b.names.json", R"(["0","1"])");
    auto esha = models::file_sha256(engine_path); REQUIRE(esha);
    auto ssha = models::file_sha256(sidecar_path); REQUIRE(ssha);
    put(dir, "manifest.json", manifest_json_v2(*esha, *ssha));

    QString db_path = dir.filePath("denso.db");
    seed_db(db_path);

    cli::MigrateInputs in;
    in.models_dir = dir.path();
    in.db_path = db_path;
    in.old_engine = "old_a.engine";
    in.new_engine = "new_b.engine";
    in.cameras = {1};

    auto out = cli::run_migrate(in);
    REQUIRE(out.exit_code == 0);
    auto oj = QJsonDocument::fromJson(out.json.toUtf8()).object();
    REQUIRE(oj["ok"].toBool() == true);
    REQUIRE(oj["new_engine"].toString() == "new_b.engine");

    auto db2 = db::Db::open(db_path); REQUIRE(db2);
    auto cd = detection::detection_for(db2->handle(), 1);
    REQUIRE(cd.models.size() == 1);
    REQUIRE(cd.models[0].filename == "new_b.engine");
}

TEST_CASE("run_migrate rejects a schema 2 engine hash mismatch", "[migrate_coord]") {
    QTemporaryDir tmp; REQUIRE(tmp.isValid());
    QDir dir(tmp.path());
    put(dir, "new_b.engine", "ENGINE-BYTES");
    QString sidecar_path = put(dir, "new_b.names.json", R"(["0","1"])");
    auto ssha = models::file_sha256(sidecar_path); REQUIRE(ssha);
    put(dir, "manifest.json", manifest_json_v2(std::string(64, 'b'), *ssha));

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
    REQUIRE(QJsonDocument::fromJson(out.json.toUtf8()).object()["code"].toString()
            == "hash-mismatch");

    auto db2 = db::Db::open(db_path); REQUIRE(db2);
    auto cd = detection::detection_for(db2->handle(), 1);
    REQUIRE(cd.models[0].filename == "old_a.engine");
}

TEST_CASE("run_migrate rejects a schema 2 sidecar-only hash mismatch",
          "[migrate_coord]") {
    QTemporaryDir tmp; REQUIRE(tmp.isValid());
    QDir dir(tmp.path());
    QString engine_path = put(dir, "new_b.engine", "ENGINE-BYTES");
    put(dir, "new_b.names.json", R"(["0","1"])");
    auto esha = models::file_sha256(engine_path); REQUIRE(esha);
    put(dir, "manifest.json", manifest_json_v2(*esha, std::string(64, 'c')));

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
    REQUIRE(QJsonDocument::fromJson(out.json.toUtf8()).object()["code"].toString()
            == "hash-mismatch");
}

TEST_CASE("run_migrate reports a missing schema 2 engine file", "[migrate_coord]") {
    QTemporaryDir tmp; REQUIRE(tmp.isValid());
    QDir dir(tmp.path());
    QString engine_path = put(dir, "new_b.engine", "ENGINE-BYTES");
    QString sidecar_path = put(dir, "new_b.names.json", R"(["0","1"])");
    auto esha = models::file_sha256(engine_path); REQUIRE(esha);
    auto ssha = models::file_sha256(sidecar_path); REQUIRE(ssha);
    put(dir, "manifest.json", manifest_json_v2(*esha, *ssha));
    REQUIRE(QFile::remove(engine_path));   // delete AFTER hashing

    QString db_path = dir.filePath("denso.db");
    seed_db(db_path);

    cli::MigrateInputs in;
    in.models_dir = dir.path();
    in.db_path = db_path;
    in.old_engine = "old_a.engine";
    in.new_engine = "new_b.engine";
    in.cameras = {1};

    // A missing engine cannot canonicalize, so it trips the path-escape guard —
    // exactly as it does at schema 1.
    auto out = cli::run_migrate(in);
    REQUIRE(out.exit_code == 6);
    REQUIRE(QJsonDocument::fromJson(out.json.toUtf8()).object()["code"].toString()
            == "path-escape");

    auto db2 = db::Db::open(db_path); REQUIRE(db2);
    auto cd = detection::detection_for(db2->handle(), 1);
    REQUIRE(cd.models[0].filename == "old_a.engine");
}

TEST_CASE("run_migrate rejects an absent generation in a schema 2 manifest",
          "[migrate_coord]") {
    QTemporaryDir tmp; REQUIRE(tmp.isValid());
    QDir dir(tmp.path());
    QString engine_path = put(dir, "new_b.engine", "ENGINE-BYTES");
    QString sidecar_path = put(dir, "new_b.names.json", R"(["0","1"])");
    auto esha = models::file_sha256(engine_path); REQUIRE(esha);
    auto ssha = models::file_sha256(sidecar_path); REQUIRE(ssha);
    put(dir, "manifest.json", manifest_json_v2(*esha, *ssha));

    QString db_path = dir.filePath("denso.db");
    seed_db(db_path);

    cli::MigrateInputs in;
    in.models_dir = dir.path();
    in.db_path = db_path;
    in.old_engine = "old_a.engine";
    in.new_engine = "not_in_manifest.engine";
    in.cameras = {1};

    auto out = cli::run_migrate(in);
    REQUIRE(out.exit_code == 5);
    REQUIRE(QJsonDocument::fromJson(out.json.toUtf8()).object()["code"].toString()
            == "no-such-generation");
}

TEST_CASE("run_migrate never resolves a schema 2 generation by its onnx filename",
          "[migrate_coord]") {
    // find_by_engine matches runtime.tensorrt.engine ONLY. Asking for the ONNX
    // name must be "no such generation", never a silent match that would then go
    // on to hash the wrong artifact.
    QTemporaryDir tmp; REQUIRE(tmp.isValid());
    QDir dir(tmp.path());
    QString engine_path = put(dir, "new_b.engine", "ENGINE-BYTES");
    QString sidecar_path = put(dir, "new_b.names.json", R"(["0","1"])");
    put(dir, "new_b.onnx", "ONNX-BYTES");
    auto esha = models::file_sha256(engine_path); REQUIRE(esha);
    auto ssha = models::file_sha256(sidecar_path); REQUIRE(ssha);
    put(dir, "manifest.json", manifest_json_v2(*esha, *ssha));

    QString db_path = dir.filePath("denso.db");
    seed_db(db_path);

    cli::MigrateInputs in;
    in.models_dir = dir.path();
    in.db_path = db_path;
    in.old_engine = "old_a.engine";
    in.new_engine = "new_b.onnx";
    in.cameras = {1};

    auto out = cli::run_migrate(in);
    REQUIRE(out.exit_code == 5);
    REQUIRE(QJsonDocument::fromJson(out.json.toUtf8()).object()["code"].toString()
            == "no-such-generation");
}
