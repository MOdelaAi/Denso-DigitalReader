#include <catch2/catch_test_macros.hpp>

#include "ui/camera/grid/grid_layout.h"

using denso::ui::grid_dims;
using denso::ui::GridDims;

TEST_CASE("one camera fills a single cell") {
    const GridDims d = grid_dims(1);
    REQUIRE(d.rows == 1);
    REQUIRE(d.cols == 1);
}

TEST_CASE("two cameras sit side by side") {
    const GridDims d = grid_dims(2);
    REQUIRE(d.rows == 1);
    REQUIRE(d.cols == 2);
}

TEST_CASE("three and four cameras use a 2x2 grid") {
    REQUIRE(grid_dims(3).rows == 2);
    REQUIRE(grid_dims(3).cols == 2);
    REQUIRE(grid_dims(4).rows == 2);
    REQUIRE(grid_dims(4).cols == 2);
}

TEST_CASE("counts are clamped to the 1..4 range") {
    REQUIRE(grid_dims(0).rows == 1);   // never zero cells
    REQUIRE(grid_dims(0).cols == 1);
    REQUIRE(grid_dims(9).rows == 2);   // capped at the 2x2 grid
    REQUIRE(grid_dims(9).cols == 2);
}
