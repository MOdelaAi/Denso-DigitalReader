#include <catch2/catch_test_macros.hpp>

#include "camera/source_change.h"

using denso::camera::Camera;
using denso::camera::requires_area_review;
using denso::camera::same_effective_source;
using denso::camera::view_geometry_changed;

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

TEST_CASE("identical camera → no review", "[source_change]") {
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

TEST_CASE("aspect-ratio change needs review", "[source_change]") {
    Camera before = ip_cam();  // 16:9
    Camera after = ip_cam();
    after.width = 1440;  // 1440x1080 = 4:3
    after.height = 1080;
    CHECK(view_geometry_changed(before, after));
    CHECK(requires_area_review(before, after));
}
