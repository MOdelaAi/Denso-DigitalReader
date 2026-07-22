#include <catch2/catch_test_macros.hpp>

#include "mode/mode.h"
#include "mode/config.h"
#include "settings/repo.h"
#include "settings/settings.h"
#include "db/db.h"

#include <QSqlQuery>

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

TEST_CASE("persisting a mode adds no schema migration — user_version stays v13", "[mode]") {
    auto db = denso::db::Db::open_in_memory();
    REQUIRE(db);
    REQUIRE(denso::db::run_migrations(db->handle()));
    REQUIRE(denso::db::supported_schema_version() == 13);
    CHECK(denso::db::read_user_version(db->handle()) == 13);
    REQUIRE(denso::mode::save(db->handle(), TargetMode::BallLeveler));
    // The mode key rides the existing settings table; no DDL runs on save.
    CHECK(denso::db::read_user_version(db->handle()) == 13);
}
