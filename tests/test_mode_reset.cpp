// Unit tests for the atomic switch-and-reset transaction + preview counts
// (Slice 3 of the operating-modes feature). All pure/backend-free: denso_core
// + an in-memory SQLite DB only. Proves the spec §12.4-§12.9, §12.16 acceptance
// criteria at the unit level:
//   - every camera row + id + connection/capture column is preserved;
//   - only setup_complete / areas_need_review are reset;
//   - the five mode-owned tables are emptied (camera_model_class unconditionally,
//     so pre-existing orphans are repaired too);
//   - mode.target is switched and brazing.enabled disabled inside ONE transaction
//     while brazing.base_url is preserved;
//   - any failure at any statement rolls the whole transaction back, leaving all
//     original state intact and returning the SQL error verbatim.
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_adapters.hpp>

#include "mode/reset.h"
#include "mode/config.h"
#include "camera/repo.h"
#include "brazing/config.h"
#include "db/db.h"

#include <QSqlDatabase>
#include <QSqlQuery>
#include <QString>
#include <QVariant>

#include <cstdint>

using denso::mode::TargetMode;

namespace {

// An IP camera with every credential/channel/stream/manufacturer field set, so
// the preservation test can assert the IP-only columns survive byte-identically.
int64_t seed_camera(const QSqlDatabase& db, bool active, bool setup) {
    denso::camera::Camera c;
    c.name = "cam";
    c.camera_type = "ip";
    c.ip = "10.0.0.9";
    c.rtsp = "rtsp://10.0.0.9/s1";
    c.username = "u";
    c.password = "p";
    c.channel = 1u;
    c.stream = 0u;
    c.manufacturer = "Dahua";
    c.width = 1920;
    c.height = 1080;
    c.fps = 25;
    c.pitch = 1.5f;
    c.roll = -2.0f;
    c.rotation = 90;
    c.active = active;
    c.setup_complete = setup;
    c.areas_need_review = true;
    auto id = denso::camera::insert(db, c);
    REQUIRE(id);
    return *id;
}

// A USB camera with a non-null cam_index, so the USB-only column is exercised.
int64_t seed_camera_usb(const QSqlDatabase& db, bool active, bool setup) {
    denso::camera::Camera c;
    c.name = "usbcam";
    c.camera_type = "usb";
    c.index = 2u;  // cam_index — the USB-only field
    c.width = 1280;
    c.height = 720;
    c.fps = 30;
    c.pitch = 0.0f;
    c.roll = 0.0f;
    c.rotation = 180;
    c.active = active;
    c.setup_complete = setup;
    c.areas_need_review = false;
    auto id = denso::camera::insert(db, c);
    REQUIRE(id);
    return *id;
}

void exec(const QSqlDatabase& db, const QString& sql) {
    QSqlQuery q(db);
    REQUIRE(q.exec(sql));
}

int count(const QSqlDatabase& db, const QString& table) {
    QSqlQuery q(db);
    REQUIRE(q.exec("SELECT COUNT(*) FROM " + table));
    REQUIRE(q.next());
    return q.value(0).toInt();
}

// Seed exactly one row in each of the five mode-owned tables (parent camera id
// supplied) so a BEFORE-DELETE trigger on any of them actually fires (a DELETE
// over an empty table never touches a row, so its trigger would never run).
void seed_one_row_each(const QSqlDatabase& db, int64_t camera_id) {
    exec(db, QStringLiteral("INSERT INTO camera_area (camera_id,name,points,zone) "
                            "VALUES (%1,'a','[]',4)").arg(camera_id));
    exec(db, QStringLiteral("INSERT INTO camera_model (camera_id,model_id) "
                            "VALUES (%1,1)").arg(camera_id));
    exec(db, QStringLiteral("INSERT INTO camera_model_class (camera_model_id,class_id,conf) "
                            "VALUES (1,0,0.5)"));
    exec(db, QStringLiteral("INSERT INTO reading (camera_id,ts_ms,value,conf) "
                            "VALUES (%1,1,'1234',0.9)").arg(camera_id));
    exec(db, QStringLiteral(
        "INSERT INTO model_migration_receipt "
        "(created_utc,old_filename,old_model_id,old_name,old_class_names,"
        " new_filename,new_model_id,new_engine_sha256,forward_map,inverse_map,attachments) "
        "VALUES ('2026-07-21T00:00:00Z','old.engine',1,'old','[]',"
        "        'new.engine',2,'deadbeef','{}','{}','[]')"));
}

} // namespace

