#include <catch2/catch_test_macros.hpp>

#include "hardware/format.h"

using denso::hardware::format_bytes;
using denso::hardware::size_or_unknown;

TEST_CASE("formats gigabytes whole") {
    REQUIRE(format_bytes(16'000'000'000ull) == "16 GB");
}

TEST_CASE("formats gigabytes rounded") {
    REQUIRE(format_bytes(512'110'190'592ull) == "512 GB");
}

TEST_CASE("formats terabytes one decimal") {
    REQUIRE(format_bytes(2'000'000'000'000ull) == "2.0 TB");
}

TEST_CASE("formats sub gigabyte as zero gb") {
    REQUIRE(format_bytes(500'000'000ull) == "0 GB");
}

TEST_CASE("size_or_unknown zero is unknown") {
    REQUIRE(size_or_unknown(0) == "Unknown");
}

TEST_CASE("size_or_unknown nonzero formats") {
    REQUIRE(size_or_unknown(16'000'000'000ull) == "16 GB");
}
