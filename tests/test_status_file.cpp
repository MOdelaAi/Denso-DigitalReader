#include <catch2/catch_test_macros.hpp>
#include "health/status_file.h"
#include <QTemporaryDir>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QStringList>
#include <optional>
#include <vector>
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

// ── Zone inhibit ONSETS (Zone Runtime Overlay) ───────────────────────────────
//
// The onset is an EVENT batch, deliberately separate from the `inhibited_zones`
// CURRENT-STATE array. It needs its own representation because the aggregator
// keeps an owed alarm alive across expiry (zone_aggregator.cpp:104-111): an
// escalated zone that then goes silent is erased from the projection while its
// alarm is still owed, so a state-derived array alone would silently drop it.

namespace {

QJsonObject write_and_read(const QString& path,
                           const std::vector<health::ZoneInhibitRecord>& onsets) {
    health::IntegrityVerdict v;
    REQUIRE(health::write_status_file(path, v, {}, {}, {}, std::nullopt, std::nullopt,
                                      onsets));
    QFile f(path);
    REQUIRE(f.open(QIODevice::ReadOnly));
    return QJsonDocument::fromJson(f.readAll()).object();
}

} // namespace

// REQUIREMENT 7 + 8 + 9: the onset reaches status output carrying camera_id,
// zone_no and a stable reason string.
TEST_CASE("status.json carries a zone inhibit onset with BOTH identifiers",
          "[status_file][zone_runtime]") {
    QTemporaryDir tmp; REQUIRE(tmp.isValid());
    const QString p = QDir(tmp.path()).filePath("st_onset.json");
    const auto o = write_and_read(p, {{12, 3, QStringLiteral("hold_timeout")}});

    const auto arr = o.value("zone_inhibit_onsets").toArray();
    REQUIRE(arr.size() == 1);
    const auto rec = arr[0].toObject();
    // Id as a STRING, matching the existing precedent for issue camera ids:
    // QJsonValue is a double, so an id above 2^53 would lose precision.
    CHECK(rec.value("camera_id").toString() == QStringLiteral("12"));
    CHECK(rec.value("zone_no").toInt() == 3);
    CHECK(rec.value("reason").toString() == QStringLiteral("hold_timeout"));
}

// REQUIREMENT 4: a zone number alone is ambiguous across cameras. Two cameras
// claiming one zone number must stay two records, never collapse into one.
TEST_CASE("status.json keeps same-zone onsets from different cameras distinct",
          "[status_file][zone_runtime]") {
    QTemporaryDir tmp; REQUIRE(tmp.isValid());
    const QString p = QDir(tmp.path()).filePath("st_onset2.json");
    const auto o = write_and_read(p, {{8, 1, QStringLiteral("hold_timeout")},
                                      {9, 1, QStringLiteral("hold_timeout")}});

    const auto arr = o.value("zone_inhibit_onsets").toArray();
    REQUIRE(arr.size() == 2);
    QStringList ids;
    for (const auto& v : arr) {
        CHECK(v.toObject().value("zone_no").toInt() == 1);
        ids << v.toObject().value("camera_id").toString();
    }
    ids.sort();
    CHECK(ids == QStringList{QStringLiteral("8"), QStringLiteral("9")});
}

// REQUIREMENT 16: the format stays backward-compatible. With no onset drained
// the key is OMITTED entirely, so every historical document is byte-for-byte
// what it has always been — the same rule `policy_reason` already follows.
TEST_CASE("status.json omits the onset array when no onset was drained",
          "[status_file][zone_runtime]") {
    QTemporaryDir tmp; REQUIRE(tmp.isValid());
    const QString p = QDir(tmp.path()).filePath("st_noonset.json");
    health::IntegrityVerdict v;
    REQUIRE(health::write_status_file(p, v, {}, {}, {}));  // historical 5-arg call
    QFile f(p); REQUIRE(f.open(QIODevice::ReadOnly));
    const auto o = QJsonDocument::fromJson(f.readAll()).object();

    CHECK_FALSE(o.contains("zone_inhibit_onsets"));
    // Pin the whole historical key set, so an additive field can never quietly
    // become an unconditional one.
    QStringList keys = o.keys();
    keys.sort();
    CHECK(keys == QStringList{QStringLiteral("blockers"), QStringLiteral("camera_causes"),
                              QStringLiteral("held_zones"), QStringLiteral("inhibited_zones"),
                              QStringLiteral("issues"), QStringLiteral("status")});
    // An explicitly EMPTY batch must behave identically to no batch at all.
    const QString p2 = QDir(tmp.path()).filePath("st_emptybatch.json");
    REQUIRE(health::write_status_file(p2, v, {}, {}, {}, std::nullopt, std::nullopt, {}));
    QFile f2(p2); REQUIRE(f2.open(QIODevice::ReadOnly));
    CHECK_FALSE(QJsonDocument::fromJson(f2.readAll()).object().contains("zone_inhibit_onsets"));
}

// REQUIREMENT 5 + 10: diagnostics are credential-safe. The record can only ever
// carry two integers and a fixed reason token, so no camera URL, username or
// password can reach the file through this path.
TEST_CASE("status.json onset output exposes no credential material",
          "[status_file][zone_runtime]") {
    QTemporaryDir tmp; REQUIRE(tmp.isValid());
    const QString p = QDir(tmp.path()).filePath("st_redact.json");
    write_and_read(p, {{12, 3, QStringLiteral("hold_timeout")}});

    QFile f(p); REQUIRE(f.open(QIODevice::ReadOnly));
    const QString text = QString::fromUtf8(f.readAll());
    for (const char* needle : {"rtsp://", "://", "password", "username", "admin",
                               "token", "@"}) {
        INFO("leaked '" << needle << "'");
        CHECK_FALSE(text.contains(QString::fromLatin1(needle)));
    }
}
