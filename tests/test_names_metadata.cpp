#include <catch2/catch_test_macros.hpp>

#include "ui/camera/shared/detection/names_metadata.h"

using denso::ui::parse_names_metadata;

TEST_CASE("parse_names_metadata extracts values in key order") {
    const std::string meta = "{0: '0', 1: '1', 2: '2', 3: 'person'}";
    const auto names = parse_names_metadata(meta);
    REQUIRE(names == std::vector<std::string>{"0", "1", "2", "person"});
}

TEST_CASE("parse_names_metadata handles double quotes and blanks") {
    REQUIRE(parse_names_metadata("{0: \"a\", 1: \"b\"}")
            == std::vector<std::string>{"a", "b"});
    REQUIRE(parse_names_metadata("").empty());
}
