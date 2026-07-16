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

TEST_CASE("display_changed: mode difference always counts") {
    DisplayState a{DisplayMode::Windowed, 1600, 900, "S1"};
    DisplayState b{DisplayMode::Fullscreen, 1600, 900, "S1"};
    CHECK(display_changed(a, b));
}

TEST_CASE("display_changed: size counts only in Windowed") {
    DisplayState w1{DisplayMode::Windowed, 1600, 900, "S1"};
    DisplayState w2{DisplayMode::Windowed, 1280, 720, "S1"};
    CHECK(display_changed(w1, w2));

    DisplayState f1{DisplayMode::Fullscreen, 1600, 900, "S1"};
    DisplayState f2{DisplayMode::Fullscreen, 1280, 720, "S1"};
    CHECK_FALSE(display_changed(f1, f2));  // size irrelevant in Fullscreen
}

TEST_CASE("display_changed: same state (incl. screen move) is no change") {
    DisplayState a{DisplayMode::Windowed, 1600, 900, "S1"};
    DisplayState same{DisplayMode::Windowed, 1600, 900, "S1"};
    DisplayState moved{DisplayMode::Windowed, 1600, 900, "S2"};
    CHECK_FALSE(display_changed(a, same));
    CHECK_FALSE(display_changed(a, moved));  // screen move alone doesn't confirm
}

TEST_CASE("plan_transition: canonical flags per mode (absolute, not incremental)") {
    DisplayState fromBorderless{DisplayMode::Borderless, 1600, 900, "S1"};
    // Borderless -> Fullscreen must clear frameless (no stale flag).
    auto fs = plan_transition(fromBorderless, DisplayMode::Fullscreen, 1600, 900, {true});
    CHECK(fs.mode == DisplayMode::Fullscreen);
    CHECK(fs.fullscreen);
    CHECK_FALSE(fs.frameless);
    CHECK(fs.geom == TransitionPlan::Geom::NativeFullscreen);

    auto win = plan_transition(fromBorderless, DisplayMode::Windowed, 1280, 720, {true});
    CHECK(win.mode == DisplayMode::Windowed);
    CHECK_FALSE(win.fullscreen);
    CHECK_FALSE(win.frameless);
    CHECK(win.geom == TransitionPlan::Geom::ResizeWithinScreen);
    CHECK(win.width == 1280);
    CHECK(win.height == 720);

    auto bl = plan_transition({DisplayMode::Windowed, 1600, 900, "S1"},
                              DisplayMode::Borderless, 1600, 900, {true});
    CHECK(bl.frameless);
    CHECK_FALSE(bl.fullscreen);
    CHECK(bl.geom == TransitionPlan::Geom::FullScreenRect);
}

TEST_CASE("plan_transition: no windowing capability forces Fullscreen") {
    DisplayState cur{DisplayMode::Windowed, 1600, 900, "S1"};
    auto p = plan_transition(cur, DisplayMode::Windowed, 1600, 900, {false});
    CHECK(p.mode == DisplayMode::Fullscreen);
    CHECK(p.fullscreen);
}

TEST_CASE("plan_transition: needs_confirm follows display_changed") {
    DisplayState cur{DisplayMode::Windowed, 1600, 900, "S1"};
    CHECK(plan_transition(cur, DisplayMode::Fullscreen, 1600, 900, {true}).needs_confirm);
    CHECK_FALSE(plan_transition(cur, DisplayMode::Windowed, 1600, 900, {true}).needs_confirm);
}
