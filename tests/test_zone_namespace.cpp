// The 1..99 zone-number namespace: its range authority, its write chokepoints,
// its payload keys, and the v16 -> v17 migration that retired the zone-0
// sentinel.
//
// The single fact every case here defends: there is exactly ONE representation
// of "this area is not reported" — a disengaged optional, SQL NULL on disk —
// and 0 is not it. 0 is not a zone at all: it is out of range, refused by the
// validators and by both write chokepoints exactly as 100 is.
//
// Before v17, `camera_area.zone = 0` was a SECOND spelling of unassigned. That
// duplication is what forced readers all over the tree to carry a `*a.zone != 0`
// test. v17 normalises those legacy rows to NULL once, at migration time, which
// is what allows the runtime to drop the special case entirely. Both halves are
// worth a regression guard: leaving a legacy 0 in place would silently START a
// deliberately-excluded area reporting under "zone0", and re-admitting 0 as a
// zone would put a key on the wire that no current configuration can produce.
#include "brazing/brazing_payload.h"
#include "camera/area_validation.h"
#include "camera/camera.h"
#include "camera/repo.h"
#include "db/db.h"

#include <catch2/catch_test_macros.hpp>

#include <QDir>
#include <QSqlError>
#include <QSqlQuery>
#include <QString>
#include <QTemporaryDir>
#include <QVariant>

#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

using denso::camera::CameraArea;
using denso::camera::kMaxZone;
using denso::camera::kMinZone;

namespace {

denso::camera::Camera usb_camera(const std::string& name) {
    denso::camera::Camera c;
    c.name = name;
    c.camera_type = "usb";
    c.index = 0;
    c.width = 1280;
    c.height = 720;
    c.fps = 30;
    c.setup_complete = true;
    return c;
}

CameraArea area_at(const std::string& name, std::optional<int> zone, float ox) {
    CameraArea a;
    a.name = name;
    a.zone = zone;
    a.points = {{ox, 0.1f}, {ox + 0.2f, 0.1f}, {ox + 0.1f, 0.4f}};
    return a;
}

/// A scratch database migrated to the current schema.
struct Scratch {
    QTemporaryDir dir;
    QString path;
    Scratch() : path(QDir(dir.path()).filePath(QStringLiteral("denso.db"))) {}
};

int row_count(const QSqlDatabase& db, const char* table) {
    QSqlQuery q(db);
    if (!q.exec(QStringLiteral("SELECT COUNT(*) FROM %1").arg(QLatin1String(table))) ||
        !q.next()) {
        return -1;
    }
    return q.value(0).toInt();
}

/// The stored `zone` for one area name: nullopt when the column is SQL NULL.
/// Read straight out of SQLite rather than through the repo, because the point
/// of most of these cases is WHICH of NULL and 0 is on disk.
std::optional<int> stored_zone(const QSqlDatabase& db, const char* area_name) {
    QSqlQuery q(db);
    q.prepare(QStringLiteral("SELECT zone FROM camera_area WHERE name = ?"));
    q.addBindValue(QString::fromLatin1(area_name));
    REQUIRE(q.exec());
    REQUIRE(q.next());
    const QVariant v = q.value(0);
    return v.isNull() ? std::nullopt : std::optional<int>{v.toInt()};
}

/// The CREATE statement SQLite is holding for a table, verbatim.
QString table_ddl(const QSqlDatabase& db, const char* table) {
    QSqlQuery q(db);
    q.prepare(
        QStringLiteral("SELECT sql FROM sqlite_master WHERE type='table' AND name=?"));
    q.addBindValue(QString::fromLatin1(table));
    REQUIRE(q.exec());
    REQUIRE(q.next());
    return q.value(0).toString();
}

/// Try to persist a Ball zone straight through SQL, bypassing the repo, so the
/// answer is the DATABASE's alone. True when the row landed.
bool ball_row_accepted(const QSqlDatabase& db, int64_t camera_id, int zone_no) {
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "INSERT INTO ball_level_zone (camera_id, zone_no, conf, rect_x, rect_y, "
        "rect_w, rect_h, y_100, y_0, hold_ms) "
        "VALUES (?, ?, 0.5, 0.1, 0.1, 0.5, 0.5, 0.2, 0.8, 500)"));
    q.addBindValue(static_cast<qlonglong>(camera_id));
    q.addBindValue(zone_no);
    return q.exec();
}

}  // namespace

