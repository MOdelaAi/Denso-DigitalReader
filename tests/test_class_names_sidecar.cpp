#include <catch2/catch_test_macros.hpp>

#include "ui/camera/shared/detection/class_names_sidecar.h"

#include <filesystem>
#include <fstream>

using denso::ui::read_names_sidecar;

TEST_CASE("sidecar reads a JSON array of class names", "[sidecar]") {
    const auto dir = std::filesystem::temp_directory_path();
    const auto engine = dir / "denso_sidecar_test.engine";
    std::ofstream(dir / "denso_sidecar_test.names.json") << R"(["0","1","2","3"])";

    auto names = read_names_sidecar(engine);
    REQUIRE(names.has_value());
    REQUIRE(names->size() == 4);
    CHECK((*names)[2] == "2");
}

TEST_CASE("sidecar returns nullopt when the file is absent", "[sidecar]") {
    CHECK_FALSE(read_names_sidecar("/no/such/denso_missing.engine").has_value());
}