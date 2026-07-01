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

#include "camera/camera.h"
#include "camera/repo.h"
#include "detection/repo.h"

using denso::detection::CameraModel;
using denso::detection::detection_for;
using denso::detection::models_for;
using denso::detection::ModelClassSelection;
using denso::detection::set_camera_models;

namespace {
int64_t seed_camera(const QSqlDatabase& db) {
    denso::camera::Camera c;
    c.name = "Cam";
    c.camera_type = "usb";
    c.active = true;
    c.index = 0;
    c.width = 640; c.height = 480; c.fps = 30;
    return *denso::camera::insert(db, c);
}
int64_t seed_model(const QSqlDatabase& db) {
    denso::detection::DetectionModel m;
    m.name = "denso"; m.filename = "denso.onnx";
    m.class_names = {"0", "1", "2", "3"};
    return *upsert_model(db, m);
}
} // namespace

TEST_CASE("set_camera_models + models_for round-trip attachments and classes") {
    auto d = mem();
    const int64_t cam = seed_camera(d.handle());
    const int64_t model = seed_model(d.handle());

    CameraModel cm;
    cm.camera_id = cam;
    cm.model_id = model;
    cm.classes = {ModelClassSelection{1, 0.6f}, ModelClassSelection{3, 0.4f}};
    REQUIRE(set_camera_models(d.handle(), cam, {cm}));

    const auto got = models_for(d.handle(), cam);
    REQUIRE(got.size() == 1);
    REQUIRE(got[0].model_id == model);
    REQUIRE(got[0].classes.size() == 2);
    REQUIRE(got[0].classes[0].class_id == 1);
    REQUIRE(got[0].classes[0].conf == 0.6f);
    REQUIRE(got[0].classes[1].class_id == 3);
}

TEST_CASE("set_camera_models replaces the previous set") {
    auto d = mem();
    const int64_t cam = seed_camera(d.handle());
    const int64_t model = seed_model(d.handle());
    CameraModel cm; cm.camera_id = cam; cm.model_id = model;
    cm.classes = {ModelClassSelection{0, 0.5f}};
    REQUIRE(set_camera_models(d.handle(), cam, {cm, cm}));
    REQUIRE(models_for(d.handle(), cam).size() == 2);
    REQUIRE(set_camera_models(d.handle(), cam, {}));
    REQUIRE(models_for(d.handle(), cam).empty());
}

TEST_CASE("detection_for resolves filename + class_names from the model") {
    auto d = mem();
    const int64_t cam = seed_camera(d.handle());
    const int64_t model = seed_model(d.handle());
    CameraModel cm; cm.camera_id = cam; cm.model_id = model;
    cm.classes = {ModelClassSelection{2, 0.7f}};
    REQUIRE(set_camera_models(d.handle(), cam, {cm}));

    const auto det = detection_for(d.handle(), cam);
    REQUIRE(det.camera_id == cam);
    REQUIRE(det.models.size() == 1);
    REQUIRE(det.models[0].filename == "denso.onnx");
    REQUIRE(det.models[0].class_names.size() == 4);
    REQUIRE(det.models[0].classes.size() == 1);
    REQUIRE(det.models[0].classes[0].class_id == 2);
    REQUIRE(det.models[0].classes[0].conf == 0.7f);
}

TEST_CASE("detection_for is empty for a camera with no models") {
    auto d = mem();
    const int64_t cam = seed_camera(d.handle());
    REQUIRE(detection_for(d.handle(), cam).models.empty());
}