// ─── 1. The range authority ──────────────────────────────────────────────────

TEST_CASE("the zone namespace is the closed range 1..99", "[zone_namespace]") {
    CHECK(kMinZone == 1);
    CHECK(kMaxZone == 99);

    CHECK(denso::camera::zone_in_range(1));
    CHECK(denso::camera::zone_in_range(12));   // the old ceiling, still legal
    CHECK(denso::camera::zone_in_range(45));
    CHECK(denso::camera::zone_in_range(99));

    // 0 is refused by the SAME predicate that refuses 100 — it is not a special
    // case, an escape or a sentinel. It is simply not in the range.
    CHECK_FALSE(denso::camera::zone_in_range(0));
    CHECK_FALSE(denso::camera::zone_in_range(-1));
    CHECK_FALSE(denso::camera::zone_in_range(100));
    CHECK_FALSE(denso::camera::zone_in_range(1000));
}

TEST_CASE("find_zone_out_of_range names the first offending area",
          "[zone_namespace][area_validation]") {
    CHECK_FALSE(denso::camera::find_zone_out_of_range(
        {area_at("a", 1, 0.1f), area_at("b", 99, 0.4f),
         area_at("c", std::nullopt, 0.7f)}));

    // Zone 0 is reported as out of range, NOT quietly accepted as "unassigned".
    // An area carrying 0 came from somewhere unmigrated and the operator has to
    // be told, not have the value silently reinterpreted.
    const auto zero = denso::camera::find_zone_out_of_range(
        {area_at("ok", 5, 0.1f), area_at("legacy", 0, 0.4f)});
    REQUIRE(zero.has_value());
    CHECK(zero->zone == 0);
    CHECK(zero->area_name == "legacy");

    const auto low = denso::camera::find_zone_out_of_range(
        {area_at("ok", 5, 0.1f), area_at("bad", -1, 0.4f)});
    REQUIRE(low.has_value());
    CHECK(low->zone == -1);
    CHECK(low->area_name == "bad");

    const auto high =
        denso::camera::find_zone_out_of_range({area_at("bad", 100, 0.1f)});
    REQUIRE(high.has_value());
    CHECK(high->zone == 100);

    // An UNASSIGNED area has no number to be out of range — the one case that
    // must never be confused with a zero.
    CHECK_FALSE(
        denso::camera::find_zone_out_of_range({area_at("roi", std::nullopt, 0.1f)}));
}

TEST_CASE("find_zone_conflict is driven by assignment, never by a zero",
          "[zone_namespace][area_validation]") {
    // Duplicate detection is unchanged for real zones.
    const auto same = denso::camera::find_zone_conflict(
        {area_at("a", 7, 0.1f), area_at("b", 7, 0.4f)}, {});
    REQUIRE(same.has_value());
    CHECK(same->zone == 7);
    CHECK(same->area_name == "b");
    CHECK(same->owner.empty());

    // A zone held by another camera blocks it here, and names the holder.
    const std::map<int, std::string> elsewhere{{7, "Line 2 Cam"}};
    const auto other =
        denso::camera::find_zone_conflict({area_at("a", 7, 0.1f)}, elsewhere);
    REQUIRE(other.has_value());
    CHECK(other->zone == 7);
    CHECK(other->owner == "Line 2 Cam");

    // Unassigned areas never conflict, however many there are. This is the ONLY
    // exemption: it keys off a disengaged optional, not off any number.
    CHECK_FALSE(denso::camera::find_zone_conflict(
        {area_at("a", std::nullopt, 0.1f), area_at("b", std::nullopt, 0.4f)}, {}));

    // Two areas both carrying 0 are NOT exempted the way two unassigned ones
    // are. Under the retired sentinel rule this set was legal; it is now a pair
    // of out-of-range values that the range check refuses first, and the
    // conflict finder must not be the thing that waves them through.
    CHECK(denso::camera::find_zone_out_of_range(
              {area_at("a", 0, 0.1f), area_at("b", 0, 0.4f)})
              .has_value());
    CHECK(denso::camera::find_zone_conflict(
              {area_at("a", 0, 0.1f), area_at("b", 0, 0.4f)}, {})
              .has_value());
}

