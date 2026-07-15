#include <catch2/catch_test_macros.hpp>

#include "camera/camera.h"
#include "camera/repo.h"
#include "db/db.h"

#include <optional>
#include <utility>

using denso::camera::all;
using denso::camera::areas_for;
using denso::camera::Camera;
using denso::camera::CameraArea;
using denso::camera::get;
using denso::camera::insert;
using denso::camera::Point;
using denso::camera::remove;
using denso::camera::replace_areas;
using denso::camera::set_areas_need_review;
using denso::camera::update;
using denso::db::Db;
using denso::db::run_migrations;

namespace {

Db db() {
    auto d = Db::open_in_memory();
    REQUIRE(d.has_value());
    REQUIRE(run_migrations(d->handle()));
    return std::move(*d);
}

Camera usb_cam() {
    Camera c;
    c.name = "Front USB";
    c.camera_type = "usb";
    c.active = true;
    c.index = 0;
    c.width = 1280;
    c.height = 720;
    c.fps = 30;
    c.pitch = 1.5f;
    c.roll = -2.0f;
    c.rotation = 90;
    return c;
}

Camera ip_cam() {
    Camera c;
    c.name = "Line IP";
    c.camera_type = "ip";
    c.active = false;
    c.ip = "192.168.1.20";
    c.rtsp = "rtsp://192.168.1.20:554/stream";
    c.username = "admin";
    c.password = "secret";
    c.channel = 3;
    c.stream = 1;
    c.manufacturer = "Dahua";
    return c;
}

} // namespace

TEST_CASE("insert returns an id and get round-trips a USB camera") {
    auto d = db();
    const auto id = insert(d.handle(), usb_cam());
    REQUIRE(id.has_value());

    const auto got = get(d.handle(), *id);
    REQUIRE(got.has_value());
    REQUIRE(got->id == *id);
    REQUIRE(got->name == "Front USB");
    REQUIRE(got->camera_type == "usb");
    REQUIRE(got->active);
    REQUIRE(got->index == 0u);
    REQUIRE_FALSE(got->ip.has_value());
    REQUIRE_FALSE(got->rtsp.has_value());
    REQUIRE_FALSE(got->username.has_value());
    REQUIRE(got->width == 1280u);
    REQUIRE(got->height == 720u);
    REQUIRE(got->fps == 30u);
    REQUIRE(got->pitch == 1.5f);
    REQUIRE(got->roll == -2.0f);
    REQUIRE(got->rotation == 90u);
}

TEST_CASE("IP camera round-trips with null usb index") {
    auto d = db();
    const auto id = insert(d.handle(), ip_cam());
    REQUIRE(id.has_value());

    const auto got = get(d.handle(), *id);
    REQUIRE(got.has_value());
    REQUIRE(got->camera_type == "ip");
    REQUIRE_FALSE(got->active);
    REQUIRE_FALSE(got->index.has_value());
    REQUIRE(got->ip == "192.168.1.20");
    REQUIRE(got->rtsp == "rtsp://192.168.1.20:554/stream");
    REQUIRE(got->username == "admin");
    REQUIRE(got->password == "secret");
    REQUIRE(got->channel == 3u);
    REQUIRE(got->stream == 1u);
    REQUIRE(got->manufacturer == "Dahua");
}

TEST_CASE("USB camera has no password") {
    auto d = db();
    const auto id = insert(d.handle(), usb_cam());
    REQUIRE(id.has_value());
    const auto got = get(d.handle(), *id);
    REQUIRE(got.has_value());
    REQUIRE_FALSE(got->password.has_value());
}

TEST_CASE("USB camera has no channel, stream, or manufacturer") {
    auto d = db();
    const auto id = insert(d.handle(), usb_cam());
    REQUIRE(id.has_value());
    const auto got = get(d.handle(), *id);
    REQUIRE(got.has_value());
    REQUIRE_FALSE(got->channel.has_value());
    REQUIRE_FALSE(got->stream.has_value());
    REQUIRE_FALSE(got->manufacturer.has_value());
}

TEST_CASE("get returns nullopt for a missing id") {
    auto d = db();
    REQUIRE_FALSE(get(d.handle(), 999).has_value());
}

TEST_CASE("all returns every camera ordered by id") {
    auto d = db();
    const auto a = insert(d.handle(), usb_cam());
    const auto b = insert(d.handle(), ip_cam());
    REQUIRE(a.has_value());
    REQUIRE(b.has_value());

    const auto cams = all(d.handle());
    REQUIRE(cams.size() == 2);
    REQUIRE(cams[0].id == *a);
    REQUIRE(cams[1].id == *b);
    REQUIRE(cams[0].camera_type == "usb");
    REQUIRE(cams[1].camera_type == "ip");
}

