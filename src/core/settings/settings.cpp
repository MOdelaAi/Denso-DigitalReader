#include "settings/settings.h"

#include <cstddef>

namespace denso::settings {

namespace {
constexpr int DEFAULT_INDEX = 2;

// largest_fitting_preset returns fitting_presets().back(), which is the largest
// preset only if PRESETS is strictly ascending. Enforce that invariant here.
constexpr bool presets_strictly_ascending() {
    for (std::size_t i = 1; i < PRESETS.size(); ++i) {
        if (!(PRESETS[i - 1].first < PRESETS[i].first &&
              PRESETS[i - 1].second < PRESETS[i].second)) {
            return false;
        }
    }
    return true;
}
static_assert(presets_strictly_ascending(),
              "PRESETS must be strictly ascending; largest_fitting_preset relies on it");
}

int preset_index(uint32_t width, uint32_t height) {
    for (std::size_t i = 0; i < PRESETS.size(); ++i) {
        if (PRESETS[i].first == width && PRESETS[i].second == height) {
            return static_cast<int>(i);
        }
    }
    return DEFAULT_INDEX;
}

std::vector<int> fitting_presets(uint32_t avail_w, uint32_t avail_h,
                                 uint32_t frame_w, uint32_t frame_h) {
    std::vector<int> out;
    for (int i = 0; i < static_cast<int>(PRESETS.size()); ++i) {
        const auto [w, h] = PRESETS[static_cast<std::size_t>(i)];
        // Widen to avoid a uint32 wrap making an oversized preset look like it fits.
        if (static_cast<uint64_t>(w) + frame_w <= avail_w &&
            static_cast<uint64_t>(h) + frame_h <= avail_h) {
            out.push_back(i);
        }
    }
    return out;
}

int largest_fitting_preset(uint32_t avail_w, uint32_t avail_h,
                           uint32_t frame_w, uint32_t frame_h) {
    const std::vector<int> fit = fitting_presets(avail_w, avail_h, frame_w, frame_h);
    return fit.empty() ? -1 : fit.back();  // PRESETS ascending -> back() is largest
}

} // namespace denso::settings
