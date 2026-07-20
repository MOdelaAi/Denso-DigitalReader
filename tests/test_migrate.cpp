#include <catch2/catch_test_macros.hpp>
#include "db/db.h"
#include "detection/migrate.h"
#include "detection/repo.h"
#include <QSqlQuery>
#include <QVariant>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
using namespace denso::detection;
static denso::db::Db seed() {
    auto db = denso::db::Db::open_in_memory();
    REQUIRE(db); REQUIRE(denso::db::run_migrations(db->handle()));
    QSqlQuery q(db->handle());
    REQUIRE(q.exec("INSERT INTO camera(id,name,camera_type,width,height,fps,"
                   "pitch,roll,rotation,active,setup_complete) "
                   "VALUES(1,'C1','usb',640,480,15,0,0,0,1,1)"));
    DetectionModel old_m{0,"old","old_a.engine",{"0","1"}};
    auto oid = upsert_model(db->handle(), old_m); REQUIRE(oid);
    CameraModel cm; cm.camera_id=1; cm.model_id=*oid; cm.classes={{0,0.5f},{1,0.5f}};
    REQUIRE(set_camera_models(db->handle(), 1, {cm}));
    return std::move(*db);
}
static MigrateRequest ok_req() {
    MigrateRequest r;
    r.old_filename="old_a.engine"; r.new_filename="new_b.engine";
    r.new_name="new"; r.new_engine_sha256=std::string(64,'a');
    r.new_class_names={"0","1"}; r.camera_ids={1}; r.created_utc="2026-07-20T00:00:00Z";
    return r;
}
TEST_CASE("validate_request accepts a clean request", "[migrate]") {
    REQUIRE_FALSE(validate_request(ok_req()).has_value());
}
TEST_CASE("validate_request rejects empty cameras", "[migrate]") {
    auto r = ok_req(); r.camera_ids.clear();
    REQUIRE(validate_request(r).has_value());
}
TEST_CASE("validate_request rejects duplicate camera ids", "[migrate]") {
    auto r = ok_req(); r.camera_ids={1,1};
    REQUIRE(validate_request(r).has_value());
}
TEST_CASE("validate_request rejects a non-positive camera id", "[migrate]") {
    auto r = ok_req(); r.camera_ids={0};
    REQUIRE(validate_request(r).has_value());
}
TEST_CASE("validate_request rejects old==new filename", "[migrate]") {
    auto r = ok_req(); r.new_filename=r.old_filename;
    REQUIRE(validate_request(r).has_value());
}
TEST_CASE("validate_request rejects a negative camera id", "[migrate]") {
    auto r = ok_req(); r.camera_ids={-1};
    REQUIRE(validate_request(r).has_value());
}
TEST_CASE("validate_request rejects each empty required string field", "[migrate]") {
    { auto r = ok_req(); r.old_filename.clear();      REQUIRE(validate_request(r).has_value()); }
    { auto r = ok_req(); r.new_filename.clear();      REQUIRE(validate_request(r).has_value()); }
    { auto r = ok_req(); r.new_name.clear();          REQUIRE(validate_request(r).has_value()); }
    { auto r = ok_req(); r.new_class_names.clear();   REQUIRE(validate_request(r).has_value()); }
    { auto r = ok_req(); r.new_engine_sha256.clear(); REQUIRE(validate_request(r).has_value()); }
}
TEST_CASE("load_old_attachment returns Ok for a normal attachment", "[migrate]") {
    auto db = seed();
    auto res = load_old_attachment(db.handle(), 1, "old_a.engine");
    REQUIRE(res.status == LoadStatus::Ok);
    REQUIRE(res.attach.old_class_names == std::vector<std::string>{"0","1"});
    REQUIRE(res.attach.camera_model_id > 0);
    REQUIRE(res.attach.old_model_id > 0);
    // The kept classes must load exactly the seeded (class_id, conf) pairs — this is
    // what a rollback repoints, so assert the content, not just the count.
    REQUIRE(res.attach.classes.size() == 2);
    REQUIRE(res.attach.classes[0].class_id == 0);
    REQUIRE(res.attach.classes[0].conf == 0.5f);
    REQUIRE(res.attach.classes[1].class_id == 1);
    REQUIRE(res.attach.classes[1].conf == 0.5f);
}
TEST_CASE("load_old_attachment returns NotAttached for a wrong filename", "[migrate]") {
    auto db = seed();
    REQUIRE(load_old_attachment(db.handle(), 1, "nope.engine").status == LoadStatus::NotAttached);
}
TEST_CASE("load_old_attachment returns Ambiguous on a double attachment", "[migrate]") {
    auto db = seed();
    // Raw second identical attachment (same camera, same model) => 2 join rows.
    QSqlQuery q(db.handle());
    REQUIRE(q.exec("INSERT INTO camera_model(camera_id,model_id) "
                   "SELECT camera_id,model_id FROM camera_model WHERE camera_id=1 LIMIT 1"));
    REQUIRE(load_old_attachment(db.handle(), 1, "old_a.engine").status == LoadStatus::Ambiguous);
}
TEST_CASE("migrate_model swaps the model in one transaction and writes a receipt", "[migrate]") {
    auto db = seed();
    auto res = migrate_model(db.handle(), ok_req());
    REQUIRE(res.ok);
    REQUIRE(res.affected_cameras == std::vector<int64_t>{1});

    auto det = detection_for(db.handle(), 1);
    REQUIRE(det.models.size() == 1);
    REQUIRE(det.models[0].filename == "new_b.engine");

    QSqlQuery q(db.handle());
    REQUIRE(q.exec("SELECT count(*),old_model_id,new_engine_sha256,attachments "
                   "FROM model_migration_receipt"));
    REQUIRE(q.next());
    REQUIRE(q.value(0).toInt() == 1);
    REQUIRE(q.value(1).toLongLong() > 0);
    REQUIRE(q.value(2).toString().toStdString() == ok_req().new_engine_sha256);
    REQUIRE(q.value(3).toString().contains("camera_model_id"));
}
TEST_CASE("migrate_model CAS refusal leaves the DB unchanged", "[migrate]") {
    auto db = seed();
    auto r = ok_req();
    r.camera_ids = {1, 2};   // camera 2 has no attachment
    auto res = migrate_model(db.handle(), r);
    REQUIRE_FALSE(res.ok);
    REQUIRE(res.error.find("2") != std::string::npos);

    auto det = detection_for(db.handle(), 1);
    REQUIRE(det.models.size() == 1);
    REQUIRE(det.models[0].filename == "old_a.engine");

    QSqlQuery q(db.handle());
    REQUIRE(q.exec("SELECT count(*) FROM model_migration_receipt"));
    REQUIRE(q.next());
    REQUIRE(q.value(0).toInt() == 0);
}
TEST_CASE("migrate_model refuses an unmapped selected class and leaves the DB unchanged", "[migrate]") {
    auto db = seed();
    auto r = ok_req();
    r.new_class_names = {"0"};   // old selected class 1 has no name in the new model
    auto res = migrate_model(db.handle(), r);
    REQUIRE_FALSE(res.ok);

    auto det = detection_for(db.handle(), 1);
    REQUIRE(det.models.size() == 1);
    REQUIRE(det.models[0].filename == "old_a.engine");

    QSqlQuery q(db.handle());
    REQUIRE(q.exec("SELECT count(*) FROM model_migration_receipt"));
    REQUIRE(q.next());
    REQUIRE(q.value(0).toInt() == 0);
}
// Attach the seeded old model to a second camera, so a multi-camera swap can be
// exercised. Returns nothing; camera 2 mirrors camera 1's selection.
static void attach_second_camera(const QSqlDatabase& h) {
    QSqlQuery q(h);
    REQUIRE(q.exec("INSERT INTO camera(id,name,camera_type,width,height,fps,"
                   "pitch,roll,rotation,active,setup_complete) "
                   "VALUES(2,'C2','usb',640,480,15,0,0,0,1,1)"));
    auto oid = upsert_model(h, DetectionModel{0,"old","old_a.engine",{"0","1"}});
    REQUIRE(oid);
    CameraModel cm; cm.camera_id=2; cm.model_id=*oid; cm.classes={{0,0.5f},{1,0.5f}};
    REQUIRE(set_camera_models(h, 2, {cm}));
}
TEST_CASE("migrate_model swaps multiple cameras and records each attachment", "[migrate]") {
    auto db = seed();
    attach_second_camera(db.handle());
    auto r = ok_req(); r.camera_ids = {1, 2};
    auto res = migrate_model(db.handle(), r);
    REQUIRE(res.ok);
    REQUIRE(res.affected_cameras == std::vector<int64_t>{1, 2});
    REQUIRE(detection_for(db.handle(), 1).models.at(0).filename == "new_b.engine");
    REQUIRE(detection_for(db.handle(), 2).models.at(0).filename == "new_b.engine");
    // One receipt whose attachments array holds both cameras, ids as STRINGS.
    QSqlQuery q(db.handle());
    REQUIRE(q.exec("SELECT attachments FROM model_migration_receipt"));
    REQUIRE(q.next());
    const auto atts = QJsonDocument::fromJson(q.value(0).toString().toUtf8()).array();
    REQUIRE(atts.size() == 2);
    REQUIRE(atts.at(0).toObject().value("camera_id").isString());
    REQUIRE(atts.at(0).toObject().value("camera_id").toString() == "1");
    REQUIRE(atts.at(1).toObject().value("camera_id").toString() == "2");
}
TEST_CASE("migrate_model records the forward/inverse map for a class reorder", "[migrate]") {
    auto db = seed();
    auto r = ok_req(); r.new_class_names = {"1", "0"};   // new id 0=="1", new id 1=="0"
    REQUIRE(migrate_model(db.handle(), r).ok);
    QSqlQuery q(db.handle());
    REQUIRE(q.exec("SELECT forward_map,inverse_map FROM model_migration_receipt"));
    REQUIRE(q.next());
    // old "0"->new index 1, old "1"->new index 0.
    REQUIRE(q.value(0).toString() == "{\"0\":1,\"1\":0}");
    // inverse is the swap's self-inverse: new 0<-old 1, new 1<-old 0.
    REQUIRE(q.value(1).toString() == "{\"0\":1,\"1\":0}");
}
TEST_CASE("migrate_model re-run refuses cleanly and does not double-write", "[migrate]") {
    auto db = seed();
    REQUIRE(migrate_model(db.handle(), ok_req()).ok);        // cameras now attach new_b.engine
    auto second = migrate_model(db.handle(), ok_req());      // old_a.engine no longer attached
    REQUIRE_FALSE(second.ok);                                // CAS NotAttached
    QSqlQuery q(db.handle());
    REQUIRE(q.exec("SELECT count(*) FROM model_migration_receipt"));
    REQUIRE(q.next());
    REQUIRE(q.value(0).toInt() == 1);                        // still exactly one receipt
}
