#include <catch2/catch_test_macros.hpp>

#include "logging/log_rotation.h"
#include "logging/redact.h"

using denso::logging::rotated_path;
using denso::logging::sanitize_url;
using denso::logging::should_rotate;

TEST_CASE("should_rotate: never rotate an empty file", "[log]") {
    CHECK_FALSE(should_rotate(0, 10, 100));
    CHECK_FALSE(should_rotate(0, 1'000'000, 100));  // oversized record still lands
}

TEST_CASE("should_rotate: rotate only when the record would overflow", "[log]") {
    CHECK_FALSE(should_rotate(50, 40, 100));  // 90 <= 100
    CHECK_FALSE(should_rotate(60, 40, 100));  // 100 == cap, still fits
    CHECK(should_rotate(70, 40, 100));        // 110 > 100
}

TEST_CASE("rotated_path builds the .N archive names", "[log]") {
    CHECK(rotated_path("denso.log", 0) == "denso.log");
    CHECK(rotated_path("denso.log", 1) == "denso.log.1");
    CHECK(rotated_path("denso.log", 4) == "denso.log.4");
    CHECK(rotated_path("/var/log/denso.log", 2) == "/var/log/denso.log.2");
}

TEST_CASE("sanitize_url strips credentials and query", "[log]") {
    CHECK(sanitize_url("rtsp://admin:secret@192.168.1.50:554/cam/realmonitor?channel=1")
          == "rtsp://192.168.1.50:554/cam/realmonitor");
    CHECK(sanitize_url("rtsp://192.168.1.50:554/stream") ==
          "rtsp://192.168.1.50:554/stream");
    CHECK(sanitize_url("http://host/api?token=abc") == "http://host/api");
    CHECK(sanitize_url("not a url") == "not a url");
    // '@' only counts as userinfo when it precedes the first '/'.
    CHECK(sanitize_url("http://host/path@notcreds") == "http://host/path@notcreds");
}
