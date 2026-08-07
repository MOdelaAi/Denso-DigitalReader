#include <catch2/catch_test_macros.hpp>

#include "cli/args.h"

using denso::cli::Command;
using denso::cli::Mode;
using denso::cli::is_headless;
using denso::cli::parse;

TEST_CASE("no arguments means the GUI", "[cli]") {
    const Command c = parse({});
    REQUIRE(c.mode == Mode::Gui);
    REQUIRE_FALSE(is_headless(c.mode));
}

TEST_CASE("each headless flag maps to its mode", "[cli]") {
    REQUIRE(parse({QStringLiteral("--version")}).mode == Mode::Version);
    REQUIRE(parse({QStringLiteral("--check")}).mode == Mode::Check);
    REQUIRE(parse({QStringLiteral("--check-running")}).mode == Mode::CheckRunning);
}

TEST_CASE("every non-GUI mode is headless", "[cli]") {
    REQUIRE(is_headless(Mode::Version));
    REQUIRE(is_headless(Mode::Check));
    REQUIRE(is_headless(Mode::CheckRunning));
    REQUIRE(is_headless(Mode::CheckMigrations));
    REQUIRE(is_headless(Mode::ApplyMigrations));
    // Error prints usage and exits — it must not open a window either.
    REQUIRE(is_headless(Mode::Error));
}

TEST_CASE("parse: --apply-migrations takes no path", "[cli]") {
    const Command c = parse({QStringLiteral("--apply-migrations")});
    REQUIRE(c.mode == Mode::ApplyMigrations);
    REQUIRE(c.arg.isEmpty());
}

// The safety property of the whole primitive: the in-place migrator cannot be
// aimed at an arbitrary file. Passing a path is the natural mistake, since
// --check-migrations one line above REQUIRES one — so it must be a hard error,
// never a migration of something else.
TEST_CASE("parse: --apply-migrations REFUSES a path", "[cli]") {
    const Command c = parse({QStringLiteral("--apply-migrations"),
                             QStringLiteral("/opt/denso/data/denso.db")});
    REQUIRE(c.mode == Mode::Error);
    REQUIRE_FALSE(c.error.isEmpty());
    // The message must name the copy-only alternative, or the operator's next
    // guess is to force it some other way.
    REQUIRE(c.error.contains(QStringLiteral("--check-migrations")));
}

TEST_CASE("parse: the copy-only and in-place migrators are distinct modes", "[cli]") {
    REQUIRE(parse({QStringLiteral("--check-migrations"), QStringLiteral("/tmp/c.db")}).mode
            == Mode::CheckMigrations);
    REQUIRE(parse({QStringLiteral("--apply-migrations")}).mode == Mode::ApplyMigrations);
}

TEST_CASE("parse: --check-migrations carries its db path", "[cli]") {
    const Command c = parse({QStringLiteral("--check-migrations"),
                             QStringLiteral("/tmp/copy.db")});
    REQUIRE(c.mode == Mode::CheckMigrations);
    REQUIRE(c.arg == QStringLiteral("/tmp/copy.db"));
}

TEST_CASE("parse: --check-migrations without a path is an error, not a GUI launch", "[cli]") {
    const Command c = parse({QStringLiteral("--check-migrations")});
    REQUIRE(c.mode == Mode::Error);
    REQUIRE_FALSE(c.error.isEmpty());
}

TEST_CASE("an unknown flag is an error, not a silent GUI launch", "[cli]") {
    const Command c = parse({QStringLiteral("--wat")});
    REQUIRE(c.mode == Mode::Error);
    REQUIRE(c.error.contains(QStringLiteral("--wat")));
}

TEST_CASE("a trailing extra argument is an error", "[cli]") {
    REQUIRE(parse({QStringLiteral("--check"), QStringLiteral("junk")}).mode == Mode::Error);
    REQUIRE(parse({QStringLiteral("--check-migrations"), QStringLiteral("a"),
                   QStringLiteral("b")}).mode == Mode::Error);
}

TEST_CASE("parse: --check takes zero or more --engine names", "[cli]") {
    SECTION("none") {
        const Command c = parse({QStringLiteral("--check")});
        REQUIRE(c.mode == Mode::Check);
        REQUIRE(c.engines.isEmpty());
    }
    SECTION("one") {
        const Command c = parse({QStringLiteral("--check"), QStringLiteral("--engine"),
                                 QStringLiteral("digitv2.engine")});
        REQUIRE(c.mode == Mode::Check);
        REQUIRE(c.engines == QStringList{QStringLiteral("digitv2.engine")});
    }
    SECTION("repeated") {
        const Command c = parse({QStringLiteral("--check"),
                                 QStringLiteral("--engine"), QStringLiteral("a.engine"),
                                 QStringLiteral("--engine"), QStringLiteral("b.engine")});
        REQUIRE(c.mode == Mode::Check);
        REQUIRE(c.engines == QStringList{QStringLiteral("a.engine"), QStringLiteral("b.engine")});
    }
}

