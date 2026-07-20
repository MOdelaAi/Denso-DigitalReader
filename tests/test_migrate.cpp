#include <catch2/catch_test_macros.hpp>
#include "db/db.h"
#include "detection/migrate.h"
#include "detection/repo.h"
#include <QSqlQuery>
#include <QVariant>
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
TEST_CASE("load_old_attachment returns Ok for a normal attachment", "[migrate]") {
    auto db = seed();
    auto res = load_old_attachment(db.handle(), 1, "old_a.engine");
    REQUIRE(res.status == LoadStatus::Ok);
    REQUIRE(res.attach.old_class_names == std::vector<std::string>{"0","1"});
    REQUIRE(res.attach.classes.size() == 2);
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
