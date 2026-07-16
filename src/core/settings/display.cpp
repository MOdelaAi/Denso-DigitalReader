#include "settings/display.h"

namespace denso::settings {

const char* to_string(DisplayMode mode) {
    switch (mode) {
        case DisplayMode::Borderless: return "borderless";
        case DisplayMode::Fullscreen: return "fullscreen";
        case DisplayMode::Windowed:   break;
    }
    return "windowed";
}

DisplayMode parse_display_mode(const std::string& s) {
    if (s == "borderless") return DisplayMode::Borderless;
    if (s == "fullscreen") return DisplayMode::Fullscreen;
    return DisplayMode::Windowed;  // "windowed" + every unknown value
}

bool display_changed(const DisplayState& from, const DisplayState& to) {
    if (from.mode != to.mode) return true;
    if (from.mode == DisplayMode::Windowed &&
        (from.width != to.width || from.height != to.height)) {
        return true;
    }
    return false;
}

TransitionPlan plan_transition(const DisplayState& current, DisplayMode requested,
                               uint32_t req_w, uint32_t req_h, PlatformCaps caps) {
    const DisplayMode eff = caps.windowing ? requested : DisplayMode::Fullscreen;

    TransitionPlan p;
    p.mode = eff;
    p.width = req_w;
    p.height = req_h;
    switch (eff) {
        case DisplayMode::Windowed:
            p.frameless = false; p.fullscreen = false;
            p.geom = TransitionPlan::Geom::ResizeWithinScreen;
            break;
        case DisplayMode::Borderless:
            p.frameless = true; p.fullscreen = false;
            p.geom = TransitionPlan::Geom::FullScreenRect;
            break;
        case DisplayMode::Fullscreen:
            p.frameless = false; p.fullscreen = true;
            p.geom = TransitionPlan::Geom::NativeFullscreen;
            break;
    }
    const DisplayState target{eff, req_w, req_h, current.screen_name};
    p.needs_confirm = display_changed(current, target);
    return p;
}

} // namespace denso::settings
