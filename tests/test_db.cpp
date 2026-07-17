#include <catch2/catch_test_macros.hpp>

#include "db/db.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QVariant>

#include <utility>

using denso::db::Db;
using denso::db::run_migrations;

namespace {

int table_count(const QSqlDatabase& db, const QString& name) {
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "SELECT count(*) FROM sqlite_master WHERE type='table' AND name=?"));
    q.addBindValue(name);
    REQUIRE(q.exec());
    REQUIRE(q.next());
    return q.value(0).toInt();
}

int user_version(const QSqlDatabase& db) {
    QSqlQuery q(db);
    REQUIRE(q.exec(QStringLiteral("PRAGMA user_version")));
    REQUIRE(q.next());
    return q.value(0).toInt();
}

Db migrated() {
    auto db = Db::open_in_memory();
    REQUIRE(db.has_value());
    REQUIRE(run_migrations(db->handle()));
    return std::move(*db);
}

} // namespace

TEST_CASE("migrations create settings table") {
    const Db db = migrated();
    REQUIRE(table_count(db.handle(), QStringLiteral("settings")) == 1);
}

TEST_CASE("migrations drop readings table") {
    // `readings` is created at v1 and dropped at v3 — a fully migrated DB must
    // not have it (the digit-reader log feature was removed).
    const Db db = migrated();
    REQUIRE(table_count(db.handle(), QStringLiteral("readings")) == 0);
}

TEST_CASE("migrations create net_config table") {
    const Db db = migrated();
    REQUIRE(table_count(db.handle(), QStringLiteral("net_config")) == 1);
}

TEST_CASE("migrations set user_version") {
    const Db db = migrated();
    REQUIRE(user_version(db.handle()) == 11);
}

TEST_CASE("migrations are idempotent") {
    auto db = Db::open_in_memory();
    REQUIRE(db.has_value());
    REQUIRE(run_migrations(db->handle()));
    REQUIRE(run_migrations(db->handle()));
    REQUIRE(user_version(db->handle()) == 11);
}

TEST_CASE("open enables WAL mode") {
    const QString path = QDir::tempPath() + QStringLiteral("/denso_open_enables_wal.db");
    QFile::remove(path);
    auto db = Db::open(path);
    REQUIRE(db.has_value());
    QSqlQuery q(db->handle());
    REQUIRE(q.exec(QStringLiteral("PRAGMA journal_mode")));
    REQUIRE(q.next());
    REQUIRE(q.value(0).toString().toLower() == QStringLiteral("wal"));
}

TEST_CASE("open_read_only does not create a missing database", "[db][readonly]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString missing = dir.filePath(QStringLiteral("absent.db"));

    // The --check contract: a missing DB is an empty configured-model set. It
    // must NOT be conjured into existence by the act of checking.
    REQUIRE_FALSE(Db::open_read_only(missing).has_value());
    REQUIRE_FALSE(QFile::exists(missing));
}

TEST_CASE("open_read_only reads but refuses writes", "[db][readonly]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("ro.db"));

    {
        auto rw = Db::open(path);
        REQUIRE(rw.has_value());
        REQUIRE(run_migrations(rw->handle()));
    }

    auto ro = Db::open_read_only(path);
    REQUIRE(ro.has_value());

    QSqlQuery read(ro->handle());
    REQUIRE(read.exec(QStringLiteral("SELECT count(*) FROM camera")));
    REQUIRE(read.next());

    QSqlQuery write(ro->handle());
    REQUIRE_FALSE(write.exec(QStringLiteral(
        "INSERT INTO settings (key, value) VALUES ('k', 'v')")));
}

TEST_CASE("open_read_only leaves the primary database byte-identical", "[db][readonly]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("journal.db"));

    {
        auto rw = Db::open(path);
        REQUIRE(rw.has_value());
        REQUIRE(run_migrations(rw->handle()));
    }

    // Hash the file itself rather than re-opening and asking PRAGMA
    // journal_mode: Db::open() forces WAL again, which would mask exactly the
    // mutation we're trying to detect.
    const auto digest = [&path] {
        QFile f(path);
        REQUIRE(f.open(QIODevice::ReadOnly));
        QCryptographicHash h(QCryptographicHash::Sha256);
        REQUIRE(h.addData(&f));
        return h.result();
    };

    const QByteArray before = digest();
    {
        auto ro = Db::open_read_only(path);
        REQUIRE(ro.has_value());
        QSqlQuery q(ro->handle());
        REQUIRE(q.exec(QStringLiteral("SELECT count(*) FROM camera")));
        REQUIRE(q.next());
    }
    REQUIRE(digest() == before);
}

TEST_CASE("open_read_only sets query_only", "[db][readonly]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("qo.db"));
    {
        auto rw = Db::open(path);
        REQUIRE(rw.has_value());
        REQUIRE(run_migrations(rw->handle()));
    }

    auto ro = Db::open_read_only(path);
    REQUIRE(ro.has_value());
    QSqlQuery q(ro->handle());
    REQUIRE(q.exec(QStringLiteral("PRAGMA query_only")));
    REQUIRE(q.next());
    REQUIRE(q.value(0).toInt() == 1);
}