TEST_CASE("update changes fields in place") {
    auto d = db();
    const auto id = insert(d.handle(), usb_cam());
    REQUIRE(id.has_value());

    Camera edited = usb_cam();
    edited.id = *id;
    edited.name = "Renamed";
    edited.active = false;
    edited.rotation = 180;
    edited.index = 2;
    REQUIRE(update(d.handle(), edited));

    const auto got = get(d.handle(), *id);
    REQUIRE(got.has_value());
    REQUIRE(got->name == "Renamed");
    REQUIRE_FALSE(got->active);
    REQUIRE(got->rotation == 180u);
    REQUIRE(got->index == 2u);
}

TEST_CASE("remove deletes the camera") {
    auto d = db();
    const auto id = insert(d.handle(), usb_cam());
    REQUIRE(id.has_value());
    REQUIRE(remove(d.handle(), *id));
    REQUIRE_FALSE(get(d.handle(), *id).has_value());
    REQUIRE(all(d.handle()).empty());
}

TEST_CASE("areas_for is empty for a camera with no ROIs") {
    auto d = db();
    const auto id = insert(d.handle(), usb_cam());
    REQUIRE(id.has_value());
    REQUIRE(areas_for(d.handle(), *id).empty());
}

TEST_CASE("replace_areas saves named polygons and areas_for round-trips them") {
    auto d = db();
    const auto id = insert(d.handle(), usb_cam());
    REQUIRE(id.has_value());

    CameraArea tri;
    tri.camera_id = *id;
    tri.name = "Display A";
    tri.points = {{0.2f, 0.3f}, {0.6f, 0.3f}, {0.4f, 0.7f}};
    CameraArea rect;
    rect.camera_id = *id;
    rect.name = "Display B";
    rect.points = {{0.1f, 0.1f}, {0.9f, 0.1f}, {0.9f, 0.5f}, {0.1f, 0.5f}};
    REQUIRE(replace_areas(d.handle(), *id, {tri, rect}));

    const auto got = areas_for(d.handle(), *id);
    REQUIRE(got.size() == 2);
    REQUIRE(got[0].name == "Display A");
    REQUIRE(got[0].camera_id == *id);
    REQUIRE(got[0].points.size() == 3);
    REQUIRE(got[0].points[2].x == 0.4f);
    REQUIRE(got[1].name == "Display B");
    REQUIRE(got[1].points.size() == 4);
}

TEST_CASE("areas_need_review round-trips and set_areas_need_review toggles it") {
    auto d = db();
    const auto id = insert(d.handle(), usb_cam());
    REQUIRE(id.has_value());
    REQUIRE_FALSE(get(d.handle(), *id)->areas_need_review);  // default off

    REQUIRE(set_areas_need_review(d.handle(), *id, true));
    REQUIRE(get(d.handle(), *id)->areas_need_review);

    REQUIRE(set_areas_need_review(d.handle(), *id, false));
    REQUIRE_FALSE(get(d.handle(), *id)->areas_need_review);
}

TEST_CASE("replace_areas clears the areas_need_review quarantine (verification)") {
    auto d = db();
    const auto id = insert(d.handle(), usb_cam());
    REQUIRE(id.has_value());
    REQUIRE(set_areas_need_review(d.handle(), *id, true));

    CameraArea a;
    a.camera_id = *id;
    a.name = "Verified";
    a.points = {{0.1f, 0.1f}, {0.9f, 0.1f}, {0.5f, 0.9f}};
    REQUIRE(replace_areas(d.handle(), *id, {a}));  // saving = verifying
    REQUIRE_FALSE(get(d.handle(), *id)->areas_need_review);  // quarantine cleared
}

TEST_CASE("replace_areas overwrites the previous set for that camera") {
    auto d = db();
    const auto id = insert(d.handle(), usb_cam());
    REQUIRE(id.has_value());

    CameraArea a;
    a.camera_id = *id;
    a.name = "First";
    a.points = {{0.0f, 0.0f}, {1.0f, 0.0f}, {0.5f, 1.0f}};
    REQUIRE(replace_areas(d.handle(), *id, {a, a, a}));
    REQUIRE(areas_for(d.handle(), *id).size() == 3);

    CameraArea b;
    b.camera_id = *id;
    b.name = "Only";
    b.points = {{0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}};
    REQUIRE(replace_areas(d.handle(), *id, {b}));
    const auto got = areas_for(d.handle(), *id);
    REQUIRE(got.size() == 1);
    REQUIRE(got[0].name == "Only");
}

TEST_CASE("replace_areas with an empty set clears a camera's ROIs") {
    auto d = db();
    const auto id = insert(d.handle(), usb_cam());
    REQUIRE(id.has_value());
    CameraArea a;
    a.camera_id = *id;
    a.name = "X";
    a.points = {{0.0f, 0.0f}, {1.0f, 0.0f}, {0.5f, 1.0f}};
    REQUIRE(replace_areas(d.handle(), *id, {a}));
    REQUIRE(replace_areas(d.handle(), *id, {}));
    REQUIRE(areas_for(d.handle(), *id).empty());
}

