#include <catch2/catch_test_macros.hpp>

#include "db/db.h"
#include "settings/display.h"
#include "settings/repo.h"
#include "settings/settings.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QSqlQuery>

#include <utility>

using denso::db::Db;
using denso::db::run_migrations;
using denso::settings::DisplayMode;
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

QString raw(const Db& d, const char* key) {
    QSqlQuery q(d.handle());
    q.prepare(QStringLiteral("SELECT value FROM settings WHERE key = ?"));
    q.addBindValue(QString::fromLatin1(key));
    q.exec();
    return q.next() ? q.value(0).toString() : QString();
}

} // namespace

TEST_CASE("load returns defaults on empty db") {
    const Db d = db();
    const Settings s = load(d.handle());
    REQUIRE(s.width == 1600);
    REQUIRE(s.height == 900);
    REQUIRE(s.dark);
    REQUIRE(s.mode == DisplayMode::Windowed);
}

TEST_CASE("load uses defaults for missing keys") {
    // Only width/height persisted; theme/mode must fall back.
    const Db d = db();
    QSqlQuery q(d.handle());
    REQUIRE(q.exec(QStringLiteral("INSERT INTO settings (key,value) VALUES ('width','800')")));
    REQUIRE(q.exec(QStringLiteral("INSERT INTO settings (key,value) VALUES ('height','600')")));
    const Settings s = load(d.handle());
    REQUIRE(s.width == 800);
    REQUIRE(s.height == 600);
    REQUIRE(s.dark);  // default_dark
    REQUIRE(s.mode == DisplayMode::Windowed);
}

TEST_CASE("save then load roundtrips all fields") {
    const Db d = db();
    save(d.handle(), Settings{1280, 720, false, DisplayMode::Fullscreen});
    const Settings back = load(d.handle());
    REQUIRE(back.width == 1280);
    REQUIRE(back.height == 720);
    REQUIRE_FALSE(back.dark);
    REQUIRE(back.mode == DisplayMode::Fullscreen);
}

TEST_CASE("save dual-writes display_mode and the legacy fullscreen bool") {
    const Db d = db();

    save(d.handle(), Settings{1600, 900, true, DisplayMode::Fullscreen});
    REQUIRE(raw(d, "display_mode") == QStringLiteral("fullscreen"));
    REQUIRE(raw(d, "fullscreen") == QStringLiteral("1"));  // old build reads Fullscreen

    save(d.handle(), Settings{1600, 900, true, DisplayMode::Borderless});
    REQUIRE(raw(d, "display_mode") == QStringLiteral("borderless"));
    REQUIRE(raw(d, "fullscreen") == QStringLiteral("0"));  // old build downgrades -> Windowed

    save(d.handle(), Settings{1600, 900, true, DisplayMode::Windowed});
    REQUIRE(raw(d, "display_mode") == QStringLiteral("windowed"));
    REQUIRE(raw(d, "fullscreen") == QStringLiteral("0"));
}

TEST_CASE("load prefers display_mode over the legacy fullscreen key") {
    const Db d = db();
    QSqlQuery q(d.handle());
    q.exec(QStringLiteral("INSERT INTO settings(key,value) VALUES('display_mode','borderless')"));
    q.exec(QStringLiteral("INSERT INTO settings(key,value) VALUES('fullscreen','1')"));
    REQUIRE(load(d.handle()).mode == DisplayMode::Borderless);
}

TEST_CASE("load falls back to legacy fullscreen when display_mode absent") {
    const Db d = db();
    QSqlQuery q(d.handle());
    q.exec(QStringLiteral("INSERT INTO settings(key,value) VALUES('fullscreen','1')"));
    REQUIRE(load(d.handle()).mode == DisplayMode::Fullscreen);
    QSqlQuery q2(d.handle());
    q2.exec(QStringLiteral("UPDATE settings SET value='0' WHERE key='fullscreen'"));
    REQUIRE(load(d.handle()).mode == DisplayMode::Windowed);
}

TEST_CASE("load falls back to Windowed on a corrupt display_mode") {
    const Db d = db();
    QSqlQuery q(d.handle());
    q.exec(QStringLiteral("INSERT INTO settings(key,value) VALUES('display_mode','garbage')"));
    REQUIRE(load(d.handle()).mode == DisplayMode::Windowed);
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
    REQUIRE(s.mode == DisplayMode::Fullscreen);  // legacy fullscreen bool -> mode
    REQUIRE_FALSE(QFile::exists(path));  // legacy file deleted after import
}

TEST_CASE("import maps a missing fullscreen to Windowed") {
    const Db d = db();
    const QString path = QDir::tempPath() + QStringLiteral("/denso_import_nofs.json");
    write_file(path, R"({"width":1280,"height":720})");

    import_legacy(d.handle(), path);

    const Settings s = load(d.handle());
    REQUIRE(s.mode == DisplayMode::Windowed);
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
    save(d.handle(), Settings{800, 600, true, DisplayMode::Windowed});
    save(d.handle(), Settings{1920, 1080, false, DisplayMode::Fullscreen});
    const Settings back = load(d.handle());
    REQUIRE(back.width == 1920);
    REQUIRE(back.height == 1080);
    REQUIRE_FALSE(back.dark);
    REQUIRE(back.mode == DisplayMode::Fullscreen);
}
