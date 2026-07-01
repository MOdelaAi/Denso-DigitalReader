#include <catch2/catch_test_macros.hpp>

#include "db/db.h"

#include <QSqlDatabase>
#include <QSqlQuery>
#include <QString>

using denso::db::Db;
using denso::db::run_migrations;

namespace {
Db mem() {
    auto d = Db::open_in_memory();
    REQUIRE(d.has_value());
    REQUIRE(run_migrations(d->handle()));
    return std::move(*d);
}
bool has_table(const QSqlDatabase& db, const char* name) {
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?"));
    q.addBindValue(QString::fromLatin1(name));
    return q.exec() && q.next();
}
} // namespace

TEST_CASE("migration v8 creates the detection tables") {
    auto d = mem();
    REQUIRE(has_table(d.handle(), "model"));
    REQUIRE(has_table(d.handle(), "camera_model"));
    REQUIRE(has_table(d.handle(), "camera_model_class"));
}
