#include <catch2/catch_test_macros.hpp>

#include "db/db.h"
#include "reading/reading.h"
#include "reading/repo.h"

#include <optional>
#include <utility>

using denso::db::Db;
using denso::db::run_migrations;
using denso::reading::insert;
using denso::reading::query;
using denso::reading::Reading;

namespace {

Db db() {
    auto d = Db::open_in_memory();
    REQUIRE(d.has_value());
    REQUIRE(run_migrations(d->handle()));
    return std::move(*d);
}

Reading make(int64_t cam, int64_t ts, const char* value, float conf) {
    Reading r;
    r.camera_id = cam;
    r.ts_ms = ts;
    r.value = value;
    r.conf = conf;
    return r;
}

} // namespace

TEST_CASE("reading insert returns an id and round-trips via query") {
    Db d = db();
    const auto id = insert(d.handle(), make(1, 1000, "0042", 0.9f));
    REQUIRE(id.has_value());
    REQUIRE(*id > 0);

    const std::vector<Reading> rows = query(d.handle(), 1, 0, 2000);
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].id == *id);
    CHECK(rows[0].camera_id == 1);
    CHECK(rows[0].ts_ms == 1000);
    CHECK(rows[0].value == "0042");
    CHECK(rows[0].conf == 0.9f);
}

TEST_CASE("reading query filters by camera and time range, ordered by ts_ms") {
    Db d = db();
    REQUIRE(insert(d.handle(), make(1, 3000, "c", 0.5f)).has_value());
    REQUIRE(insert(d.handle(), make(1, 1000, "a", 0.5f)).has_value());
    REQUIRE(insert(d.handle(), make(1, 2000, "b", 0.5f)).has_value());
    REQUIRE(insert(d.handle(), make(2, 1500, "other", 0.5f)).has_value());

    SECTION("only the requested camera, ascending ts_ms") {
        const std::vector<Reading> rows = query(d.handle(), 1, 0, 10000);
        REQUIRE(rows.size() == 3);
        CHECK(rows[0].value == "a");
        CHECK(rows[1].value == "b");
        CHECK(rows[2].value == "c");
    }

    SECTION("time range is inclusive and excludes out-of-range rows") {
        const std::vector<Reading> rows = query(d.handle(), 1, 1000, 2000);
        REQUIRE(rows.size() == 2);
        CHECK(rows[0].value == "a");
        CHECK(rows[1].value == "b");
    }

    SECTION("no matches yields an empty vector") {
        CHECK(query(d.handle(), 1, 5000, 6000).empty());
        CHECK(query(d.handle(), 99, 0, 10000).empty());
    }
}
