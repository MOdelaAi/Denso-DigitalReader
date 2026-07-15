#include <catch2/catch_test_macros.hpp>

#include "brazing/brazing_payload.h"

using denso::ui::build_brazing_payload;

TEST_CASE("build_brazing_payload emits zones in ascending order", "[brazing_payload]") {
    CHECK(build_brazing_payload({{2, 200}, {1, 500}}) ==
          R"({"zone1":500,"zone2":200})");
}

TEST_CASE("build_brazing_payload of a single zone", "[brazing_payload]") {
    CHECK(build_brazing_payload({{2, 500}}) == R"({"zone2":500})");
}

TEST_CASE("build_brazing_payload of no zones is an empty object", "[brazing_payload]") {
    CHECK(build_brazing_payload({}) == "{}");
}
