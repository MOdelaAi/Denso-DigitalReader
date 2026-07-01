#include <catch2/catch_test_macros.hpp>

#include "db/db.h"

#include <QDir>
#include <QFile>
#include <QSqlQuery>
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
    REQUIRE(user_version(db.handle()) == 7);
}

TEST_CASE("migrations are idempotent") {
    auto db = Db::open_in_memory();
    REQUIRE(db.has_value());
    REQUIRE(run_migrations(db->handle()));
    REQUIRE(run_migrations(db->handle()));
    REQUIRE(user_version(db->handle()) == 7);
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
