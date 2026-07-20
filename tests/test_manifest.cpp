#include <catch2/catch_test_macros.hpp>
#include "models/manifest.h"
static const char* kSha =
  "0000000000000000000000000000000000000000000000000000000000000000";
static std::string one_gen_json() {
    return std::string(R"({"schema":1,"generations":[{)") +
      R"("name":"digit-v3.1","engine":"digit-v3.1.engine","engine_sha256":")" + kSha + R"(",)" +
      R"("sidecar":"digit-v3.1.names.json","sidecar_sha256":")" + kSha + R"(",)" +
      R"("class_names":["0","1"],"built_for":{"trt":"10.3","cuda":"12.6","sm":"87"},)" +
      R"("installed_utc":"2026-07-20T00:00:00Z","state":"installed"}]})";
}
TEST_CASE("parse_manifest reads a valid generation", "[manifest]") {
    auto r = denso::models::parse_manifest(one_gen_json());
    REQUIRE(r.error.empty());
    REQUIRE(r.manifest.has_value());
    REQUIRE(r.manifest->generations.size() == 1);
    REQUIRE(r.manifest->generations[0].engine == "digit-v3.1.engine");
    REQUIRE(r.manifest->generations[0].sm == "87");
}
TEST_CASE("parse_manifest rejects non-JSON", "[manifest]") {
    REQUIRE_FALSE(denso::models::parse_manifest("not json").manifest.has_value());
}
TEST_CASE("parse_manifest rejects a generation missing engine", "[manifest]") {
    REQUIRE_FALSE(denso::models::parse_manifest(
        R"({"schema":1,"generations":[{"name":"x"}]})").manifest.has_value());
}
TEST_CASE("validate_manifest accepts a clean manifest", "[manifest]") {
    auto r = denso::models::parse_manifest(one_gen_json());
    REQUIRE(r.manifest.has_value());
    REQUIRE_FALSE(denso::models::validate_manifest(*r.manifest).has_value());
}
TEST_CASE("validate_manifest rejects an unsafe engine basename", "[manifest]") {
    auto r = denso::models::parse_manifest(one_gen_json());
    REQUIRE(r.manifest.has_value());
    r.manifest->generations[0].engine = "../evil.engine";
    REQUIRE(denso::models::validate_manifest(*r.manifest).has_value());
}
TEST_CASE("validate_manifest rejects mismatched engine/sidecar stems", "[manifest]") {
    auto r = denso::models::parse_manifest(one_gen_json());
    REQUIRE(r.manifest.has_value());
    r.manifest->generations[0].sidecar = "other-stem.names.json";
    REQUIRE(denso::models::validate_manifest(*r.manifest).has_value());
}
TEST_CASE("validate_manifest rejects duplicate engine filenames", "[manifest]") {
    auto r = denso::models::parse_manifest(one_gen_json());
    REQUIRE(r.manifest.has_value());
    auto g2 = r.manifest->generations[0];
    g2.name = "digit-v3.2";
    r.manifest->generations.push_back(g2);
    REQUIRE(denso::models::validate_manifest(*r.manifest).has_value());
}
TEST_CASE("validate_manifest rejects a short sha256", "[manifest]") {
    auto r = denso::models::parse_manifest(one_gen_json());
    REQUIRE(r.manifest.has_value());
    r.manifest->generations[0].engine_sha256 = "aa";
    REQUIRE(denso::models::validate_manifest(*r.manifest).has_value());
}
TEST_CASE("validate_manifest rejects an empty class name", "[manifest]") {
    auto r = denso::models::parse_manifest(one_gen_json());
    REQUIRE(r.manifest.has_value());
    r.manifest->generations[0].class_names = {"0", ""};
    REQUIRE(denso::models::validate_manifest(*r.manifest).has_value());
}
TEST_CASE("validate_manifest rejects a duplicate class name", "[manifest]") {
    auto r = denso::models::parse_manifest(one_gen_json());
    REQUIRE(r.manifest.has_value());
    r.manifest->generations[0].class_names = {"0", "0"};
    REQUIRE(denso::models::validate_manifest(*r.manifest).has_value());
}
TEST_CASE("find_by_engine finds an existing generation", "[manifest]") {
    auto r = denso::models::parse_manifest(one_gen_json());
    REQUIRE(r.manifest.has_value());
    const auto* g = denso::models::find_by_engine(*r.manifest, "digit-v3.1.engine");
    REQUIRE(g != nullptr);
    REQUIRE(g->name == "digit-v3.1");
}
TEST_CASE("find_by_engine returns nullptr on a miss", "[manifest]") {
    auto r = denso::models::parse_manifest(one_gen_json());
    REQUIRE(r.manifest.has_value());
    REQUIRE(denso::models::find_by_engine(*r.manifest, "nope.engine") == nullptr);
}