// ─── 2. The digit write chokepoint enforces the range authoritatively ────────

TEST_CASE("replace_areas enforces 1..99 regardless of what the UI allowed",
          "[zone_namespace][camera_repo]") {
    auto db = denso::db::Db::open_in_memory();
    REQUIRE(db);
    REQUIRE(denso::db::run_migrations(db->handle()));
    const auto cam = denso::camera::insert(db->handle(), usb_camera("Line 1"));
    REQUIRE(cam.has_value());

    // The column carries no CHECK, so without the repo test these would persist.
    CHECK_FALSE(
        denso::camera::replace_areas(db->handle(), *cam, {area_at("zero", 0, 0.1f)}));
    CHECK_FALSE(
        denso::camera::replace_areas(db->handle(), *cam, {area_at("neg", -1, 0.1f)}));
    CHECK_FALSE(
        denso::camera::replace_areas(db->handle(), *cam, {area_at("big", 100, 0.1f)}));
    CHECK_FALSE(
        denso::camera::replace_areas(db->handle(), *cam, {area_at("huge", 4242, 0.1f)}));
    // A refused save is transactional: nothing landed.
    CHECK(row_count(db->handle(), "camera_area") == 0);

    // Both ends of the range are accepted and round-trip as themselves.
    REQUIRE(denso::camera::replace_areas(
        db->handle(), *cam,
        {area_at("floor", kMinZone, 0.1f), area_at("ceil", kMaxZone, 0.4f)}));
    const auto back = denso::camera::areas_for(db->handle(), *cam);
    REQUIRE(back.size() == 2);
    REQUIRE(back[0].zone.has_value());
    REQUIRE(back[1].zone.has_value());
    CHECK(*back[0].zone == 1);
    CHECK(*back[1].zone == 99);
}

TEST_CASE("unassigned persists as NULL, and is the only unassigned form",
          "[zone_namespace][camera_repo]") {
    auto db = denso::db::Db::open_in_memory();
    REQUIRE(db);
    REQUIRE(denso::db::run_migrations(db->handle()));
    const auto cam = denso::camera::insert(db->handle(), usb_camera("Line 1"));
    REQUIRE(cam.has_value());

    REQUIRE(denso::camera::replace_areas(
        db->handle(), *cam,
        {area_at("reported", 1, 0.1f), area_at("detect-only", std::nullopt, 0.4f)}));

    const auto one = stored_zone(db->handle(), "reported");
    REQUIRE(one.has_value());
    CHECK(*one == 1);
    CHECK_FALSE(stored_zone(db->handle(), "detect-only").has_value());

    // Any number of unassigned areas coexist — they claim nothing.
    CHECK(denso::camera::replace_areas(
        db->handle(), *cam,
        {area_at("u1", std::nullopt, 0.1f), area_at("u2", std::nullopt, 0.4f),
         area_at("u3", std::nullopt, 0.7f)}));

    // And no route through the chokepoint can put a 0 on disk, so the column can
    // never again hold the ambiguous value the migration just cleared out.
    CHECK_FALSE(
        denso::camera::replace_areas(db->handle(), *cam, {area_at("z", 0, 0.1f)}));
}

