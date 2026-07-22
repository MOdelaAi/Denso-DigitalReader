#pragma once
#include <cstdint>
namespace denso::camera {
// A queued worker callback belongs to grid generation `captured`; it must be
// dropped once the grid has rebuilt (generation advanced). Pure + unit-tested.
inline bool callback_is_current(uint64_t captured, uint64_t live) {
    return captured == live;
}
}
