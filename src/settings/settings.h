// Persisted application settings (window size, theme, fullscreen) plus the
// resolution presets. Ported from Rust `settings::model` + the pure preset
// helper in `settings::repo`. Persistence (load/save/import) is the DB layer.
#pragma once

#include <array>
#include <cstdint>
#include <utility>

namespace denso::settings {

struct Settings {
    uint32_t width = 1600;
    uint32_t height = 900;
    bool dark = true;
    bool fullscreen = false;
};

/// Selectable window resolutions, in display order. Index 2 (1600×900) is the
/// default.
inline constexpr std::array<std::pair<uint32_t, uint32_t>, 4> PRESETS = {{
    {800, 600},
    {1280, 720},
    {1600, 900},
    {1920, 1080},
}};

/// Index into PRESETS matching the given size, or the default index (2) when
/// no preset matches.
int preset_index(uint32_t width, uint32_t height);

} // namespace denso::settings
