#include <catch2/catch_test_macros.hpp>

#include "mode/mode.h"
#include "mode/config.h"
#include "settings/repo.h"
#include "settings/settings.h"
#include "camera/repo.h"
#include "camera/camera.h"
#include "db/db.h"

#include <QSqlQuery>

#include <optional>
#include <string>

using denso::mode::TargetMode;

TEST_CASE("target mode round-trips through its token", "[mode]") {
    CHECK(std::string(denso::mode::to_string(TargetMode::DigitReader)) == "digit_reader");
    CHECK(std::string(denso::mode::to_string(TargetMode::BallLeveler)) == "ball_leveler");
    CHECK(denso::mode::parse_target_mode("digit_reader") == TargetMode::DigitReader);
    CHECK(denso::mode::parse_target_mode("ball_leveler") == TargetMode::BallLeveler);
}

TEST_CASE("unknown, empty, or corrupt tokens resolve to digit_reader", "[mode]") {
    CHECK(denso::mode::parse_target_mode("") == TargetMode::DigitReader);
    CHECK(denso::mode::parse_target_mode("leveler") == TargetMode::DigitReader);
    CHECK(denso::mode::parse_target_mode("DIGIT_READER") == TargetMode::DigitReader);
    CHECK(denso::mode::parse_target_mode("\x01garbage") == TargetMode::DigitReader);
}

TEST_CASE("an invalid selector index resolves to digit_reader", "[mode]") {
    CHECK(denso::mode::from_index(0) == TargetMode::DigitReader);
    CHECK(denso::mode::from_index(1) == TargetMode::BallLeveler);
    CHECK(denso::mode::from_index(99) == TargetMode::DigitReader);
    CHECK(denso::mode::from_index(-1) == TargetMode::DigitReader);
}

TEST_CASE("mode config load/save over the settings table", "[mode]") {
    auto db = denso::db::Db::open_in_memory();
    REQUIRE(db);
    REQUIRE(denso::db::run_migrations(db->handle()));

    CHECK(denso::mode::load(db->handle()) == TargetMode::DigitReader);  // absent → default
    REQUIRE(denso::mode::save(db->handle(), TargetMode::BallLeveler));
    CHECK(denso::mode::load(db->handle()) == TargetMode::BallLeveler);
}

TEST_CASE("a corrupt stored token loads as digit_reader", "[mode]") {
    auto db = denso::db::Db::open_in_memory();
    REQUIRE(db);
    REQUIRE(denso::db::run_migrations(db->handle()));
    QSqlQuery q(db->handle());
    q.prepare(QStringLiteral("INSERT INTO settings (key, value) VALUES ('mode.target', ?)"));
    q.addBindValue(QStringLiteral("floating_ball_v2"));
    REQUIRE(q.exec());
    CHECK(denso::mode::load(db->handle()) == TargetMode::DigitReader);
}

TEST_CASE("Reset to defaults does not disturb mode.target", "[mode]") {
    auto db = denso::db::Db::open_in_memory();
    REQUIRE(db);
    REQUIRE(denso::db::run_migrations(db->handle()));
    REQUIRE(denso::mode::save(db->handle(), TargetMode::BallLeveler));
    // Simulate MainWindow::on_reset_defaults: save a default-constructed Settings.
    denso::settings::save(db->handle(), denso::settings::Settings{});
    CHECK(denso::mode::load(db->handle()) == TargetMode::BallLeveler);
}

TEST_CASE("persisting a mode adds no schema migration of its own", "[mode]") {
    auto db = denso::db::Db::open_in_memory();
    REQUIRE(db);
    REQUIRE(denso::db::run_migrations(db->handle()));
    // The mode key rides the existing `settings` table and contributes NO
    // migration. v14 is Slice 1's ball_level_calibration, not the mode key.
    REQUIRE(denso::db::supported_schema_version() == 14);
    CHECK(denso::db::read_user_version(db->handle()) == 14);
    REQUIRE(denso::mode::save(db->handle(), TargetMode::BallLeveler));
    // The mode key rides the existing settings table; no DDL runs on save.
    CHECK(denso::db::read_user_version(db->handle()) == 14);
}

TEST_CASE("digit_reader mode_setup_required is true with zero completed cameras", "[mode]") {
    auto db = denso::db::Db::open_in_memory();
    REQUIRE(db); REQUIRE(denso::db::run_migrations(db->handle()));
    CHECK(denso::mode::mode_setup_required(db->handle(), TargetMode::DigitReader)
          == std::optional<bool>(true));
}

TEST_CASE("digit_reader mode_setup_required is false when a completed but inactive camera exists", "[mode]") {
    auto db = denso::db::Db::open_in_memory();
    REQUIRE(db); REQUIRE(denso::db::run_migrations(db->handle()));
    denso::camera::Camera c; c.name = "cam"; c.camera_type = "usb"; c.index = 0u;
    c.active = false; c.setup_complete = true;          // configured, disabled
    REQUIRE(denso::camera::insert(db->handle(), c));
    CHECK(denso::mode::mode_setup_required(db->handle(), TargetMode::DigitReader)
          == std::optional<bool>(false));               // NOT runtime()-based
}

TEST_CASE("ball_leveler mode_setup_required is ALWAYS true, even with a completed camera", "[mode]") {
    auto db = denso::db::Db::open_in_memory();
    REQUIRE(db); REQUIRE(denso::db::run_migrations(db->handle()));
    denso::camera::Camera c; c.name = "cam"; c.camera_type = "usb"; c.index = 0u;
    c.active = true; c.setup_complete = true;            // a completed camera exists
    REQUIRE(denso::camera::insert(db->handle(), c));
    CHECK(denso::mode::mode_setup_required(db->handle(), TargetMode::BallLeveler)
          == std::optional<bool>(true));                // spec §2.1 — permanent
}

TEST_CASE("digit_reader mode_setup_required is nullopt when the query cannot run", "[mode]") {
    auto db = denso::db::Db::open_in_memory();
    REQUIRE(db); REQUIRE(denso::db::run_migrations(db->handle()));
    QSqlQuery(db->handle()).exec(QStringLiteral("DROP TABLE camera"));  // force a query failure
    CHECK_FALSE(denso::mode::mode_setup_required(db->handle(), TargetMode::DigitReader).has_value());
}