// ─── preview_counts ──────────────────────────────────────────────────────────

TEST_CASE("preview_counts returns the real row counts", "[mode_reset]") {
    auto db = denso::db::Db::open_in_memory();
    REQUIRE(db);
    REQUIRE(denso::db::run_migrations(db->handle()));
    const int64_t id1 = seed_camera(db->handle(), true, true);
    seed_camera_usb(db->handle(), false, true);  // a second retained camera
    // 2 model bindings, 3 areas, 4 readings, 2 receipts.
    exec(db->handle(), QStringLiteral("INSERT INTO camera_model (camera_id,model_id) VALUES (%1,1)").arg(id1));
    exec(db->handle(), QStringLiteral("INSERT INTO camera_model (camera_id,model_id) VALUES (%1,2)").arg(id1));
    exec(db->handle(), QStringLiteral("INSERT INTO camera_area (camera_id,name,points,zone) VALUES (%1,'a','[]',4)").arg(id1));
    exec(db->handle(), QStringLiteral("INSERT INTO camera_area (camera_id,name,points,zone) VALUES (%1,'b','[]',2)").arg(id1));
    exec(db->handle(), QStringLiteral("INSERT INTO camera_area (camera_id,name,points,zone) VALUES (%1,'c','[]',NULL)").arg(id1));
    for (int i = 0; i < 4; ++i) {
        exec(db->handle(), QStringLiteral("INSERT INTO reading (camera_id,ts_ms,value,conf) VALUES (%1,%2,'9',0.5)").arg(id1).arg(i));
    }
    for (int i = 0; i < 2; ++i) {
        exec(db->handle(), QStringLiteral(
            "INSERT INTO model_migration_receipt "
            "(created_utc,old_filename,old_model_id,old_name,old_class_names,"
            " new_filename,new_model_id,new_engine_sha256,forward_map,inverse_map,attachments) "
            "VALUES ('t','o',1,'o','[]','n',2,'x','{}','{}','[]')"));
    }

    const auto c = denso::mode::preview_counts(db->handle());
    REQUIRE(c);
    CHECK(c->cameras == 2);
    CHECK(c->model_bindings == 2);
    CHECK(c->areas == 3);
    CHECK(c->readings == 4);
    CHECK(c->receipts == 2);
}

TEST_CASE("preview_counts returns sorted distinct non-zero zone numbers", "[mode_reset]") {
    auto db = denso::db::Db::open_in_memory();
    REQUIRE(db);
    REQUIRE(denso::db::run_migrations(db->handle()));
    const int64_t id = seed_camera(db->handle(), true, true);
    // Zones out of order, with a duplicate, a 0 sentinel, and a NULL — the
    // preview must return only distinct non-null non-zero, ascending: {2,4,7}.
    exec(db->handle(), QStringLiteral("INSERT INTO camera_area (camera_id,name,points,zone) VALUES (%1,'a','[]',7)").arg(id));
    exec(db->handle(), QStringLiteral("INSERT INTO camera_area (camera_id,name,points,zone) VALUES (%1,'b','[]',2)").arg(id));
    exec(db->handle(), QStringLiteral("INSERT INTO camera_area (camera_id,name,points,zone) VALUES (%1,'c','[]',4)").arg(id));
    exec(db->handle(), QStringLiteral("INSERT INTO camera_area (camera_id,name,points,zone) VALUES (%1,'d','[]',2)").arg(id));
    exec(db->handle(), QStringLiteral("INSERT INTO camera_area (camera_id,name,points,zone) VALUES (%1,'e','[]',0)").arg(id));
    exec(db->handle(), QStringLiteral("INSERT INTO camera_area (camera_id,name,points,zone) VALUES (%1,'f','[]',NULL)").arg(id));

    const auto c = denso::mode::preview_counts(db->handle());
    REQUIRE(c);
    CHECK(c->zones == std::vector<int>{2, 4, 7});
}

