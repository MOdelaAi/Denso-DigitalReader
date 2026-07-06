#include <catch2/catch_test_macros.hpp>

#include "brazing/config.h"
#include "db/db.h"

TEST_CASE("brazing config round-trips through the settings table", "[brazing_config]") {
    auto db = denso::db::Db::open_in_memory();
    REQUIRE(db);
    REQUIRE(denso::db::run_migrations(db->handle()));

    // Defaults when nothing is stored.
    const denso::brazing::BrazingConfig def = denso::brazing::load(db->handle());
    CHECK_FALSE(def.enabled);
    CHECK(def.base_url.empty());

    denso::brazing::BrazingConfig cfg;
    cfg.enabled = true;
    cfg.base_url = "http://192.168.1.50:8098";
    denso::brazing::save(db->handle(), cfg);

    const denso::brazing::BrazingConfig got = denso::brazing::load(db->handle());
    CHECK(got.enabled);
    CHECK(got.base_url == "http://192.168.1.50:8098");
}
