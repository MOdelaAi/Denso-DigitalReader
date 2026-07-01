#include "ui/camera/grid/grid_layout.h"

#include <algorithm>

namespace denso::ui {

GridDims grid_dims(int count) {
    const int n = std::clamp(count, 1, 4);
    if (n == 1) return {1, 1};
    if (n == 2) return {1, 2};
    return {2, 2};  // 3 or 4
}

} // namespace denso::ui
