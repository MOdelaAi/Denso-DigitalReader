// The DB-stage readiness classifier shared by GUI boot (main.cpp) and the
// headless --check path (run_headless.cpp). Both route a future-schema database
// through evaluate_db_schema, so they classify it IDENTICALLY as Blocked and
// return the same EX_CONFIG (78) exit code — a DB written by a newer app build
// must never be opened or migrated by an older one.
#include <catch2/catch_test_macros.hpp>

#include "db/db.h"
#include "health/integrity.h"

#include <QFile>
#include <QSqlQuery>
#include <QTemporaryDir>

using namespace denso;

TEST_CASE("evaluate_db_schema: a missing database is Ready (fresh install)",
          "[boot_schema]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString missing = dir.filePath(QStringLiteral("absent.db"));

    const auto v = health::evaluate_db_schema(missing);
    REQUIRE(v.status == health::Readiness::Ready);
    REQUIRE(v.blockers.empty());
    // Classifying must never CREATE the database (mirrors the --check contract).
    REQUIRE_FALSE(QFile::exists(missing));
}

TEST_CASE("evaluate_db_schema: a normally-migrated database is Ready",
          "[boot_schema]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("ok.db"));
    {
        auto rw = db::Db::open(path);
        REQUIRE(rw.has_value());
        REQUIRE(db::run_migrations(rw->handle()));
    }
    const auto v = health::evaluate_db_schema(path);
    REQUIRE(v.status == health::Readiness::Ready);
    REQUIRE(v.blockers.empty());
}

TEST_CASE("evaluate_db_schema: a future-version database is Blocked as SchemaNewer",
          "[boot_schema]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("future.db"));
    {
        auto rw = db::Db::open(path);
        REQUIRE(rw.has_value());
        REQUIRE(db::run_migrations(rw->handle()));
        QSqlQuery q(rw->handle());
        REQUIRE(q.exec(QStringLiteral("PRAGMA user_version = %1")
                           .arg(db::supported_schema_version() + 1)));
    }
    const auto v = health::evaluate_db_schema(path);
    REQUIRE(v.status == health::Readiness::Blocked);
    REQUIRE(v.blockers.size() == 1);
    REQUIRE(v.blockers[0].kind == health::GlobalBlocker::Kind::SchemaNewer);
    // Boot and --check both map this verdict through exit_code_for → EX_CONFIG.
    REQUIRE(health::exit_code_for(v.status) == 78);
}

TEST_CASE("evaluate_db_schema: an unreadable/corrupt database is Blocked as DbUnopenable",
          "[boot_schema]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("corrupt.db"));
    {
        QFile f(path);
        REQUIRE(f.open(QIODevice::WriteOnly));
        f.write("this is not a SQLite database — just garbage header bytes");
    }
    const auto v = health::evaluate_db_schema(path);
    REQUIRE(v.status == health::Readiness::Blocked);
    REQUIRE(v.blockers.size() == 1);
    REQUIRE(v.blockers[0].kind == health::GlobalBlocker::Kind::DbUnopenable);
    REQUIRE(health::exit_code_for(v.status) == 78);
}
