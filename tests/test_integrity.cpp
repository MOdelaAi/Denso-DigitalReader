#include <catch2/catch_test_macros.hpp>
#include "health/integrity.h"
#include "db/db.h"
#include <QTemporaryDir>
#include <QDir>
#include <QFile>
#include <QSqlQuery>
using namespace denso;

static void put(const QDir& d, const QString& name, const QByteArray& bytes) {
    QFile f(d.filePath(name)); REQUIRE(f.open(QIODevice::WriteOnly));
    f.write(bytes); f.close();
}

TEST_CASE("integrity: a clean empty install is Ready", "[integrity]") {
    QTemporaryDir tmp; REQUIRE(tmp.isValid());
    QDir models(tmp.path()); REQUIRE(models.mkpath("models"));
    auto db = db::Db::open(QDir(tmp.path()).filePath("denso.db")); REQUIRE(db);
    REQUIRE(db::run_migrations(db->handle()));
    const auto v = health::evaluate_integrity(db->handle(),
                                              QDir(tmp.path()).filePath("models"));
    REQUIRE(v.status == health::Readiness::Ready);
}

TEST_CASE("integrity: an unreadable models_dir is a GLOBAL blocker", "[integrity]") {
    QTemporaryDir tmp; REQUIRE(tmp.isValid());
    auto db = db::Db::open(QDir(tmp.path()).filePath("denso.db")); REQUIRE(db);
    REQUIRE(db::run_migrations(db->handle()));
    const auto v = health::evaluate_integrity(db->handle(),
                                              QDir(tmp.path()).filePath("nope"));
    REQUIRE(v.status == health::Readiness::Blocked);
    REQUIRE_FALSE(v.blockers.empty());
    REQUIRE(v.blockers[0].kind == health::GlobalBlocker::Kind::ModelsDirUnreadable);
}

TEST_CASE("integrity: a corrupt manifest is a GLOBAL blocker", "[integrity]") {
    QTemporaryDir tmp; REQUIRE(tmp.isValid());
    QDir root(tmp.path()); REQUIRE(root.mkpath("models"));
    QDir models(root.filePath("models"));
    put(models, "manifest.json", "{ this is not json");
    auto db = db::Db::open(root.filePath("denso.db")); REQUIRE(db);
    REQUIRE(db::run_migrations(db->handle()));
    const auto v = health::evaluate_integrity(db->handle(), models.path());
    REQUIRE(v.status == health::Readiness::Blocked);
    REQUIRE(v.blockers[0].kind == health::GlobalBlocker::Kind::ManifestCorrupt);
}

TEST_CASE("integrity: an engine on disk but absent from the manifest is DEGRADED",
          "[integrity]") {
    // The production-Jetson compatibility case (spec §2.3): reported, never blocking.
    QTemporaryDir tmp; REQUIRE(tmp.isValid());
    QDir root(tmp.path()); REQUIRE(root.mkpath("models"));
    QDir models(root.filePath("models"));
    put(models, "digitv3.engine", "ENGINE");
    put(models, "digitv3.names.json", R"(["0","1"])");
    auto db = db::Db::open(root.filePath("denso.db")); REQUIRE(db);
    REQUIRE(db::run_migrations(db->handle()));
    const auto v = health::evaluate_integrity(db->handle(), models.path());
    REQUIRE(v.status == health::Readiness::Degraded);
    bool found = false;
    for (const auto& i : v.issues) {
        if (i.kind == health::ZoneIssue::Kind::EnginesUnmanifested) found = true;
    }
    REQUIRE(found);
    REQUIRE(v.blockers.empty());   // MUST NOT block — it would brick production
}

TEST_CASE("integrity: a valid but empty manifest is accepted, not corrupt",
          "[integrity]") {
    // An empty generations array parses and validates cleanly; with no engines on
    // disk the install is Ready, not Blocked. Guards against a future change that
    // mistakes "no models yet" for a corrupt manifest.
    QTemporaryDir tmp; REQUIRE(tmp.isValid());
    QDir root(tmp.path()); REQUIRE(root.mkpath("models"));
    QDir models(root.filePath("models"));
    put(models, "manifest.json", R"({"schema":1,"generations":[]})");
    auto db = db::Db::open(root.filePath("denso.db")); REQUIRE(db);
    REQUIRE(db::run_migrations(db->handle()));
    const auto v = health::evaluate_integrity(db->handle(), models.path());
    REQUIRE(v.status == health::Readiness::Ready);
    REQUIRE(v.blockers.empty());
}

TEST_CASE("integrity: a failed attachment query BLOCKS, never a silent empty fleet",
          "[integrity]") {
    // A broken DB must not be conflated with "no rows": dropping a joined table
    // makes the readiness query ERROR, which is a global blocker (spec §2.2), not
    // a Ready install with zero cameras.
    QTemporaryDir tmp; REQUIRE(tmp.isValid());
    QDir root(tmp.path()); REQUIRE(root.mkpath("models"));
    QDir models(root.filePath("models"));
    auto db = db::Db::open(root.filePath("denso.db")); REQUIRE(db);
    REQUIRE(db::run_migrations(db->handle()));
    QSqlQuery q(db->handle());
    REQUIRE(q.exec("DROP TABLE camera_model"));
    const auto v = health::evaluate_integrity(db->handle(), models.path());
    REQUIRE(v.status == health::Readiness::Blocked);
    REQUIRE_FALSE(v.blockers.empty());
    REQUIRE(v.blockers[0].kind == health::GlobalBlocker::Kind::DbQueryFailed);
}