TEST_CASE("cross-camera ownership is unchanged for real zones",
          "[zone_namespace][camera_repo]") {
    auto db = denso::db::Db::open_in_memory();
    REQUIRE(db);
    REQUIRE(denso::db::run_migrations(db->handle()));
    const auto a = denso::camera::insert(db->handle(), usb_camera("Line 1"));
    const auto b = denso::camera::insert(db->handle(), usb_camera("Line 2"));
    REQUIRE(a.has_value());
    REQUIRE(b.has_value());

    REQUIRE(denso::camera::replace_areas(db->handle(), *a, {area_at("a1", 1, 0.1f)}));

    const auto owned = denso::camera::zones_owned_by_other_cameras(db->handle(), *b);
    REQUIRE(owned.count(1) == 1);
    CHECK(owned.at(1) == "Line 1");

    // ...and camera B cannot take it. The payload keys by zone number alone, so
    // both cameras posting "zone1" would merge two readings onto one key.
    CHECK_FALSE(denso::camera::replace_areas(db->handle(), *b, {area_at("b1", 1, 0.4f)}));

    // The owner may still re-save its own zone — its rows are replaced wholesale.
    CHECK(denso::camera::replace_areas(db->handle(), *a, {area_at("a1", 1, 0.1f)}));

    // An unassigned area on A claims nothing, so B sees an empty ownership map.
    REQUIRE(denso::camera::replace_areas(db->handle(), *a,
                                         {area_at("roi", std::nullopt, 0.1f)}));
    CHECK(denso::camera::zones_owned_by_other_cameras(db->handle(), *b).empty());
}

// ─── 3. The payload ──────────────────────────────────────────────────────────

TEST_CASE("payload keys span the whole namespace and stay sparse",
          "[zone_namespace][payload]") {
    using denso::ui::ZoneValue;
    const auto whole = [](int v) { return ZoneValue{v, 0, 4}; };

    CHECK(denso::ui::build_brazing_payload({{1, whole(12)}}) == "{\"zone1\":12}");
    CHECK(denso::ui::build_brazing_payload({{45, whole(7)}}) == "{\"zone45\":7}");
    CHECK(denso::ui::build_brazing_payload({{99, whole(1234)}}) == "{\"zone99\":1234}");

    // Sparse and ascending, unchanged: only the zones present appear, in order.
    CHECK(denso::ui::build_brazing_payload(
              {{1, whole(1)}, {45, whole(2)}, {99, whole(3)}}) ==
          "{\"zone1\":1,\"zone45\":2,\"zone99\":3}");

    // No zone at all is still an empty object, not a body full of nulls.
    CHECK(denso::ui::build_brazing_payload({}) == "{}");
}

TEST_CASE("no valid configuration can emit a zone0 key",
          "[zone_namespace][payload][camera_repo]") {
    // The payload builder is a pure function of the aggregated zone map, so the
    // guarantee is not "the builder refuses 0" — it is that no CONFIGURATION the
    // chokepoint accepts can put a 0 into that map. Built end to end rather than
    // asserted on a hand-made map, because a hand-made map proves nothing about
    // what the machine can actually reach.
    auto db = denso::db::Db::open_in_memory();
    REQUIRE(db);
    REQUIRE(denso::db::run_migrations(db->handle()));
    const auto cam = denso::camera::insert(db->handle(), usb_camera("Line 1"));
    REQUIRE(cam.has_value());

    // Everything an operator could try to configure, including a 0 and an
    // unassigned ROI. Saved one at a time: the 0 must be REFUSED, not dropped.
    CHECK_FALSE(
        denso::camera::replace_areas(db->handle(), *cam, {area_at("z", 0, 0.1f)}));
    REQUIRE(denso::camera::replace_areas(
        db->handle(), *cam,
        {area_at("first", 1, 0.1f), area_at("mid", 45, 0.4f),
         area_at("last", 99, 0.7f), area_at("roi", std::nullopt, 0.1f)}));

    using denso::ui::ZoneValue;
    std::map<int, ZoneValue> aggregated;
    int n = 1;
    for (const CameraArea& a : denso::camera::areas_for(db->handle(), *cam)) {
        if (!a.zone) continue;   // ROI-only contributes no key, and no zero
        REQUIRE(denso::camera::zone_in_range(*a.zone));
        aggregated[*a.zone] = ZoneValue{n++, 0, 4};
    }
    REQUIRE(aggregated.size() == 3u);
    CHECK(aggregated.count(0) == 0);

    const std::string body = denso::ui::build_brazing_payload(aggregated);
    CHECK(body.find("\"zone0\"") == std::string::npos);
    CHECK(body == "{\"zone1\":1,\"zone45\":2,\"zone99\":3}");
}

// ─── 4. v16 -> v17: the sentinel retirement ──────────────────────────────────

