#include <catch2/catch_test_macros.hpp>
#include "models/class_map.h"

TEST_CASE("resolve_class_map identical names id->id", "[class_map]") {
    auto r = denso::models::resolve_class_map({"a", "b", "c"}, {"a", "b", "c"}, {});
    REQUIRE(r.map.has_value());
    REQUIRE((*r.map) == (std::map<int, int>{{0, 0}, {1, 1}, {2, 2}}));
}

TEST_CASE("resolve_class_map reorder by name", "[class_map]") {
    auto r = denso::models::resolve_class_map({"a", "b"}, {"b", "a"}, {});
    REQUIRE(r.map.has_value());
    REQUIRE((*r.map) == (std::map<int, int>{{0, 1}, {1, 0}}));
}

TEST_CASE("resolve_class_map rejects duplicate new name", "[class_map]") {
    auto r = denso::models::resolve_class_map({"a"}, {"a", "a"}, {});
    REQUIRE_FALSE(r.map.has_value());
}

TEST_CASE("resolve_class_map rejects duplicate old name", "[class_map]") {
    auto r = denso::models::resolve_class_map({"a", "a"}, {"a"}, {});
    REQUIRE_FALSE(r.map.has_value());
}

TEST_CASE("resolve_class_map explicit remap redirects", "[class_map]") {
    auto r = denso::models::resolve_class_map({"a"}, {"x"}, {{"a", "x"}});
    REQUIRE(r.map.has_value());
    REQUIRE((*r.map) == (std::map<int, int>{{0, 0}}));
}

TEST_CASE("resolve_class_map rejects explicit key absent from old", "[class_map]") {
    auto r = denso::models::resolve_class_map({"a"}, {"x"}, {{"z", "x"}});
    REQUIRE_FALSE(r.map.has_value());
}

TEST_CASE("resolve_class_map rejects explicit target absent from new", "[class_map]") {
    auto r = denso::models::resolve_class_map({"a"}, {"x"}, {{"a", "z"}});
    REQUIRE_FALSE(r.map.has_value());
}

TEST_CASE("resolve_class_map rejects many-to-one", "[class_map]") {
    auto r = denso::models::resolve_class_map({"a", "b"}, {"x"}, {{"a", "x"}, {"b", "x"}});
    REQUIRE_FALSE(r.map.has_value());
}

TEST_CASE("resolve_class_map omits an old id absent from new", "[class_map]") {
    auto r = denso::models::resolve_class_map({"a", "gone"}, {"a"}, {});
    REQUIRE(r.map.has_value());
    REQUIRE((*r.map) == (std::map<int, int>{{0, 0}}));
}