TEST_CASE("preview_counts returns nullopt when a count query fails", "[mode_reset]") {
    auto db = denso::db::Db::open_in_memory();
    REQUIRE(db);
    REQUIRE(denso::db::run_migrations(db->handle()));
    exec(db->handle(), QStringLiteral("DROP TABLE reading"));  // force a query failure
    CHECK_FALSE(denso::mode::preview_counts(db->handle()).has_value());
}

// ─── camera preservation ─────────────────────────────────────────────────────

TEST_CASE("reset preserves every camera row, id, and all connection/capture columns", "[mode_reset]") {
    auto db = denso::db::Db::open_in_memory();
    REQUIRE(db);
    REQUIRE(denso::db::run_migrations(db->handle()));
    // Seed BOTH an IP and a USB camera so every shared field — incl. the USB-only
    // cam_index and the IP-only credential/channel fields — is non-null on at
    // least one row.
    const int64_t ip_id = seed_camera(db->handle(), /*active*/ true, /*setup*/ true);
    const int64_t usb_id = seed_camera_usb(db->handle(), /*active*/ false, /*setup*/ true);
    const auto before_ip = denso::camera::get(db->handle(), ip_id);
    const auto before_usb = denso::camera::get(db->handle(), usb_id);
    REQUIRE(before_ip);
    REQUIRE(before_usb);

    const auto r = denso::mode::switch_and_reset(db->handle(), TargetMode::BallLeveler);
    REQUIRE(r.ok);

    const auto check_preserved = [](const denso::camera::Camera& b, const denso::camera::Camera& a) {
        CHECK(a.id == b.id);
        CHECK(a.name == b.name);
        CHECK(a.camera_type == b.camera_type);
        CHECK(a.active == b.active);  // A1: preserved (incl. active=false)
        CHECK(a.index == b.index);    // cam_index
        CHECK(a.ip == b.ip);
        CHECK(a.rtsp == b.rtsp);
        CHECK(a.username == b.username);
        CHECK(a.password == b.password);
        CHECK(a.channel == b.channel);
        CHECK(a.stream == b.stream);
        CHECK(a.manufacturer == b.manufacturer);
        CHECK(a.width == b.width);
        CHECK(a.height == b.height);
        CHECK(a.fps == b.fps);
        CHECK(a.pitch == b.pitch);
        CHECK(a.roll == b.roll);
        CHECK(a.rotation == b.rotation);
        CHECK(a.setup_complete == false);     // reset
        CHECK(a.areas_need_review == false);  // reset
    };
    check_preserved(*before_ip, *denso::camera::get(db->handle(), ip_id));
    check_preserved(*before_usb, *denso::camera::get(db->handle(), usb_id));
}

// ─── workspace deletion ──────────────────────────────────────────────────────

TEST_CASE("reset empties the five mode-owned tables and leaves the camera", "[mode_reset]") {
    auto db = denso::db::Db::open_in_memory();
    REQUIRE(db);
    REQUIRE(denso::db::run_migrations(db->handle()));
    const int64_t id = seed_camera(db->handle(), true, true);
    seed_one_row_each(db->handle(), id);

    const auto r = denso::mode::switch_and_reset(db->handle(), TargetMode::BallLeveler);
    REQUIRE(r.ok);
    CHECK(count(db->handle(), "camera_model_class") == 0);
    CHECK(count(db->handle(), "camera_model") == 0);
    CHECK(count(db->handle(), "camera_area") == 0);
    CHECK(count(db->handle(), "reading") == 0);
    CHECK(count(db->handle(), "model_migration_receipt") == 0);
    CHECK(count(db->handle(), "camera") == 1);  // camera preserved
}

// ─── orphan repair ───────────────────────────────────────────────────────────

