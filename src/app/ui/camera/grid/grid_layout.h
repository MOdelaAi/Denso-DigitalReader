// Pure layout math for the live camera grid: how many rows/cols hold N feeds.
// 1 → 1×1, 2 → 1×2 (side by side), 3–4 → 2×2. N is clamped to 1..4 (the grid
// shows at most the first four cameras). Tile i sits at row i/cols, col i%cols.
// No Qt dependency, so it's unit-tested off-screen.
#pragma once

namespace denso::ui {

struct GridDims {
    int rows;
    int cols;
};

GridDims grid_dims(int count);

} // namespace denso::ui
