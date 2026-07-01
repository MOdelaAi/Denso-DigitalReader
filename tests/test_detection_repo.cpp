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

#include "detection/detection.h"
#include "detection/repo.h"

using denso::detection::DetectionModel;
using denso::detection::list_models;
using denso::detection::upsert_model;

TEST_CASE("upsert_model inserts and list_models returns it") {
    auto d = mem();
    DetectionModel m;
    m.name = "denso";
    m.filename = "denso.onnx";
    m.class_names = {"0", "1", "2"};
    const auto id = upsert_model(d.handle(), m);
    REQUIRE(id.has_value());

    const auto models = list_models(d.handle());
    REQUIRE(models.size() == 1);
    REQUIRE(models[0].id == *id);
    REQUIRE(models[0].name == "denso");
    REQUIRE(models[0].filename == "denso.onnx");
    REQUIRE(models[0].class_names == std::vector<std::string>{"0", "1", "2"});
}

TEST_CASE("upsert_model updates by filename without adding a row") {
    auto d = mem();
    DetectionModel m;
    m.name = "old";
    m.filename = "denso.onnx";
    m.class_names = {"0"};
    const auto id1 = upsert_model(d.handle(), m);
    m.name = "new";
    m.class_names = {"0", "1"};
    const auto id2 = upsert_model(d.handle(), m);
    REQUIRE(id1 == id2);
    const auto models = list_models(d.handle());
    REQUIRE(models.size() == 1);
    REQUIRE(models[0].name == "new");
    REQUIRE(models[0].class_names.size() == 2);
}
