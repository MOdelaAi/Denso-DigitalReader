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

TEST_CASE("evaluate_db_schema: a non-SQLite file is Blocked as DbUnopenable",
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

TEST_CASE("evaluate_db_schema: a structurally corrupt database is Blocked as DbUnopenable",
          "[boot_schema]") {
    // The nastier case (Codex High): a VALID SQLite header with a readable
    // user_version, but a damaged b-tree page deeper in the file. A header check
    // alone waves it through as Ready; only a structural probe (PRAGMA
    // quick_check) catches it. A 24/7 appliance must fail CLOSED on this, not boot
    // onto a corrupt store and discover the damage on a later query.
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("torn.db"));
    {
        auto rw = db::Db::open(path);
        REQUIRE(rw.has_value());
        REQUIRE(db::run_migrations(rw->handle()));
        // Grow the file well past one page so there is a data page to damage while
        // leaving page 1 (header + user_version) intact.
        QSqlQuery ins(rw->handle());
        for (int i = 0; i < 400; ++i) {
            ins.prepare(QStringLiteral("INSERT INTO settings(key, value) VALUES(?, ?)"));
            ins.addBindValue(QStringLiteral("k%1").arg(i));
            ins.addBindValue(QString(64, QLatin1Char('x')));
            REQUIRE(ins.exec());
        }
        // Force the WAL into the main file so corrupting the .db actually damages
        // the data (Db::open uses WAL — rows would otherwise sit in the -wal).
        QSqlQuery ck(rw->handle());
        REQUIRE(ck.exec(QStringLiteral("PRAGMA wal_checkpoint(TRUNCATE)")));
    }
    {
        // Overwrite the whole of page 2 — its page header and cells — leaving
        // page 1 (the file header + user_version at offset 60) untouched.
        QFile f(path);
        REQUIRE(f.size() >= 8192);
        REQUIRE(f.open(QIODevice::ReadWrite));
        REQUIRE(f.seek(4096));
        REQUIRE(f.write(QByteArray(4096, '\xFF')) == 4096);
    }
    const auto v = health::evaluate_db_schema(path);
    REQUIRE(v.status == health::Readiness::Blocked);
    REQUIRE(v.blockers.size() == 1);
    REQUIRE(v.blockers[0].kind == health::GlobalBlocker::Kind::DbUnopenable);
}
