#include <catch2/catch_test_macros.hpp>

#include "detection/class_names.h"

using denso::detection::parse_class_names;
using denso::detection::serialize_class_names;

TEST_CASE("class names round-trip through serialize/parse") {
    std::vector<std::string> names = {"0", "1", "person", "hard hat"};
    const std::string text = serialize_class_names(names);
    REQUIRE(parse_class_names(text) == names);
}

TEST_CASE("empty class names round-trip to empty") {
    REQUIRE(serialize_class_names({}).size() >= 0);
    REQUIRE(parse_class_names(serialize_class_names({})).empty());
}

TEST_CASE("parse tolerates blank text") {
    REQUIRE(parse_class_names("").empty());
    REQUIRE(parse_class_names("not json").empty());
}