TEST_CASE("reset deletes camera_model_class rows already orphaned by camera::remove", "[mode_reset]") {
    auto db = denso::db::Db::open_in_memory();
    REQUIRE(db);
    REQUIRE(denso::db::run_migrations(db->handle()));
    // Orphan: a class row whose parent camera_model does not exist (the state
    // camera::remove leaves behind). A scoped delete would miss it.
    exec(db->handle(), QStringLiteral(
        "INSERT INTO camera_model_class (camera_model_id,class_id,conf) VALUES (999,0,0.5)"));
    const auto r = denso::mode::switch_and_reset(db->handle(), TargetMode::DigitReader);
    REQUIRE(r.ok);
    CHECK(count(db->handle(), "camera_model_class") == 0);  // spec §12.16
}

// ─── runtime gate ────────────────────────────────────────────────────────────

TEST_CASE("after reset runtime() is empty even for an active camera", "[mode_reset]") {
    auto db = denso::db::Db::open_in_memory();
    REQUIRE(db);
    REQUIRE(denso::db::run_migrations(db->handle()));
    seed_camera(db->handle(), /*active*/ true, /*setup*/ true);
    REQUIRE(denso::mode::switch_and_reset(db->handle(), TargetMode::BallLeveler).ok);
    CHECK(denso::camera::runtime(db->handle()).empty());  // spec §12.6
    CHECK(denso::camera::all(db->handle()).size() == 1);
}

// ─── reporting + mode ────────────────────────────────────────────────────────

TEST_CASE("reset writes the new mode and disables brazing but keeps the url", "[mode_reset]") {
    auto db = denso::db::Db::open_in_memory();
    REQUIRE(db);
    REQUIRE(denso::db::run_migrations(db->handle()));
    denso::brazing::BrazingConfig b;
    b.enabled = true;
    b.base_url = "http://host:8098";
    denso::brazing::save(db->handle(), b);
    REQUIRE(denso::mode::save(db->handle(), TargetMode::DigitReader));

    REQUIRE(denso::mode::switch_and_reset(db->handle(), TargetMode::BallLeveler).ok);
    CHECK(denso::mode::load(db->handle()) == TargetMode::BallLeveler);
    const auto after = denso::brazing::load(db->handle());
    CHECK_FALSE(after.enabled);                    // A2
    CHECK(after.base_url == "http://host:8098");   // preserved
}

// ─── rollback (single injected failure) ──────────────────────────────────────

TEST_CASE("a failure inside the transaction rolls back mode, reporting, and camera flags", "[mode_reset]") {
    auto db = denso::db::Db::open_in_memory();
    REQUIRE(db);
    REQUIRE(denso::db::run_migrations(db->handle()));
    const int64_t id = seed_camera(db->handle(), true, true);
    denso::brazing::BrazingConfig b;
    b.enabled = true;
    b.base_url = "http://host";
    denso::brazing::save(db->handle(), b);
    exec(db->handle(), QStringLiteral("INSERT INTO reading (camera_id,ts_ms,value,conf) "
                                      "VALUES (%1,1,'1',0.5)").arg(id));
    // Force the DELETE FROM reading to fail mid-transaction.
    exec(db->handle(), QStringLiteral(
        "CREATE TRIGGER boom BEFORE DELETE ON reading BEGIN "
        "SELECT RAISE(ABORT,'injected'); END"));

    const auto r = denso::mode::switch_and_reset(db->handle(), TargetMode::BallLeveler);
    CHECK_FALSE(r.ok);
    CHECK(!r.error.empty());  // SQL error surfaced verbatim
    // The verbatim SQL error text is propagated, not a generic message: the
    // RAISE(ABORT,'injected') reason must appear in ResetResult.error (req 12).
    CHECK(r.error.find("injected") != std::string::npos);
    // Everything reverts together (spec §12.9):
    CHECK(denso::mode::load(db->handle()) == TargetMode::DigitReader);
    CHECK(denso::brazing::load(db->handle()).enabled == true);
    const auto after = denso::camera::get(db->handle(), id);
    REQUIRE(after);
    CHECK(after->setup_complete == true);
    CHECK(after->areas_need_review == true);
    CHECK(count(db->handle(), "reading") == 1);
}

