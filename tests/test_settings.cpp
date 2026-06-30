#include <catch2/catch_test_macros.hpp>

#include "settings/settings.h"

using denso::settings::preset_index;
using denso::settings::Settings;

TEST_CASE("preset_index matches known size") {
    REQUIRE(preset_index(800, 600) == 0);
    REQUIRE(preset_index(1920, 1080) == 3);
}

TEST_CASE("preset_index falls back for unknown size") {
    REQUIRE(preset_index(1234, 567) == 2);
}

TEST_CASE("default settings are dark 1600x900 windowed") {
    Settings s;
    REQUIRE(s.width == 1600);
    REQUIRE(s.height == 900);
    REQUIRE(s.dark);
    REQUIRE_FALSE(s.fullscreen);
}