TEST_CASE("v16 -> v17 turns legacy zone 0 into NULL and leaves real zones alone",
          "[zone_namespace][schema][migration]") {
    Scratch s;
    int64_t cam_a = 0;
    int64_t cam_b = 0;
    {
        auto db = denso::db::Db::open(s.path);
        REQUIRE(db.has_value());
        REQUIRE(denso::db::run_migrations(db->handle()));

        const auto a = denso::camera::insert(db->handle(), usb_camera("Line 1"));
        const auto b = denso::camera::insert(db->handle(), usb_camera("Line 2"));
        REQUIRE(a.has_value());
        REQUIRE(b.has_value());
        cam_a = *a;
        cam_b = *b;

        // Written as SQL so the fixture can express the PRE-v17 world, which the
        // repo can no longer produce: legacy zone 0 meaning "not reported",
        // alongside a genuine NULL meaning the same thing, plus real zones at
        // both ends of the old 1..12 range and a per-zone decimal format.
        const auto ins = [&](int64_t cam, const char* name, const char* zone, int dp) {
            QSqlQuery q(db->handle());
            q.prepare(QStringLiteral(
                          "INSERT INTO camera_area (camera_id, name, points, zone, "
                          "decimal_places) "
                          "VALUES (?, ?, '0.1,0.1;0.9,0.1;0.9,0.9', %1, ?)")
                          .arg(QLatin1String(zone)));
            q.addBindValue(static_cast<qlonglong>(cam));
            q.addBindValue(QString::fromLatin1(name));
            q.addBindValue(dp);
            REQUIRE(q.exec());
        };
        ins(cam_a, "legacy-zero", "0", 0);        // the sentinel
        ins(cam_a, "already-null", "NULL", 0);    // the other spelling of the same
        ins(cam_a, "real-one", "1", 1);           // a real zone, low end
        ins(cam_b, "real-twelve", "12", 3);       // a real zone, the old ceiling
        ins(cam_b, "second-zero", "0", 2);        // a SECOND sentinel row

        // A Ball zone and its calibration, so the migration can be shown to
        // leave hand-measured operator values alone.
        QSqlQuery z(db->handle());
        z.prepare(QStringLiteral(
            "INSERT INTO ball_level_zone (camera_id, zone_no, conf, rect_x, rect_y, "
            "rect_w, rect_h, y_100, y_0, hold_ms) "
            "VALUES (?, 7, 0.42, 0.11, 0.22, 0.33, 0.44, 0.25, 0.75, 1234)"));
        z.addBindValue(static_cast<qlonglong>(cam_b));
        REQUIRE(z.exec());

        // The v15 CHECK is in force BEFORE the migration...
        CHECK_FALSE(ball_row_accepted(db->handle(), cam_a, 0));
        CHECK(table_ddl(db->handle(), "ball_level_zone")
                  .contains(QStringLiteral("CHECK (zone_no >= 1)")));

        REQUIRE(QSqlQuery(db->handle()).exec(QStringLiteral("PRAGMA user_version = 16")));
    }

    {
        auto db = denso::db::Db::open(s.path);
        REQUIRE(db.has_value());
        REQUIRE(denso::db::read_user_version(db->handle()).value_or(-1) == 16);
        REQUIRE(denso::db::run_migrations(db->handle()));
        CHECK(denso::db::read_user_version(db->handle()).value_or(-1) == 17);

        // ── the semantic migration ──
        CHECK_FALSE(stored_zone(db->handle(), "legacy-zero").has_value());
        CHECK_FALSE(stored_zone(db->handle(), "second-zero").has_value());
        CHECK_FALSE(stored_zone(db->handle(), "already-null").has_value());
        // Real zones are untouched. If these moved, an operator would find their
        // machine reporting under different keys after an upgrade.
        REQUIRE(stored_zone(db->handle(), "real-one").has_value());
        CHECK(*stored_zone(db->handle(), "real-one") == 1);
        REQUIRE(stored_zone(db->handle(), "real-twelve").has_value());
        CHECK(*stored_zone(db->handle(), "real-twelve") == 12);

        // ── nothing was dropped ──
        CHECK(row_count(db->handle(), "camera") == 2);
        CHECK(row_count(db->handle(), "camera_area") == 5);
        CHECK(row_count(db->handle(), "ball_level_zone") == 1);

        // ── the per-area number format survived the migration ──
        QSqlQuery dp(db->handle());
        REQUIRE(dp.exec(QStringLiteral(
            "SELECT name, decimal_places FROM camera_area ORDER BY name")));
        std::map<QString, int> formats;
        while (dp.next()) formats[dp.value(0).toString()] = dp.value(1).toInt();
        CHECK(formats[QStringLiteral("real-one")] == 1);
        CHECK(formats[QStringLiteral("real-twelve")] == 3);
        CHECK(formats[QStringLiteral("second-zero")] == 2);

        // ── ball_level_zone was NOT rebuilt, and did not need to be ──────────
        // Its v15 `CHECK (zone_no >= 1)` IS the 1..99 floor, so there is nothing
        // for v17 to widen; and because that CHECK has been in force since v15,
        // a zero row could never have been persisted, so there is no ball-side
        // data to normalise either. The DDL is asserted verbatim: a rebuild that
        // "happened to" produce an equivalent table would still be work done on
        // hand-measured calibration for no reason.
        const QString ddl = table_ddl(db->handle(), "ball_level_zone");
        CHECK(ddl.contains(QStringLiteral("CHECK (zone_no >= 1)")));
        CHECK_FALSE(ddl.contains(QStringLiteral("zone_no >= 0")));
        CHECK_FALSE(ddl.contains(QStringLiteral("_v17")));

        // The constraint still bites after the migration, from the DB itself.
        CHECK_FALSE(ball_row_accepted(db->handle(), cam_a, 0));
        CHECK_FALSE(ball_row_accepted(db->handle(), cam_a, -1));
        CHECK(ball_row_accepted(db->handle(), cam_a, 1));

        // ── no rebuild scaffolding was ever created ──
        QSqlQuery t(db->handle());
        REQUIRE(t.exec(QStringLiteral(
            "SELECT COUNT(*) FROM sqlite_master WHERE name LIKE '%_v17'")));
        REQUIRE(t.next());
        CHECK(t.value(0).toInt() == 0);

        // ── the untouched ball calibration is still exactly as measured ──
        QSqlQuery b(db->handle());
        REQUIRE(b.exec(QStringLiteral(
            "SELECT camera_id, zone_no, conf, rect_x, rect_y, rect_w, rect_h, "
            "y_100, y_0, hold_ms FROM ball_level_zone WHERE zone_no = 7")));
        REQUIRE(b.next());
        CHECK(b.value(0).toLongLong() == cam_b);
        CHECK(b.value(1).toInt() == 7);
        CHECK(b.value(2).toDouble() == 0.42);
        CHECK(b.value(3).toDouble() == 0.11);
        CHECK(b.value(4).toDouble() == 0.22);
        CHECK(b.value(5).toDouble() == 0.33);
        CHECK(b.value(6).toDouble() == 0.44);
        CHECK(b.value(7).toDouble() == 0.25);
        CHECK(b.value(8).toDouble() == 0.75);
        CHECK(b.value(9).toInt() == 1234);

        // ── the database is structurally sound ──
        QSqlQuery ic(db->handle());
        REQUIRE(ic.exec(QStringLiteral("PRAGMA integrity_check")));
        REQUIRE(ic.next());
        CHECK(ic.value(0).toString() == QStringLiteral("ok"));
        QSqlQuery fk(db->handle());
        REQUIRE(fk.exec(QStringLiteral("PRAGMA foreign_key_check")));
        CHECK_FALSE(fk.next());   // no row == no violation
    }

    // ── reopening must not migrate again ──
    {
        auto db = denso::db::Db::open(s.path);
        REQUIRE(db.has_value());
        REQUIRE(denso::db::read_user_version(db->handle()).value_or(-1) == 17);
        REQUIRE(denso::db::run_migrations(db->handle()));
        CHECK(denso::db::read_user_version(db->handle()).value_or(-1) == 17);
        CHECK(row_count(db->handle(), "camera_area") == 5);
        CHECK(row_count(db->handle(), "ball_level_zone") == 2);
    }

    // ── the migration is idempotent by construction ──────────────────────────
    // Re-running the UPDATE matches no rows, because after v17 nothing on either
    // side of the schema can hold a zero. This is what lets the v17 block stand
    // WITHOUT a transaction of its own: an interrupted migration re-runs
    // harmlessly, exactly like every other block in run_migrations.
    {
        auto db = denso::db::Db::open(s.path);
        REQUIRE(db.has_value());
        REQUIRE(denso::db::run_migrations(db->handle()));

        QSqlQuery zeros(db->handle());
        REQUIRE(zeros.exec(
            QStringLiteral("SELECT COUNT(*) FROM camera_area WHERE zone = 0")));
        REQUIRE(zeros.next());
        CHECK(zeros.value(0).toInt() == 0);

        // A new Zone 0 cannot be created after v17 either, so the migration has
        // nothing left to find on any subsequent run.
        CHECK_FALSE(denso::camera::replace_areas(
            db->handle(), cam_a, {area_at("brand-new-zero", 0, 0.1f)}));
        // The real zones are still reachable and still owned.
        const auto c = denso::camera::insert(db->handle(), usb_camera("Line 3"));
        REQUIRE(c.has_value());
        const auto owned = denso::camera::zones_owned_by_other_cameras(db->handle(), *c);
        CHECK(owned.count(0) == 0);
        CHECK(owned.count(1) == 1);
        CHECK(owned.count(12) == 1);
    }
}