// ─── failure-injection matrix (every statement, in turn) ─────────────────────
//
// Each transaction statement has a concrete BEFORE trigger that RAISE(ABORT)s.
// GENERATE re-runs the whole body (fresh in-memory DB) once per row, so every
// statement is injected independently with the IDENTICAL post-condition:
// switch_and_reset returns {ok:false, error non-empty}, and mode.target,
// brazing.enabled, the camera flags, and all five mode-owned rows are unchanged.
// The two settings triggers are key-specific so the SECOND settings upsert is
// tested independently — proving a failure there rolls back the FIRST (mode.target).
TEST_CASE("a failure at every transaction statement rolls the whole transaction back", "[mode_reset]") {
    auto [label, trigger] = GENERATE(table<const char*, const char*>({
        {"camera_model_class",
         "CREATE TRIGGER t BEFORE DELETE ON camera_model_class BEGIN SELECT RAISE(ABORT,'injected'); END"},
        {"camera_model",
         "CREATE TRIGGER t BEFORE DELETE ON camera_model BEGIN SELECT RAISE(ABORT,'injected'); END"},
        {"camera_area",
         "CREATE TRIGGER t BEFORE DELETE ON camera_area BEGIN SELECT RAISE(ABORT,'injected'); END"},
        {"reading",
         "CREATE TRIGGER t BEFORE DELETE ON reading BEGIN SELECT RAISE(ABORT,'injected'); END"},
        {"model_migration_receipt",
         "CREATE TRIGGER t BEFORE DELETE ON model_migration_receipt BEGIN SELECT RAISE(ABORT,'injected'); END"},
        {"camera_update",
         "CREATE TRIGGER t BEFORE UPDATE ON camera BEGIN SELECT RAISE(ABORT,'injected'); END"},
        {"settings_mode_target",
         "CREATE TRIGGER t BEFORE UPDATE ON settings WHEN NEW.key='mode.target' "
         "BEGIN SELECT RAISE(ABORT,'injected'); END"},
        {"settings_brazing_enabled",
         "CREATE TRIGGER t BEFORE UPDATE ON settings WHEN NEW.key='brazing.enabled' "
         "BEGIN SELECT RAISE(ABORT,'injected'); END"},
    }));
    INFO("injected failure at: " << label);

    auto db = denso::db::Db::open_in_memory();
    REQUIRE(db);
    REQUIRE(denso::db::run_migrations(db->handle()));
    const int64_t id = seed_camera(db->handle(), true, true);
    seed_one_row_each(db->handle(), id);
    // Seed BOTH settings rows so each upsert takes the ON CONFLICT UPDATE branch
    // (BEFORE-UPDATE), letting the key-specific triggers fire.
    denso::brazing::BrazingConfig b;
    b.enabled = true;
    b.base_url = "http://host";
    denso::brazing::save(db->handle(), b);  // brazing.enabled='1'
    REQUIRE(denso::mode::save(db->handle(), TargetMode::DigitReader));  // mode.target seeded

    exec(db->handle(), QString::fromLatin1(trigger));

    const auto r = denso::mode::switch_and_reset(db->handle(), TargetMode::BallLeveler);
    CHECK_FALSE(r.ok);
    CHECK(!r.error.empty());
    CHECK(r.error.find("injected") != std::string::npos);  // verbatim SQL text (req 12)

    // All original state survives, whichever statement was injected.
    CHECK(denso::mode::load(db->handle()) == TargetMode::DigitReader);
    CHECK(denso::brazing::load(db->handle()).enabled == true);
    const auto after = denso::camera::get(db->handle(), id);
    REQUIRE(after);
    CHECK(after->setup_complete == true);
    CHECK(after->areas_need_review == true);
    CHECK(count(db->handle(), "camera_model_class") == 1);
    CHECK(count(db->handle(), "camera_model") == 1);
    CHECK(count(db->handle(), "camera_area") == 1);
    CHECK(count(db->handle(), "reading") == 1);
    CHECK(count(db->handle(), "model_migration_receipt") == 1);
}
