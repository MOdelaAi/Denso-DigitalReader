#include <catch2/catch_test_macros.hpp>

#include "brazing/config.h"
#include "brazing/url.h"
#include "db/db.h"

#include <QSqlQuery>
#include <QString>

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
    // save() is CHECKED now, so a caller can refuse to report success (and refuse
    // to reconfigure a running pipeline) when the write did not land.
    CHECK(denso::brazing::save(db->handle(), cfg));

    const denso::brazing::BrazingConfig got = denso::brazing::load(db->handle());
    CHECK(got.enabled);
    CHECK(got.base_url == "http://192.168.1.50:8098");
}

TEST_CASE("a save that fails half way changes nothing", "[brazing_config]") {
    // The two rows go in ONE transaction, so a false result truthfully means
    // nothing was persisted — which is what lets the Settings dialog refuse to
    // report success AND refuse to reconfigure the running pipeline.
    //
    // The dangerous partial state is specifically: the FLAG written, the ADDRESS
    // not. Reporting would then come back up against the PREVIOUS server on the
    // next boot. Forced deterministically with a trigger that aborts only the
    // base_url upsert, so the enabled upsert has already succeeded when the
    // failure lands.
    auto db = denso::db::Db::open_in_memory();
    REQUIRE(db);
    REQUIRE(denso::db::run_migrations(db->handle()));

    denso::brazing::BrazingConfig first;
    first.enabled = false;
    first.base_url = "http://192.168.1.112:8080";
    REQUIRE(denso::brazing::save(db->handle(), first));

    QSqlQuery q(db->handle());
    REQUIRE(q.exec(QStringLiteral(
        "CREATE TRIGGER block_url BEFORE INSERT ON settings "
        "WHEN NEW.key = 'brazing.base_url' "
        "BEGIN SELECT RAISE(ABORT, 'blocked'); END")));

    denso::brazing::BrazingConfig second;
    second.enabled = true;
    second.base_url = "http://192.168.1.113:9090";
    CHECK_FALSE(denso::brazing::save(db->handle(), second));

    REQUIRE(q.exec(QStringLiteral("DROP TRIGGER block_url")));

    // Rolled back whole: NEITHER field moved. Without the transaction, `enabled`
    // would now be 1 against the OLD address.
    const auto after = denso::brazing::load(db->handle());
    CHECK_FALSE(after.enabled);
    CHECK(after.base_url == "http://192.168.1.112:8080");
}

TEST_CASE("what is persisted is the canonical base URL", "[brazing_config]") {
    // The stored row is what every later reader — the grid gate, the transport,
    // the next Settings open — sees. It must be the canonical base, not whatever
    // the operator pasted, so the doubled endpoint cannot be reintroduced by a
    // path that skips the dialog.
    auto db = denso::db::Db::open_in_memory();
    REQUIRE(db);
    REQUIRE(denso::db::run_migrations(db->handle()));

    const auto url = denso::brazing::normalize_base_url(
        "  http://192.168.1.112:8080/api/brazing/update/  ");
    REQUIRE(url.ok);

    denso::brazing::BrazingConfig cfg;
    cfg.enabled = true;
    cfg.base_url = url.base_url;
    REQUIRE(denso::brazing::save(db->handle(), cfg));

    const denso::brazing::BrazingConfig got = denso::brazing::load(db->handle());
    CHECK(got.base_url == "http://192.168.1.112:8080");
    // …and the round trip composes the endpoint exactly once.
    CHECK(denso::brazing::endpoint_url(got.base_url) ==
          "http://192.168.1.112:8080/api/brazing/update");
}
