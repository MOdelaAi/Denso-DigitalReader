#include <catch2/catch_test_macros.hpp>

#include "brazing/brazing_payload.h"
#include "zone_value_compat.h"

using denso::ui::build_brazing_payload;

TEST_CASE("build_brazing_payload emits zones in ascending order", "[brazing_payload]") {
    CHECK(build_brazing_payload({{2, {200}}, {1, {500}}}) ==
          R"({"zone1":500,"zone2":200})");
}

TEST_CASE("build_brazing_payload of a single zone", "[brazing_payload]") {
    CHECK(build_brazing_payload({{2, {500}}}) == R"({"zone2":500})");
}

TEST_CASE("build_brazing_payload of no zones is an empty object", "[brazing_payload]") {
    CHECK(build_brazing_payload({}) == "{}");
}

// ── Wire-format regression guards ────────────────────────────────────────────
// The live-Backend-settings work changes WHERE and WHEN a payload is sent; it
// must change nothing about WHAT is sent. These two pin the byte-for-byte forms
// the running appliance and the PC test backend already exchange.

TEST_CASE("the Ball Leveler integer payload is unchanged", "[brazing_payload]") {
    // A whole-percent Ball value carries decimal_places 0 and serializes as a
    // bare integer — no point, no padding, exactly as before decimals existed.
    CHECK(build_brazing_payload({{1, denso::ui::ZoneValue{35, 0, 0}}}) ==
          R"({"zone1":35})");
    CHECK(build_brazing_payload({{1, denso::ui::ZoneValue{0, 0, 0}},
                                 {4, denso::ui::ZoneValue{100, 0, 0}}}) ==
          R"({"zone1":0,"zone4":100})");
}

TEST_CASE("the Digital Number decimal payload is unchanged", "[brazing_payload]") {
    // The three confirmed requests from the running application, verbatim: a
    // four-position digital face with two decimal places. display_digits is a
    // rendering width and must never reach the wire.
    CHECK(build_brazing_payload({{1, denso::ui::ZoneValue{300, 2, 4}}}) ==
          R"({"zone1":3.00})");
    CHECK(build_brazing_payload({{1, denso::ui::ZoneValue{30, 2, 4}}}) ==
          R"({"zone1":0.30})");
    CHECK(build_brazing_payload({{1, denso::ui::ZoneValue{3, 2, 4}}}) ==
          R"({"zone1":0.03})");
}