TEST_CASE("parse: --engine without a value is an error", "[cli]") {
    REQUIRE(parse({QStringLiteral("--check"), QStringLiteral("--engine")}).mode == Mode::Error);
}

TEST_CASE("parse: --engine only applies to --check", "[cli]") {
    REQUIRE(parse({QStringLiteral("--version"), QStringLiteral("--engine"),
                   QStringLiteral("a.engine")}).mode == Mode::Error);
}

TEST_CASE("parse: --migrate-model with old, new, and repeated --camera", "[cli]") {
    const Command c = parse({QStringLiteral("--migrate-model"),
                             QStringLiteral("--old"), QStringLiteral("a.engine"),
                             QStringLiteral("--new"), QStringLiteral("b.engine"),
                             QStringLiteral("--camera"), QStringLiteral("1"),
                             QStringLiteral("--camera"), QStringLiteral("2")});
    REQUIRE(c.mode == Mode::MigrateModel);
    REQUIRE(c.old_engine == QStringLiteral("a.engine"));
    REQUIRE(c.new_engine == QStringLiteral("b.engine"));
    REQUIRE(c.cameras == QList<qint64>{1, 2});
}

TEST_CASE("parse: --migrate-model captures an optional --class-map", "[cli]") {
    const Command c = parse({QStringLiteral("--migrate-model"),
                             QStringLiteral("--old"), QStringLiteral("a.engine"),
                             QStringLiteral("--new"), QStringLiteral("b.engine"),
                             QStringLiteral("--camera"), QStringLiteral("1"),
                             QStringLiteral("--class-map"), QStringLiteral("p.json")});
    REQUIRE(c.mode == Mode::MigrateModel);
    REQUIRE(c.class_map_path == QStringLiteral("p.json"));
}

TEST_CASE("parse: --migrate-model without --old is an error", "[cli]") {
    const Command c = parse({QStringLiteral("--migrate-model"),
                             QStringLiteral("--new"), QStringLiteral("b.engine"),
                             QStringLiteral("--camera"), QStringLiteral("1")});
    REQUIRE(c.mode == Mode::Error);
}

TEST_CASE("parse: --migrate-model without --new is an error", "[cli]") {
    const Command c = parse({QStringLiteral("--migrate-model"),
                             QStringLiteral("--old"), QStringLiteral("a.engine"),
                             QStringLiteral("--camera"), QStringLiteral("1")});
    REQUIRE(c.mode == Mode::Error);
}

TEST_CASE("parse: --migrate-model without any --camera is an error", "[cli]") {
    const Command c = parse({QStringLiteral("--migrate-model"),
                             QStringLiteral("--old"), QStringLiteral("a.engine"),
                             QStringLiteral("--new"), QStringLiteral("b.engine")});
    REQUIRE(c.mode == Mode::Error);
}

TEST_CASE("parse: --migrate-model with a non-integer --camera id is an error", "[cli]") {
    const Command c = parse({QStringLiteral("--migrate-model"),
                             QStringLiteral("--old"), QStringLiteral("a.engine"),
                             QStringLiteral("--new"), QStringLiteral("b.engine"),
                             QStringLiteral("--camera"), QStringLiteral("abc")});
    REQUIRE(c.mode == Mode::Error);
}

TEST_CASE("parse: --migrate-model with a duplicate --camera id is an error", "[cli]") {
    const Command c = parse({QStringLiteral("--migrate-model"),
                             QStringLiteral("--old"), QStringLiteral("a.engine"),
                             QStringLiteral("--new"), QStringLiteral("b.engine"),
                             QStringLiteral("--camera"), QStringLiteral("1"),
                             QStringLiteral("--camera"), QStringLiteral("1")});
    REQUIRE(c.mode == Mode::Error);
}

TEST_CASE("parse: --migrate-model with a flag missing its value is an error", "[cli]") {
    const Command c = parse({QStringLiteral("--migrate-model"),
                             QStringLiteral("--new"), QStringLiteral("b.engine"),
                             QStringLiteral("--camera"), QStringLiteral("1"),
                             QStringLiteral("--old")});
    REQUIRE(c.mode == Mode::Error);
}
