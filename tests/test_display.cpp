#include "settings/display.h"

#include <catch2/catch_test_macros.hpp>

using namespace denso::settings;

TEST_CASE("DisplayMode round-trips through string") {
    CHECK(std::string(to_string(DisplayMode::Windowed)) == "windowed");
    CHECK(std::string(to_string(DisplayMode::Borderless)) == "borderless");
    CHECK(std::string(to_string(DisplayMode::Fullscreen)) == "fullscreen");
    CHECK(parse_display_mode("windowed") == DisplayMode::Windowed);
    CHECK(parse_display_mode("borderless") == DisplayMode::Borderless);
    CHECK(parse_display_mode("fullscreen") == DisplayMode::Fullscreen);
}

TEST_CASE("parse_display_mode falls back to Windowed on unknown/corrupt") {
    CHECK(parse_display_mode("") == DisplayMode::Windowed);
    CHECK(parse_display_mode("garbage") == DisplayMode::Windowed);
    CHECK(parse_display_mode("FULLSCREEN") == DisplayMode::Windowed);  // case-sensitive
}
