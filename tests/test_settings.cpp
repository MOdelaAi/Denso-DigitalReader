#include <catch2/catch_test_macros.hpp>

#include "settings/settings.h"

#include <vector>

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
    REQUIRE(s.mode == denso::settings::DisplayMode::Windowed);
}

TEST_CASE("fitting_presets returns indices whose FRAMED size fits") {
    // Frame overhead 20x40. 1920x1080 needs 1940x1120.
    auto all = denso::settings::fitting_presets(3000, 2000, 20, 40);
    CHECK(all.size() == denso::settings::PRESETS.size());  // all fit on a big screen

    // 1600x900 (index 2) framed = 1620x940; 1920x1080 framed = 1940x1120.
    auto some = denso::settings::fitting_presets(1620, 940, 20, 40);
    CHECK(some == std::vector<int>{0, 1, 2});  // 1920x1080 excluded
}

TEST_CASE("fitting_presets is empty when nothing fits, never a lie") {
    auto none = denso::settings::fitting_presets(500, 400, 20, 40);
    CHECK(none.empty());  // even 800x600 doesn't fit
}

TEST_CASE("largest_fitting_preset picks the biggest fitting, else -1") {
    CHECK(denso::settings::largest_fitting_preset(1620, 940, 20, 40) == 2);
    CHECK(denso::settings::largest_fitting_preset(3000, 2000, 20, 40) == 3);
    CHECK(denso::settings::largest_fitting_preset(500, 400, 20, 40) == -1);
}

TEST_CASE("fitting_presets does not overflow with a huge frame overhead") {
    // frame_w near UINT32_MAX must never wrap to look like it fits a small screen.
    auto none = denso::settings::fitting_presets(1920, 1080, 0xFFFFFF00u, 0xFFFFFF00u);
    CHECK(none.empty());
}
