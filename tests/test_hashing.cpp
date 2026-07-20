#include <catch2/catch_test_macros.hpp>
#include "models/hashing.h"
#include <QTemporaryFile>

TEST_CASE("file_sha256 hashes known content", "[hashing]") {
    QTemporaryFile f; REQUIRE(f.open());
    f.write("abc"); f.flush();
    auto h = denso::models::file_sha256(f.fileName());
    REQUIRE(h.has_value());
    REQUIRE(*h == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}
TEST_CASE("file_sha256 returns nullopt for a missing file", "[hashing]") {
    REQUIRE_FALSE(denso::models::file_sha256("/no/such/file/xyz").has_value());
}
