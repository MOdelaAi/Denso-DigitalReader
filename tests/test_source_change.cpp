#include <catch2/catch_test_macros.hpp>

#include "camera/source_change.h"

#include <string>

using denso::camera::Camera;
using denso::camera::requires_area_review;
using denso::camera::same_effective_source;
using denso::camera::view_geometry_changed;
using denso::camera::view_revision;

namespace {
Camera usb_cam() {
    Camera c;
    c.camera_type = "usb";
    c.index = 0u;
    c.width = 1920;
    c.height = 1080;
    return c;
}
Camera ip_cam() {
    Camera c;
    c.camera_type = "ip";
    c.ip = "192.168.1.50";
    c.rtsp = "rtsp://192.168.1.50:554/cam/realmonitor?channel=1&subtype=0";
    c.manufacturer = "Dahua";
    c.channel = 1u;
    c.stream = 0u;
    c.width = 1920;
    c.height = 1080;
    return c;
}
} // namespace

TEST_CASE("identical camera -> no review", "[source_change]") {
    CHECK(same_effective_source(ip_cam(), ip_cam()));
    CHECK_FALSE(view_geometry_changed(ip_cam(), ip_cam()));
    CHECK_FALSE(requires_area_review(ip_cam(), ip_cam()));
}

TEST_CASE("credential / name changes are NOT view-significant", "[source_change]") {
    Camera before = ip_cam();
    Camera after = ip_cam();
    after.name = "Renamed";
    after.username = "admin";
    after.password = "secret";
    CHECK(same_effective_source(before, after));
    CHECK_FALSE(requires_area_review(before, after));
}

TEST_CASE("changing the IP / rtsp / channel / stream needs review", "[source_change]") {
    Camera before = ip_cam();
    {
        Camera a = ip_cam(); a.ip = "192.168.1.99";
        CHECK_FALSE(same_effective_source(before, a));
        CHECK(requires_area_review(before, a));
    }
    {
        Camera a = ip_cam(); a.channel = 2u;
        CHECK(requires_area_review(before, a));
    }
    {
        Camera a = ip_cam(); a.stream = 1u;
        CHECK(requires_area_review(before, a));
    }
    {
        Camera a = ip_cam(); a.rtsp = "rtsp://192.168.1.50:554/other";
        CHECK(requires_area_review(before, a));
    }
}

TEST_CASE("changing the USB index needs review", "[source_change]") {
    Camera before = usb_cam();
    Camera after = usb_cam();
    after.index = 2u;
    CHECK_FALSE(same_effective_source(before, after));
    CHECK(requires_area_review(before, after));
}

TEST_CASE("switching camera type needs review", "[source_change]") {
    CHECK_FALSE(same_effective_source(usb_cam(), ip_cam()));
    CHECK(requires_area_review(usb_cam(), ip_cam()));
}

TEST_CASE("geometry: rotation / pitch / roll changes need review", "[source_change]") {
    Camera before = ip_cam();
    {
        Camera a = ip_cam(); a.rotation = 90;
        CHECK(view_geometry_changed(before, a));
        CHECK(requires_area_review(before, a));
    }
    {
        Camera a = ip_cam(); a.pitch = 5.0f;
        CHECK(view_geometry_changed(before, a));
    }
    {
        Camera a = ip_cam(); a.roll = -3.0f;
        CHECK(view_geometry_changed(before, a));
    }
}

TEST_CASE("resolution change at the SAME aspect ratio is safe", "[source_change]") {
    Camera before = ip_cam();  // 1920x1080 = 16:9
    Camera after = ip_cam();
    after.width = 1280;  // 1280x720 = 16:9
    after.height = 720;
    CHECK_FALSE(view_geometry_changed(before, after));
    CHECK_FALSE(requires_area_review(before, after));
}

