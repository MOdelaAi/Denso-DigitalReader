#include <catch2/catch_test_macros.hpp>

#include "detection/engine_requirements.h"

#include <set>
#include <string>
#include <vector>

using namespace denso::ui;

TEST_CASE("no required models → nothing missing", "[engine_requirements]") {
    CHECK(missing_required_models({}, {}).empty());
    CHECK(missing_required_models({}, {"a.engine"}).empty());
}

TEST_CASE("all required models warmed → nothing missing", "[engine_requirements]") {
    const std::vector<std::string> required{"a.engine", "b.engine"};
    const std::set<std::string> warmed{"a.engine", "b.engine", "extra.engine"};
    CHECK(missing_required_models(required, warmed).empty());
}

TEST_CASE("a required model absent from warmed is reported", "[engine_requirements]") {
    const std::vector<std::string> required{"a.engine", "b.engine"};
    const std::set<std::string> warmed{"a.engine"};  // b never loaded
    const auto missing = missing_required_models(required, warmed);
    REQUIRE(missing.size() == 1);
    CHECK(missing[0] == "b.engine");
}

TEST_CASE("an empty warmed set reports every required model", "[engine_requirements]") {
    // The missing-models-dir / zero-engines case: nothing warmed, so every
    // configured model must abort warm-up.
    const std::vector<std::string> required{"a.engine", "b.engine"};
    const auto missing = missing_required_models(required, {});
    REQUIRE(missing.size() == 2);
    CHECK(missing[0] == "a.engine");
    CHECK(missing[1] == "b.engine");
}
