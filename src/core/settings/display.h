// Pure display-settings domain: the window display mode plus the transition
// logic the UI/window drive it with. Qt-free (std types only) so it stays in
// denso_core and is unit-tested off-device.
#pragma once

#include <cstdint>
#include <string>

namespace denso::settings {

enum class DisplayMode { Windowed, Borderless, Fullscreen };

/// Stable persisted token for a mode ("windowed"|"borderless"|"fullscreen").
const char* to_string(DisplayMode mode);

/// Parse a persisted token; any unknown/corrupt value is Windowed (never strand
/// startup in an unreachable mode).
DisplayMode parse_display_mode(const std::string& s);

} // namespace denso::settings