TEST_CASE("remove deletes the camera's areas too") {
    auto d = db();
    const auto id = insert(d.handle(), usb_cam());
    REQUIRE(id.has_value());
    CameraArea a;
    a.camera_id = *id;
    a.name = "X";
    a.points = {{0.0f, 0.0f}, {1.0f, 0.0f}, {0.5f, 1.0f}};
    REQUIRE(replace_areas(d.handle(), *id, {a}));
    REQUIRE(remove(d.handle(), *id));
    REQUIRE(areas_for(d.handle(), *id).empty());
}

TEST_CASE("replace_areas round-trips the zone number", "[camera_repo]") {
    auto d = db();
    const auto id = insert(d.handle(), usb_cam());
    REQUIRE(id.has_value());

    CameraArea a;
    a.name = "z";
    a.zone = 3;
    a.points = {{0.1f, 0.1f}, {0.9f, 0.1f}, {0.5f, 0.9f}};
    CameraArea b;
    b.name = "roi_only";  // zone stays nullopt
    b.points = {{0.2f, 0.2f}, {0.8f, 0.2f}, {0.5f, 0.8f}};
    REQUIRE(replace_areas(d.handle(), *id, {a, b}));

    const auto got = areas_for(d.handle(), *id);
    REQUIRE(got.size() == 2);
    CHECK(got[0].zone == 3);
    CHECK_FALSE(got[1].zone.has_value());
}

TEST_CASE("replace_areas rejects a zone already used by another camera", "[camera_repo]") {
    auto d = db();
    const auto a = insert(d.handle(), usb_cam());
    const auto b = insert(d.handle(), ip_cam());
    REQUIRE(a.has_value());
    REQUIRE(b.has_value());

    CameraArea za;
    za.name = "A-zone1";
    za.zone = 1;
    za.points = {{0.1f, 0.1f}, {0.9f, 0.1f}, {0.5f, 0.9f}};
    REQUIRE(replace_areas(d.handle(), *a, {za}));

    // Camera B tries to claim the same machine-wide zone number.
    CameraArea zb;
    zb.name = "B-zone1";
    zb.zone = 1;
    zb.points = {{0.2f, 0.2f}, {0.8f, 0.2f}, {0.5f, 0.8f}};
    CHECK_FALSE(replace_areas(d.handle(), *b, {zb}));

    // The rejected save left B with no areas and A's zone intact.
    CHECK(areas_for(d.handle(), *b).empty());
    const auto ga = areas_for(d.handle(), *a);
    REQUIRE(ga.size() == 1);
    CHECK(ga[0].zone == 1);
}

TEST_CASE("replace_areas lets a camera keep its own zone on re-save", "[camera_repo]") {
    auto d = db();
    const auto a = insert(d.handle(), usb_cam());
    REQUIRE(a.has_value());

    CameraArea za;
    za.name = "A-zone2";
    za.zone = 2;
    za.points = {{0.1f, 0.1f}, {0.9f, 0.1f}, {0.5f, 0.9f}};
    REQUIRE(replace_areas(d.handle(), *a, {za}));
    // Editing the same camera and re-saving the same zone must not self-conflict.
    CHECK(replace_areas(d.handle(), *a, {za}));
    CHECK(areas_for(d.handle(), *a).size() == 1);
}

TEST_CASE("replace_areas treats zone 0 as ROI-only, not a unique zone", "[camera_repo]") {
    auto d = db();
    const auto a = insert(d.handle(), usb_cam());
    REQUIRE(a.has_value());

    // Zone 0 means "ROI-only, not reported" (mirrors the UI's 0 = none). Multiple
    // such areas must coexist without tripping the machine-wide uniqueness guard.
    CameraArea r1;
    r1.name = "roi-a";
    r1.zone = 0;
    r1.points = {{0.1f, 0.1f}, {0.4f, 0.1f}, {0.25f, 0.4f}};
    CameraArea r2;
    r2.name = "roi-b";
    r2.zone = 0;
    r2.points = {{0.6f, 0.6f}, {0.9f, 0.6f}, {0.75f, 0.9f}};
    CHECK(replace_areas(d.handle(), *a, {r1, r2}));
    CHECK(areas_for(d.handle(), *a).size() == 2);
}

TEST_CASE("replace_areas rejects duplicate zone numbers within one camera", "[camera_repo]") {
    auto d = db();
    const auto a = insert(d.handle(), usb_cam());
    REQUIRE(a.has_value());

    CameraArea z1;
    z1.name = "one";
    z1.zone = 5;
    z1.points = {{0.1f, 0.1f}, {0.4f, 0.1f}, {0.25f, 0.4f}};
    CameraArea z2;
    z2.name = "two";
    z2.zone = 5;  // same zone number in the same save
    z2.points = {{0.6f, 0.6f}, {0.9f, 0.6f}, {0.75f, 0.9f}};
    CHECK_FALSE(replace_areas(d.handle(), *a, {z1, z2}));
    CHECK(areas_for(d.handle(), *a).empty());
}
