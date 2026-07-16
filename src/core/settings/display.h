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

/// Semantic display state — what the user chose, independent of raw Qt window
/// flags. `width`/`height` are the Windowed client size.
struct DisplayState {
    DisplayMode mode = DisplayMode::Windowed;
    uint32_t width = 1600;
    uint32_t height = 900;
    std::string screen_name;

    bool operator==(const DisplayState&) const = default;
};

/// A change worth a confirm/revert transaction: mode differs, or (both Windowed
/// and the window size differs). Screen moves and size changes in
/// Borderless/Fullscreen do not count.
bool display_changed(const DisplayState& from, const DisplayState& to);

/// What the current platform can actually do. On a WM-less stack (eglfs/linuxfb)
/// windowed/borderless are fictional, so only Fullscreen is real.
struct PlatformCaps {
    bool windowing = true;
};

/// The canonical target of a mode transition — absolute, so it never inherits a
/// stale flag from the previous mode. The window layer executes this.
struct TransitionPlan {
    DisplayMode mode = DisplayMode::Windowed;
    bool frameless = false;
    bool fullscreen = false;
    enum class Geom { ResizeWithinScreen, FullScreenRect, NativeFullscreen } geom =
        Geom::ResizeWithinScreen;
    // The windowed client size to apply (Windowed) or carry forward (other
    // modes). Only Geom::ResizeWithinScreen consumes it, and the window layer
    // persists it only in Windowed — so it never clobbers the remembered size.
    uint32_t width = 1600;
    uint32_t height = 900;
    bool needs_confirm = false;
};

/// Plan a transition from `current` to `requested` at the requested windowed
/// size, honoring platform capability. `req_w`/`req_h` matter only for Windowed.
TransitionPlan plan_transition(const DisplayState& current, DisplayMode requested,
                               uint32_t req_w, uint32_t req_h, PlatformCaps caps);

} // namespace denso::settings
