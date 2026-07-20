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

TEST_CASE("status.json: rewriting leaves no temp file behind", "[status_file]") {
    QTemporaryDir tmp; REQUIRE(tmp.isValid());
    QDir dir(tmp.path());
    const QString path = dir.filePath("status.json");
    health::IntegrityVerdict v;
    REQUIRE(health::write_status_file(path, v, {}, {}, {}));
    REQUIRE(health::write_status_file(path, v, {}, {}, {}));
    // Atomic write = temp + rename; a leftover temp would mean a partial write
    // could be observed after a crash.
    REQUIRE(dir.entryList({"*.tmp"}, QDir::Files).isEmpty());
}
