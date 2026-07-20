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
