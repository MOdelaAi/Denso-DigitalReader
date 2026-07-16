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

} // namespace denso::settings
