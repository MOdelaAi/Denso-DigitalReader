#include <catch2/catch_test_macros.hpp>

#include "ui/camera/grid/stream_pacing.h"

using denso::ui::next_backoff_ms;
using denso::ui::should_emit;

TEST_CASE("next_backoff_ms ramps 1s -> x2 -> 10s cap", "[stream_pacing]") {
    CHECK(next_backoff_ms(0) == 1000);      // first attempt
    CHECK(next_backoff_ms(-5) == 1000);     // negative treated as first
    CHECK(next_backoff_ms(1000) == 2000);
    CHECK(next_backoff_ms(2000) == 4000);
    CHECK(next_backoff_ms(4000) == 8000);
    CHECK(next_backoff_ms(8000) == 10000);  // capped
    CHECK(next_backoff_ms(10000) == 10000); // stays capped
}

TEST_CASE("should_emit gates on in-flight count", "[stream_pacing]") {
    CHECK(should_emit(0, 2));
    CHECK(should_emit(1, 2));
    CHECK_FALSE(should_emit(2, 2));  // at cap -> drop
    CHECK_FALSE(should_emit(3, 2));  // over cap -> drop
}