// ─── schema-2 manifested-set bookkeeping (Slice 1) ───────────────────────────
//
// These guard the ONE integrity change Release A makes. Under schema 2 the
// artifact filenames move into runtime.{onnxruntime,tensorrt}; if the manifested
// set kept reading the root `engine` it would be empty, and an appliance's own
// correctly-declared engine would be reported EnginesUnmanifested -> Degraded.
// That would degrade every appliance the release touched.
//
// Note what is NOT here: no mode, no backend, no compatibility verdict. This path
// is declaration bookkeeping only.

namespace {
std::string sha2(char c) { return std::string(64, c); }

// A schema-2 manifest with one generation, either or both runtime blocks present.
QByteArray schema2_manifest(bool with_ort, bool with_trt) {
    std::string rt = "{";
    bool first = true;
    if (with_ort) {
        rt += std::string(R"("onnxruntime":{"model":"digitv3.onnx","model_sha256":")") +
              sha2('a') + R"(","class_metadata_source":"onnx_metadata_names"})";
        first = false;
    }
    if (with_trt) {
        if (!first) rt += ",";
        rt += std::string(R"("tensorrt":{"engine":"digitv3.engine","engine_sha256":")") +
              sha2('b') + R"(","sidecar":"digitv3.names.json","sidecar_sha256":")" +
              sha2('c') + R"(","class_metadata_source":"names_sidecar",)" +
              R"("built_for":{"trt":"10.3","cuda":"12.6","sm":"87"}})";
    }
    rt += "}";
    const std::string prov =
        std::string(R"("provenance":{"source_pt":"digitv3.pt","source_pt_sha256":")") +
        sha2('d') + R"(","onnx":"digitv3.onnx","onnx_sha256":")" + sha2('a') +
        R"(","onnx_opset":12,"training_ultralytics":"8.4.33",)" +
        R"("export_ultralytics":"8.4.33","batch":1,"dynamic":false,"nms":false,)" +
        R"("precision":"fp16","jetpack":"6.2","export_onnx_command":"e",)" +
        R"("export_engine_command":"t"})";
    return QByteArray::fromStdString(
        std::string(R"({"schema":2,"generations":[{)") +
        R"("name":"digitv3","canonical_id":"digitv3","family":"digit_numeric",)" +
        R"("task":"detect","input_size":640,"class_names":["0","1"],"class_count":2,)" +
        R"("runtime":)" + rt + "," + prov +
        R"(,"installed_utc":"2026-07-22T00:00:00Z","state":"installed"}]})");
}

bool has_unmanifested(const health::IntegrityVerdict& v, const QString& file) {
    for (const auto& i : v.issues)
        if (i.kind == health::ZoneIssue::Kind::EnginesUnmanifested && i.detail == file)
            return true;
    return false;
}
}  // namespace

TEST_CASE("integrity: a schema 2 tensorrt declaration manifests its engine",
          "[integrity]") {
    QTemporaryDir tmp; REQUIRE(tmp.isValid());
    QDir root(tmp.path()); REQUIRE(root.mkpath("models"));
    QDir models(root.filePath("models"));
    put(models, "digitv3.engine", "ENGINE");
    put(models, "digitv3.names.json", R"(["0","1"])");
    put(models, "manifest.json", schema2_manifest(/*ort=*/false, /*trt=*/true));
    auto db = db::Db::open(root.filePath("denso.db")); REQUIRE(db);
    REQUIRE(db::run_migrations(db->handle()));
    const auto v = health::evaluate_integrity(db->handle(), models.path());
    REQUIRE_FALSE(has_unmanifested(v, "digitv3.engine"));
    REQUIRE(v.blockers.empty());
    REQUIRE(v.status == health::Readiness::Ready);
}

TEST_CASE("integrity: a schema 2 onnxruntime declaration manifests its onnx",
          "[integrity]") {
    QTemporaryDir tmp; REQUIRE(tmp.isValid());
    QDir root(tmp.path()); REQUIRE(root.mkpath("models"));
    QDir models(root.filePath("models"));
    put(models, "digitv3.onnx", "ONNX");
    put(models, "manifest.json", schema2_manifest(/*ort=*/true, /*trt=*/false));
    auto db = db::Db::open(root.filePath("denso.db")); REQUIRE(db);
    REQUIRE(db::run_migrations(db->handle()));
    const auto v = health::evaluate_integrity(db->handle(), models.path());
    REQUIRE_FALSE(has_unmanifested(v, "digitv3.onnx"));
    REQUIRE(v.blockers.empty());
    REQUIRE(v.status == health::Readiness::Ready);
}