TEST_CASE("legacy unset (0x0) resolution -> real preset needs review", "[source_change]") {
    Camera before = ip_cam();
    before.width = 0;
    before.height = 0;  // legacy camera with no stored geometry
    Camera after = ip_cam();  // Configure resolves it to 1920x1080
    CHECK(view_geometry_changed(before, after));
    CHECK(requires_area_review(before, after));
}

TEST_CASE("aspect-ratio change needs review", "[source_change]") {
    Camera before = ip_cam();  // 16:9
    Camera after = ip_cam();
    after.width = 1440;  // 1440x1080 = 4:3
    after.height = 1080;
    CHECK(view_geometry_changed(before, after));
    CHECK(requires_area_review(before, after));
}

// ─────────────────────────────────────────────────────────────────────────────
// view_revision — the fingerprint the Ball Leveler calibration is stored against.
//
// It exists so `ball_level_calibration.view_revision` can be produced from ONE
// place. Its contract is defined ENTIRELY in terms of the predicates above: two
// cameras share a revision exactly when neither the effective source nor the view
// geometry changed. Testing it against `requires_area_review` rather than against
// a literal digest is deliberate — the digest is an implementation detail, the
// agreement is the rule.
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("view_revision agrees with requires_area_review", "[source_change]") {
    const Camera before = ip_cam();

    SECTION("a view-significant change alters the revision") {
        Camera after = ip_cam();
        after.rotation = 90;
        REQUIRE(requires_area_review(before, after));
        CHECK(view_revision(before) != view_revision(after));
    }
    SECTION("a credential/name-only edit keeps it") {
        Camera after = ip_cam();
        after.name = "Renamed";
        after.username = "admin";
        after.password = "secret";
        REQUIRE_FALSE(requires_area_review(before, after));
        CHECK(view_revision(before) == view_revision(after));
    }
    SECTION("a same-aspect resolution change keeps it") {
        Camera after = ip_cam();
        after.width = 1280;
        after.height = 720;
        REQUIRE_FALSE(requires_area_review(before, after));
        CHECK(view_revision(before) == view_revision(after));
    }
    SECTION("an aspect change alters it") {
        Camera after = ip_cam();
        after.width = 1440;
        after.height = 1080;
        REQUIRE(requires_area_review(before, after));
        CHECK(view_revision(before) != view_revision(after));
    }
    SECTION("a USB camera and an IP camera never share one") {
        CHECK(view_revision(usb_cam()) != view_revision(ip_cam()));
    }
}

TEST_CASE("view_revision is stable and carries no credential", "[source_change]") {
    Camera c = ip_cam();
    c.username = "admin";
    c.password = "hunter2";
    const std::string rev = view_revision(c);

    // Opaque and fixed-width: it is stored in the database and may reach a
    // diagnostic, so it must never be able to carry a credential-bearing URL.
    CHECK(rev.size() == 64);
    CHECK(rev.find("hunter2") == std::string::npos);
    CHECK(rev.find("admin") == std::string::npos);
    CHECK(rev.find("192.168.1.50") == std::string::npos);
    // Deterministic across calls — a fresh fingerprint on every save would
    // invalidate every stored calibration.
    CHECK(view_revision(c) == rev);
    // …and across an identical, separately constructed camera.
    Camera same = ip_cam();
    same.username = "admin";
    same.password = "hunter2";
    CHECK(view_revision(same) == rev);
}

TEST_CASE("view_revision distinguishes rotation, pitch and roll", "[source_change]") {
    const Camera base = ip_cam();
    Camera rot = ip_cam(); rot.rotation = 180;
    Camera pitch = ip_cam(); pitch.pitch = 3.5f;
    Camera roll = ip_cam(); roll.roll = -2.0f;
    CHECK(view_revision(base) != view_revision(rot));
    CHECK(view_revision(base) != view_revision(pitch));
    CHECK(view_revision(base) != view_revision(roll));
    CHECK(view_revision(rot) != view_revision(pitch));
}
