#include <catch2/catch_test_macros.hpp>
#include "health/status_file.h"
#include <QTemporaryDir>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
using namespace denso;

TEST_CASE("status.json: writes a parseable document", "[status_file]") {
    QTemporaryDir tmp; REQUIRE(tmp.isValid());
    const QString path = QDir(tmp.path()).filePath("status.json");
    health::IntegrityVerdict v;
    v.status = health::Readiness::Degraded;
    v.issues.push_back({health::ZoneIssue::Kind::EngineMissing, 1, "gone.engine"});
    REQUIRE(health::write_status_file(path, v, {{1, 0x08}}, {5}, {9}));

    QFile f(path); REQUIRE(f.open(QIODevice::ReadOnly));
    const auto doc = QJsonDocument::fromJson(f.readAll());
    REQUIRE(doc.isObject());
    REQUIRE(doc.object()["status"].toString() == "degraded");
    REQUIRE(doc.object()["issues"].toArray()[0].toObject()["reason"].toString()
            == "engine_missing");   // stable string code, not an enum ordinal
    REQUIRE(doc.object()["held_zones"].toArray().size() == 1);
    REQUIRE(doc.object()["inhibited_zones"].toArray().size() == 1);
}

TEST_CASE("status.json: rewriting leaves nothing but the target behind",
          "[status_file]") {
    QTemporaryDir tmp; REQUIRE(tmp.isValid());
    QDir dir(tmp.path());
    const QString path = dir.filePath("status.json");
    health::IntegrityVerdict v;
    REQUIRE(health::write_status_file(path, v, {}, {}, {}));
    REQUIRE(health::write_status_file(path, v, {}, {}, {}));
    // QSaveFile writes a randomly-named sibling then atomically renames; assert the
    // directory holds EXACTLY the target afterwards. "No *.tmp" was too weak —
    // QSaveFile makes no promise about the temp suffix, so residue under any other
    // name would have slipped past it.
    const auto entries = dir.entryList(QDir::Files | QDir::Hidden | QDir::System);
    REQUIRE(entries == QStringList{"status.json"});
}

TEST_CASE("status.json: a write to an unopenable path fails cleanly", "[status_file]") {
    QTemporaryDir tmp; REQUIRE(tmp.isValid());
    // Parent directory does not exist -> QSaveFile::open fails -> false, no file.
    const QString path = QDir(tmp.path()).filePath("no_such_dir/status.json");
    health::IntegrityVerdict v;
    REQUIRE_FALSE(health::write_status_file(path, v, {}, {}, {}));
    REQUIRE_FALSE(QFile::exists(path));
}

TEST_CASE("status.json: a failed write leaves a prior good file intact",
          "[status_file]") {
    QTemporaryDir tmp; REQUIRE(tmp.isValid());
    QDir dir(tmp.path());
    const QString path = dir.filePath("status.json");
    health::IntegrityVerdict good;
    good.status = health::Readiness::Blocked;
    REQUIRE(health::write_status_file(path, good, {}, {}, {}));

    // A subsequent write that cannot open (its parent is the existing file) must
    // return false and must not disturb the already-committed good file — the
    // atomic temp+rename never touches the target until commit succeeds.
    health::IntegrityVerdict other;
    other.status = health::Readiness::Ready;
    REQUIRE_FALSE(health::write_status_file(path + "/inner.json", other, {}, {}, {}));

    QFile f(path); REQUIRE(f.open(QIODevice::ReadOnly));
    REQUIRE(QJsonDocument::fromJson(f.readAll()).object()["status"].toString()
            == "blocked");
}

TEST_CASE("status.json emits mode fields when provided", "[status_file]") {
    QTemporaryDir tmp; REQUIRE(tmp.isValid());
    const QString p = QDir(tmp.path()).filePath("st_mode.json");
    health::IntegrityVerdict v;  // Ready
    REQUIRE(health::write_status_file(p, v, {}, {}, {},
            QStringLiteral("ball_leveler"), true));
    QFile f(p); REQUIRE(f.open(QIODevice::ReadOnly));
    const auto o = QJsonDocument::fromJson(f.readAll()).object();
    CHECK(o.value("mode").toString() == QStringLiteral("ball_leveler"));
    CHECK(o.value("mode_setup_required").toBool() == true);
}

TEST_CASE("status.json omits mode fields when not provided", "[status_file]") {
    QTemporaryDir tmp; REQUIRE(tmp.isValid());
    const QString p = QDir(tmp.path()).filePath("st_nomode.json");
    health::IntegrityVerdict v;
    REQUIRE(health::write_status_file(p, v, {}, {}, {}));  // 5-arg
    QFile f(p); REQUIRE(f.open(QIODevice::ReadOnly));
    const auto o = QJsonDocument::fromJson(f.readAll()).object();
    CHECK_FALSE(o.contains("mode"));
    CHECK_FALSE(o.contains("mode_setup_required"));
}
