#include <catch2/catch_test_macros.hpp>

#include "db/db.h"
#include "settings/repo.h"
#include "settings/settings.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QSqlQuery>

#include <utility>

using denso::db::Db;
using denso::db::run_migrations;
using denso::settings::import_legacy;
using denso::settings::load;
using denso::settings::save;
using denso::settings::Settings;

namespace {

/// A migrated, empty in-memory database.
Db db() {
    auto d = Db::open_in_memory();
    REQUIRE(d.has_value());
    REQUIRE(run_migrations(d->handle()));
    return std::move(*d);
}

void write_file(const QString& path, const QByteArray& contents) {
    QFile f(path);
    REQUIRE(f.open(QIODevice::WriteOnly));
    f.write(contents);
    f.close();
}

} // namespace

TEST_CASE("load returns defaults on empty db") {
    const Db d = db();
    const Settings s = load(d.handle());
    REQUIRE(s.width == 1600);
    REQUIRE(s.height == 900);
    REQUIRE(s.dark);
    REQUIRE_FALSE(s.fullscreen);
}

TEST_CASE("load uses defaults for missing keys") {
    // Only width/height persisted; theme/fullscreen must fall back.
    const Db d = db();
    QSqlQuery q(d.handle());
    REQUIRE(q.exec(QStringLiteral("INSERT INTO settings (key,value) VALUES ('width','800')")));
    REQUIRE(q.exec(QStringLiteral("INSERT INTO settings (key,value) VALUES ('height','600')")));
    const Settings s = load(d.handle());
    REQUIRE(s.width == 800);
    REQUIRE(s.height == 600);
    REQUIRE(s.dark);  // default_dark
    REQUIRE_FALSE(s.fullscreen);
}

TEST_CASE("save then load roundtrips all fields") {
    const Db d = db();
    save(d.handle(), Settings{1280, 720, false, true});
    const Settings back = load(d.handle());
    REQUIRE(back.width == 1280);
    REQUIRE(back.height == 720);
    REQUIRE_FALSE(back.dark);
    REQUIRE(back.fullscreen);
}

TEST_CASE("import writes settings and deletes file") {
    const Db d = db();
    const QString path = QDir::tempPath() + QStringLiteral("/denso_import_ok.json");
    write_file(path, R"({"width":1280,"height":720,"dark":false,"fullscreen":true})");

    import_legacy(d.handle(), path);

    const Settings s = load(d.handle());
    REQUIRE(s.width == 1280);
    REQUIRE(s.height == 720);
    REQUIRE_FALSE(s.dark);
    REQUIRE(s.fullscreen);
    REQUIRE_FALSE(QFile::exists(path));  // legacy file deleted after import
}

TEST_CASE("import is noop when file absent") {
    const Db d = db();
    const QString path = QDir::tempPath() + QStringLiteral("/denso_import_absent.json");
    QFile::remove(path);

    import_legacy(d.handle(), path);  // must not crash

    const Settings s = load(d.handle());
    REQUIRE(s.width == 1600);  // untouched defaults
    REQUIRE(s.height == 900);
}

TEST_CASE("import leaves corrupt file intact") {
    const Db d = db();
    const QString path = QDir::tempPath() + QStringLiteral("/denso_import_corrupt.json");
    write_file(path, "}{ not json");

    import_legacy(d.handle(), path);

    REQUIRE(QFile::exists(path));  // corrupt file kept for inspection
    const Settings s = load(d.handle());
    REQUIRE(s.width == 1600);  // defaults, nothing imported
    REQUIRE(s.height == 900);
    QFile::remove(path);
}

TEST_CASE("save overwrites previous values") {
    const Db d = db();
    save(d.handle(), Settings{800, 600, true, false});
    save(d.handle(), Settings{1920, 1080, false, true});
    const Settings back = load(d.handle());
    REQUIRE(back.width == 1920);
    REQUIRE(back.height == 1080);
    REQUIRE_FALSE(back.dark);
    REQUIRE(back.fullscreen);
}
