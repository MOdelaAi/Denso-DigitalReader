#include "settings/settings.h"

namespace denso::settings {

namespace {
constexpr int DEFAULT_INDEX = 2;
}

int preset_index(uint32_t width, uint32_t height) {
    for (size_t i = 0; i < PRESETS.size(); ++i) {
        if (PRESETS[i].first == width && PRESETS[i].second == height) {
            return static_cast<int>(i);
        }
    }
    return DEFAULT_INDEX;
}

} // namespace denso::settings
