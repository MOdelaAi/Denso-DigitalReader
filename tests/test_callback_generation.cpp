#include <catch2/catch_test_macros.hpp>
#include "camera/callback_generation.h"

TEST_CASE("a callback from the same generation is delivered", "[callback_generation]") {
    CHECK(denso::camera::callback_is_current(7, 7));
}
TEST_CASE("a callback from an older generation is dropped", "[callback_generation]") {
    // Old worker captured gen 3; grid has since rebuilt to gen 4.
    CHECK_FALSE(denso::camera::callback_is_current(3, 4));
}