// ─── 5. Automatic allocation ─────────────────────────────────────────────────
//
// One allocator, shared by both modes. Under 1..99 there is no longer a
// preference order distinct from validity — the lowest legal zone is also the
// one a fresh machine should start at — so the two facts are one constant and
// the allocator is a plain ascending scan.
TEST_CASE("automatic allocation walks 1..99 and then reports exhaustion",
          "[zone_namespace]") {
    using denso::camera::next_free_zone;

    SECTION("nothing taken: the first offer is 1") {
        REQUIRE(next_free_zone({}) == 1);
        REQUIRE(next_free_zone({}) == kMinZone);
    }

    SECTION("the lowest free number is offered") {
        REQUIRE(next_free_zone({1}) == 2);
        REQUIRE(next_free_zone({1, 2, 3}) == 4);
        REQUIRE(next_free_zone({2, 3, 4}) == 1);   // gaps are reused
    }

    SECTION("allocation proceeds all the way through 99") {
        std::set<int> all_but_47;
        for (int z = kMinZone; z <= kMaxZone; ++z) {
            if (z != 47) all_but_47.insert(z);
        }
        REQUIRE(next_free_zone(all_but_47) == 47);

        std::set<int> all_but_last;
        for (int z = kMinZone; z < kMaxZone; ++z) all_but_last.insert(z);
        REQUIRE(next_free_zone(all_but_last) == 99);
    }

    SECTION("the whole namespace occupied: nullopt, never a sentinel") {
        std::set<int> taken;
        for (int z = kMinZone; z <= kMaxZone; ++z) taken.insert(z);
        REQUIRE(next_free_zone(taken) == std::nullopt);
    }

    SECTION("every offer is in range, distinct, and 0 is never among them") {
        std::set<int> taken;
        for (int z = kMinZone; z <= kMaxZone; ++z) {
            const std::optional<int> offer = next_free_zone(taken);
            REQUIRE(offer.has_value());
            REQUIRE(denso::camera::zone_in_range(*offer));
            REQUIRE(*offer != 0);
            REQUIRE(taken.count(*offer) == 0);
            taken.insert(*offer);
        }
        // Exactly 99 distinct offers, then exhaustion.
        REQUIRE(taken.size() == 99u);
        REQUIRE(taken.count(0) == 0);
        REQUIRE(next_free_zone(taken) == std::nullopt);
    }

    SECTION("a taken set outside the namespace does not shift the offer") {
        // Ownership sets come from persistence; a stray out-of-range value —
        // a legacy 0 among them — must not push the allocator off the first
        // genuinely free number.
        REQUIRE(next_free_zone({-5, 0, 100, 250}) == 1);
    }
}