TEST_CASE("integrity: a schema 2 generation with both blocks manifests both files",
          "[integrity]") {
    QTemporaryDir tmp; REQUIRE(tmp.isValid());
    QDir root(tmp.path()); REQUIRE(root.mkpath("models"));
    QDir models(root.filePath("models"));
    put(models, "digitv3.engine", "ENGINE");
    put(models, "digitv3.onnx", "ONNX");
    put(models, "digitv3.names.json", R"(["0","1"])");
    put(models, "manifest.json", schema2_manifest(/*ort=*/true, /*trt=*/true));
    auto db = db::Db::open(root.filePath("denso.db")); REQUIRE(db);
    REQUIRE(db::run_migrations(db->handle()));
    const auto v = health::evaluate_integrity(db->handle(), models.path());
    REQUIRE_FALSE(has_unmanifested(v, "digitv3.engine"));
    REQUIRE_FALSE(has_unmanifested(v, "digitv3.onnx"));
    REQUIRE(v.status == health::Readiness::Ready);
}

TEST_CASE("integrity: an undeclared file beside a schema 2 manifest stays DEGRADED",
          "[integrity]") {
    // The negative control. Broadening the manifested set must not blunt the real
    // fault it exists to report.
    QTemporaryDir tmp; REQUIRE(tmp.isValid());
    QDir root(tmp.path()); REQUIRE(root.mkpath("models"));
    QDir models(root.filePath("models"));
    put(models, "digitv3.engine", "ENGINE");
    put(models, "digitv3.names.json", R"(["0","1"])");
    put(models, "stray.engine", "STRAY");
    put(models, "manifest.json", schema2_manifest(/*ort=*/false, /*trt=*/true));
    auto db = db::Db::open(root.filePath("denso.db")); REQUIRE(db);
    REQUIRE(db::run_migrations(db->handle()));
    const auto v = health::evaluate_integrity(db->handle(), models.path());
    REQUIRE(v.status == health::Readiness::Degraded);
    REQUIRE(has_unmanifested(v, "stray.engine"));
    REQUIRE_FALSE(has_unmanifested(v, "digitv3.engine"));
    REQUIRE(v.blockers.empty());
}

TEST_CASE("integrity: the manifested set does not depend on the committed mode",
          "[integrity]") {
    // evaluate_integrity takes no mode and consults no policy: the same manifest
    // must yield the same verdict whatever mode.target says. Asserted directly so
    // that a later slice cannot quietly make this bookkeeping mode-sensitive.
    QTemporaryDir tmp; REQUIRE(tmp.isValid());
    QDir root(tmp.path()); REQUIRE(root.mkpath("models"));
    QDir models(root.filePath("models"));
    put(models, "digitv3.engine", "ENGINE");
    put(models, "digitv3.names.json", R"(["0","1"])");
    put(models, "manifest.json", schema2_manifest(/*ort=*/false, /*trt=*/true));
    auto db = db::Db::open(root.filePath("denso.db")); REQUIRE(db);
    REQUIRE(db::run_migrations(db->handle()));
    QSqlQuery q(db->handle());

    REQUIRE(q.exec("INSERT OR REPLACE INTO settings(key,value) "
                   "VALUES('mode.target','digit_reader')"));
    const auto a = health::evaluate_integrity(db->handle(), models.path());
    REQUIRE(q.exec("INSERT OR REPLACE INTO settings(key,value) "
                   "VALUES('mode.target','ball_leveler')"));
    const auto b = health::evaluate_integrity(db->handle(), models.path());

    REQUIRE(a.status == b.status);
    REQUIRE(a.issues.size() == b.issues.size());
    REQUIRE(a.status == health::Readiness::Ready);
}

TEST_CASE("integrity: a camera attached to a missing engine is a per-zone issue",
          "[integrity]") {
    QTemporaryDir tmp; REQUIRE(tmp.isValid());
    QDir root(tmp.path()); REQUIRE(root.mkpath("models"));
    QDir models(root.filePath("models"));
    auto db = db::Db::open(root.filePath("denso.db")); REQUIRE(db);
    REQUIRE(db::run_migrations(db->handle()));
    QSqlQuery q(db->handle());
    REQUIRE(q.exec("INSERT INTO camera(id,name,camera_type,width,height,fps,"
                   "pitch,roll,rotation,active,setup_complete) "
                   "VALUES(1,'C1','usb',640,480,15,0,0,0,1,1)"));
    REQUIRE(q.exec("INSERT INTO model(id,name,filename,class_names) "
                   "VALUES(1,'m','gone.engine','[\"0\"]')"));
    REQUIRE(q.exec("INSERT INTO camera_model(camera_id,model_id) VALUES(1,1)"));
    const auto v = health::evaluate_integrity(db->handle(), models.path());
    REQUIRE(v.status == health::Readiness::Degraded);
    bool found = false;
    for (const auto& i : v.issues) {
        if (i.kind == health::ZoneIssue::Kind::EngineMissing && i.camera_id == 1) found = true;
    }
    REQUIRE(found);
}
