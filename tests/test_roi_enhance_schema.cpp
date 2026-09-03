// Schema v18 — the per-camera Image Enhancement columns: the v17 -> v18
// migration, the repository round trip, and what a mode switch does to them.
//
// The single fact most of these cases defend: an UPGRADE MUST NOT TURN IMAGE
// PROCESSING ON. Every camera on every appliance in the field comes up DISABLED
// with every control at its neutral value, which is byte-for-byte the pipeline it
// has today, and only an explicit operator action moves it. The second fact is
// the opposite of the first: once an operator HAS tuned a camera, nothing may
// quietly take it away — not disabling the feature, and not a trip through the
// Ball Leveler and back.
#include <catch2/catch_test_macros.hpp>

#include "camera/camera.h"
#include "camera/repo.h"
#include "camera/roi_enhancement.h"
#include "db/db.h"
#include "mode/config.h"
#include "mode/mode.h"
#include "mode/reset.h"

#include <QDir>
#include <QSqlError>
#include <QSqlQuery>
#include <QString>
#include <QTemporaryDir>
#include <QVariant>

#include <optional>
#include <string>
#include <utility>
#include <vector>

using denso::camera::Camera;
using denso::camera::ImageEnhancement;
using denso::camera::RoiEnhancement;
using denso::db::run_migrations;

namespace {

/// The six v18 columns, in the order this file reads them everywhere.
const char* const kEnhanceColumns[] = {
    "img_enh_enabled",  "img_enh_local_contrast", "img_enh_brightness",
    "img_enh_contrast", "img_enh_gamma",          "img_enh_saturation",
};

struct Scratch {
    QTemporaryDir dir;
    QString path;
    Scratch() : path(QDir(dir.path()).filePath(QStringLiteral("denso.db"))) {}
};

Camera usb_camera(const std::string& name) {
    Camera c;
    c.name = name;
    c.camera_type = "usb";
    c.index = 0;
    c.width = 1280;
    c.height = 720;
    c.fps = 30;
    c.active = true;
    c.setup_complete = true;
    return c;
}

/// A fully tuned, enabled bundle — the thing an operator would lose if any of
/// these guarantees broke.
ImageEnhancement tuned() {
    ImageEnhancement e;
    e.enabled = true;
    e.local_contrast = RoiEnhancement::High;
    e.brightness = 37;
    e.contrast = -22;
    e.gamma = 175;
    e.saturation = 61;
    return e;
}

/// The RAW stored integers, read straight out of SQLite rather than through the
/// repository — several of these cases are about which integers are on disk, and
/// going through the parser would hide the answer.
std::vector<int> stored_row(const QSqlDatabase& db, const char* name) {
    QSqlQuery q(db);
    q.prepare(QStringLiteral("SELECT img_enh_enabled, img_enh_local_contrast, "
                             "img_enh_brightness, img_enh_contrast, "
                             "img_enh_gamma, img_enh_saturation "
                             "FROM camera WHERE name = ?"));
    q.addBindValue(QString::fromLatin1(name));
    REQUIRE(q.exec());
    REQUIRE(q.next());
    std::vector<int> out;
    for (int i = 0; i < 6; ++i) out.push_back(q.value(i).toInt());
    return out;
}

QString table_ddl(const QSqlDatabase& db, const char* table) {
    QSqlQuery q(db);
    q.prepare(
        QStringLiteral("SELECT sql FROM sqlite_master WHERE type='table' AND name=?"));
    q.addBindValue(QString::fromLatin1(table));
    REQUIRE(q.exec());
    REQUIRE(q.next());
    return q.value(0).toString();
}

int row_count(const QSqlDatabase& db, const char* table) {
    QSqlQuery q(db);
    if (!q.exec(QStringLiteral("SELECT COUNT(*) FROM %1").arg(QLatin1String(table))) ||
        !q.next()) {
        return -1;
    }
    return q.value(0).toInt();
}

/// Write one column straight through SQL, bypassing the repository, so the
/// verdict is the DATABASE's alone. True when the row landed.
bool raw_accepted(const QSqlDatabase& db, int64_t id, const char* column, int value) {
    QSqlQuery q(db);
    q.prepare(QStringLiteral("UPDATE camera SET %1 = ? WHERE id = ?")
                  .arg(QLatin1String(column)));
    q.addBindValue(value);
    q.addBindValue(static_cast<qlonglong>(id));
    return q.exec();
}

}  // namespace

// ─── 1. The supported version ────────────────────────────────────────────────

TEST_CASE("the supported schema version is 18", "[roi_enhance][schema]") {
    CHECK(denso::db::supported_schema_version() == 18);
}

// ─── 2. v17 -> v18 ───────────────────────────────────────────────────────────

TEST_CASE("v17 -> v18 adds the bundle and every existing camera lands disabled "
          "and neutral", "[roi_enhance][schema][migration]") {
    Scratch s;
    int64_t cam_a = 0;
    int64_t cam_b = 0;

    {
        auto db = denso::db::Db::open(s.path);
        REQUIRE(db.has_value());
        REQUIRE(run_migrations(db->handle()));

        const auto a = denso::camera::insert(db->handle(), usb_camera("Line 1"));
        const auto b = denso::camera::insert(db->handle(), usb_camera("Line 2"));
        REQUIRE(a.has_value());
        REQUIRE(b.has_value());
        cam_a = *a;
        cam_b = *b;

        // Rewind to the pre-feature world: drop the columns this migration adds,
        // then set the version back. SQLite 3.35+ supports DROP COLUMN; if this
        // build did not, the case would fail loudly rather than silently testing
        // nothing.
        for (const char* col : kEnhanceColumns) {
            QSqlQuery drop(db->handle());
            REQUIRE(drop.exec(QStringLiteral("ALTER TABLE camera DROP COLUMN %1")
                                  .arg(QLatin1String(col))));
        }
        for (const char* col : kEnhanceColumns) {
            REQUIRE_FALSE(table_ddl(db->handle(), "camera")
                              .contains(QString::fromLatin1(col)));
        }
        REQUIRE(QSqlQuery(db->handle())
                    .exec(QStringLiteral("PRAGMA user_version = 17")));
    }

    {
        auto db = denso::db::Db::open(s.path);
        REQUIRE(db.has_value());
        REQUIRE(denso::db::read_user_version(db->handle()).value_or(-1) == 17);
        REQUIRE(run_migrations(db->handle()));
        CHECK(denso::db::read_user_version(db->handle()).value_or(-1) == 18);

        // THE upgrade guarantee: disabled, and every control neutral.
        const std::vector<int> want{0, 0, 0, 0, 100, 0};
        CHECK(stored_row(db->handle(), "Line 1") == want);
        CHECK(stored_row(db->handle(), "Line 2") == want);

        // …and that decodes to a bundle with no effect at all.
        const auto loaded = denso::camera::get(db->handle(), cam_a);
        REQUIRE(loaded.has_value());
        CHECK(loaded->image_enhance == denso::camera::neutral_enhancement());
        CHECK_FALSE(denso::camera::has_effect(loaded->image_enhance));

        // Nothing was dropped or rebuilt to get there.
        CHECK(row_count(db->handle(), "camera") == 2);
        const QString ddl = table_ddl(db->handle(), "camera");
        for (const char* col : kEnhanceColumns) {
            CHECK(ddl.contains(QString::fromLatin1(col)));
        }
        CHECK(ddl.contains(QStringLiteral("DEFAULT 100")));   // gamma's neutral
        QSqlQuery scaffolding(db->handle());
        REQUIRE(scaffolding.exec(QStringLiteral(
            "SELECT COUNT(*) FROM sqlite_master WHERE name LIKE '%_v18'")));
        REQUIRE(scaffolding.next());
        CHECK(scaffolding.value(0).toInt() == 0);

        // Every CHECK is live, from the database itself.
        CHECK(raw_accepted(db->handle(), cam_b, "img_enh_enabled", 1));
        CHECK_FALSE(raw_accepted(db->handle(), cam_b, "img_enh_enabled", 2));
        CHECK(raw_accepted(db->handle(), cam_b, "img_enh_local_contrast", 3));
        CHECK_FALSE(raw_accepted(db->handle(), cam_b, "img_enh_local_contrast", 4));
        CHECK(raw_accepted(db->handle(), cam_b, "img_enh_brightness", -100));
        CHECK_FALSE(raw_accepted(db->handle(), cam_b, "img_enh_brightness", -101));
        CHECK(raw_accepted(db->handle(), cam_b, "img_enh_contrast", 100));
        CHECK_FALSE(raw_accepted(db->handle(), cam_b, "img_enh_contrast", 101));
        CHECK(raw_accepted(db->handle(), cam_b, "img_enh_gamma", 50));
        CHECK_FALSE(raw_accepted(db->handle(), cam_b, "img_enh_gamma", 49));
        CHECK_FALSE(raw_accepted(db->handle(), cam_b, "img_enh_gamma", 301));
        CHECK(raw_accepted(db->handle(), cam_b, "img_enh_saturation", 0));
        CHECK_FALSE(raw_accepted(db->handle(), cam_b, "img_enh_saturation", 999));

        QSqlQuery ic(db->handle());
        REQUIRE(ic.exec(QStringLiteral("PRAGMA integrity_check")));
        REQUIRE(ic.next());
        CHECK(ic.value(0).toString() == QStringLiteral("ok"));
    }

    // Re-running must not migrate again, and must not reset anyone's tuning.
    {
        auto db = denso::db::Db::open(s.path);
        REQUIRE(db.has_value());
        Camera c = *denso::camera::get(db->handle(), cam_a);
        c.image_enhance = tuned();
        REQUIRE(denso::camera::update(db->handle(), c));

        REQUIRE(run_migrations(db->handle()));
        CHECK(denso::db::read_user_version(db->handle()).value_or(-1) == 18);
        // add_column probes the schema first, so a re-run is a no-op rather than
        // a "duplicate column name" failure — the property that makes an
        // interrupted migration resumable.
        CHECK(denso::camera::get(db->handle(), cam_a)->image_enhance == tuned());
        CHECK(row_count(db->handle(), "camera") == 2);
    }
}

TEST_CASE("an interrupted v18 resumes: the columns are already there",
          "[roi_enhance][schema][migration]") {
    // user_version is stamped ONCE, at the very end of the chain, so a power cut
    // between an ALTER and that stamp leaves some columns added and the version
    // behind. The next boot must survive it — including the case where only SOME
    // of the six landed.
    Scratch s;
    auto db = denso::db::Db::open(s.path);
    REQUIRE(db.has_value());
    REQUIRE(run_migrations(db->handle()));
    REQUIRE(denso::camera::insert(db->handle(), usb_camera("Line 1")).has_value());

    // Drop only the last three, then rewind: the shape an interruption leaves.
    for (const char* col :
         {"img_enh_contrast", "img_enh_gamma", "img_enh_saturation"}) {
        QSqlQuery drop(db->handle());
        REQUIRE(drop.exec(QStringLiteral("ALTER TABLE camera DROP COLUMN %1")
                              .arg(QLatin1String(col))));
    }
    REQUIRE(QSqlQuery(db->handle()).exec(QStringLiteral("PRAGMA user_version = 17")));

    REQUIRE(run_migrations(db->handle()));
    CHECK(denso::db::read_user_version(db->handle()).value_or(-1) == 18);
    CHECK(stored_row(db->handle(), "Line 1") == std::vector<int>{0, 0, 0, 0, 100, 0});
}

// ─── 3. The repository round trip ────────────────────────────────────────────

TEST_CASE("a fully tuned bundle survives insert, load and update",
          "[roi_enhance][schema]") {
    auto db = denso::db::Db::open_in_memory();
    REQUIRE(db.has_value());
    REQUIRE(run_migrations(db->handle()));

    Camera c = usb_camera("round trip");
    c.image_enhance = tuned();
    const auto id = denso::camera::insert(db->handle(), c);
    REQUIRE(id.has_value());

    const auto loaded = denso::camera::get(db->handle(), *id);
    REQUIRE(loaded.has_value());
    CHECK(loaded->image_enhance == tuned());
    CHECK(stored_row(db->handle(), "round trip") ==
          std::vector<int>{1, 3, 37, -22, 175, 61});

    // Every level, through update.
    for (const RoiEnhancement level :
         {RoiEnhancement::Off, RoiEnhancement::Low, RoiEnhancement::Medium,
          RoiEnhancement::High}) {
        Camera edited = *denso::camera::get(db->handle(), *id);
        edited.image_enhance.local_contrast = level;
        REQUIRE(denso::camera::update(db->handle(), edited));
        CHECK(denso::camera::get(db->handle(), *id)->image_enhance.local_contrast ==
              level);
    }
}

TEST_CASE("disabling preserves every tuned value on disk",
          "[roi_enhance][schema]") {
    // The master switch is a flag, never an erasure: field calibration can only
    // be redone in front of the real meter, so switching the feature off and on
    // again must give it back exactly.
    auto db = denso::db::Db::open_in_memory();
    REQUIRE(db.has_value());
    REQUIRE(run_migrations(db->handle()));

    Camera c = usb_camera("kept");
    c.image_enhance = tuned();
    const auto id = denso::camera::insert(db->handle(), c);
    REQUIRE(id.has_value());

    Camera off = *denso::camera::get(db->handle(), *id);
    off.image_enhance.enabled = false;
    REQUIRE(denso::camera::update(db->handle(), off));

    const auto reloaded = denso::camera::get(db->handle(), *id);
    REQUIRE(reloaded.has_value());
    CHECK_FALSE(reloaded->image_enhance.enabled);
    CHECK_FALSE(denso::camera::has_effect(reloaded->image_enhance));
    // …and every value is exactly where the operator left it.
    CHECK(reloaded->image_enhance.local_contrast == RoiEnhancement::High);
    CHECK(reloaded->image_enhance.brightness == 37);
    CHECK(reloaded->image_enhance.contrast == -22);
    CHECK(reloaded->image_enhance.gamma == 175);
    CHECK(reloaded->image_enhance.saturation == 61);

    // Turning it back on restores the whole thing.
    Camera on = *reloaded;
    on.image_enhance.enabled = true;
    REQUIRE(denso::camera::update(db->handle(), on));
    CHECK(denso::camera::get(db->handle(), *id)->image_enhance == tuned());
}

TEST_CASE("a fresh camera defaults to disabled and neutral",
          "[roi_enhance][schema]") {
    CHECK(Camera{}.image_enhance == denso::camera::neutral_enhancement());

    auto db = denso::db::Db::open_in_memory();
    REQUIRE(db.has_value());
    REQUIRE(run_migrations(db->handle()));
    REQUIRE(denso::camera::insert(db->handle(), usb_camera("fresh")).has_value());
    CHECK(stored_row(db->handle(), "fresh") == std::vector<int>{0, 0, 0, 0, 100, 0});
}

TEST_CASE("runtime() carries the bundle to the grid", "[roi_enhance][schema]") {
    auto db = denso::db::Db::open_in_memory();
    REQUIRE(db.has_value());
    REQUIRE(run_migrations(db->handle()));

    // Different cameras, different tuning, at the same time — this is the query
    // CameraGrid::reload() actually reads.
    Camera a = usb_camera("A");
    a.image_enhance = tuned();
    Camera b = usb_camera("B");
    b.index = 1;   // disabled and neutral
    Camera c = usb_camera("C");
    c.index = 2;
    c.image_enhance.enabled = true;
    c.image_enhance.brightness = -50;
    REQUIRE(denso::camera::insert(db->handle(), a).has_value());
    REQUIRE(denso::camera::insert(db->handle(), b).has_value());
    REQUIRE(denso::camera::insert(db->handle(), c).has_value());

    const std::vector<Camera> live = denso::camera::runtime(db->handle());
    REQUIRE(live.size() == 3);
    CHECK(live[0].image_enhance == tuned());
    CHECK(live[1].image_enhance == denso::camera::neutral_enhancement());
    CHECK(live[2].image_enhance.brightness == -50);
    CHECK(live[2].image_enhance.enabled);
}

TEST_CASE("hand-edited out-of-range values read back fail-safe",
          "[roi_enhance][schema]") {
    // The column CHECKs block the ordinary routes, but a database can also arrive
    // from a restore or from a build with a wider range. The reader must fail
    // safe rather than adopt whatever it finds.
    auto db = denso::db::Db::open_in_memory();
    REQUIRE(db.has_value());
    REQUIRE(run_migrations(db->handle()));
    Camera c = usb_camera("tampered");
    c.image_enhance = tuned();
    const auto id = denso::camera::insert(db->handle(), c);
    REQUIRE(id.has_value());

    // Reproduce the columns WITHOUT their CHECKs, which is the shape a restored
    // or hand-built database can genuinely have, then plant impossible values.
    for (const char* col : kEnhanceColumns) {
        QSqlQuery drop(db->handle());
        REQUIRE(drop.exec(QStringLiteral("ALTER TABLE camera DROP COLUMN %1")
                              .arg(QLatin1String(col))));
        QSqlQuery add(db->handle());
        REQUIRE(add.exec(QStringLiteral("ALTER TABLE camera ADD COLUMN %1 "
                                        "INTEGER NOT NULL DEFAULT 0")
                             .arg(QLatin1String(col))));
    }
    REQUIRE(raw_accepted(db->handle(), *id, "img_enh_enabled", 7));
    REQUIRE(raw_accepted(db->handle(), *id, "img_enh_local_contrast", 9));
    REQUIRE(raw_accepted(db->handle(), *id, "img_enh_brightness", 5000));
    REQUIRE(raw_accepted(db->handle(), *id, "img_enh_contrast", -5000));
    REQUIRE(raw_accepted(db->handle(), *id, "img_enh_gamma", 9999));
    REQUIRE(raw_accepted(db->handle(), *id, "img_enh_saturation", -9999));

    const auto loaded = denso::camera::get(db->handle(), *id);
    REQUIRE(loaded.has_value());
    CHECK_FALSE(loaded->image_enhance.enabled);   // "exactly 1", so 7 is OFF
    CHECK(loaded->image_enhance.local_contrast == RoiEnhancement::Off);
    CHECK(loaded->image_enhance.brightness == denso::camera::kMaxBrightness);
    CHECK(loaded->image_enhance.contrast == denso::camera::kMinContrast);
    CHECK(loaded->image_enhance.gamma == denso::camera::kMaxGamma);
    CHECK(loaded->image_enhance.saturation == denso::camera::kMinSaturation);
    // The decisive one: a corrupt row cannot switch processing on.
    CHECK_FALSE(denso::camera::has_effect(loaded->image_enhance));
}

TEST_CASE("an out-of-range in-memory bundle is clamped, not refused",
          "[roi_enhance][schema]") {
    // The column CHECKs would reject an out-of-range value and fail the WHOLE
    // camera write. Clamping on the way in means a corrupted in-memory value
    // costs the operator the tuning, never the save.
    auto db = denso::db::Db::open_in_memory();
    REQUIRE(db.has_value());
    REQUIRE(run_migrations(db->handle()));

    Camera c = usb_camera("bogus");
    c.image_enhance.enabled = true;
    c.image_enhance.brightness = 9999;
    c.image_enhance.gamma = -5;
    const auto id = denso::camera::insert(db->handle(), c);
    REQUIRE(id.has_value());                       // the save was NOT lost
    const auto loaded = denso::camera::get(db->handle(), *id);
    REQUIRE(loaded.has_value());
    CHECK(loaded->image_enhance.brightness == denso::camera::kMaxBrightness);
    CHECK(loaded->image_enhance.gamma == denso::camera::kMinGamma);
}

// ─── 4. The mode switch ──────────────────────────────────────────────────────

TEST_CASE("a mode switch preserves the whole bundle in both directions",
          "[roi_enhance][schema][mode]") {
    // The switch is destructive to the mode-owned PROCESSING workspace — areas,
    // model bindings, calibration. The enhancement bundle is not part of that: it
    // is a property of the camera's optics, it lives on the camera row alongside
    // rotation and pitch, and an operator who goes Digital -> Ball -> Digital
    // must get their field calibration back rather than re-derive it in front of
    // the meter.
    auto db = denso::db::Db::open_in_memory();
    REQUIRE(db.has_value());
    REQUIRE(run_migrations(db->handle()));

    Camera c = usb_camera("Line 1");
    c.image_enhance = tuned();
    const auto id = denso::camera::insert(db->handle(), c);
    REQUIRE(id.has_value());

    // Something the switch IS meant to destroy, so the case proves it really ran.
    denso::camera::CameraArea area;
    area.name = "meter";
    area.zone = 4;
    area.points = {{0.1f, 0.1f}, {0.9f, 0.1f}, {0.9f, 0.9f}};
    REQUIRE(denso::camera::replace_areas(db->handle(), *id, {area}));
    REQUIRE(denso::camera::areas_for(db->handle(), *id).size() == 1);

    const auto to_ball = denso::mode::switch_and_reset(
        db->handle(), denso::mode::TargetMode::BallLeveler);
    REQUIRE(to_ball.ok);
    CHECK(denso::camera::areas_for(db->handle(), *id).empty());   // destroyed
    auto after_ball = denso::camera::get(db->handle(), *id);
    REQUIRE(after_ball.has_value());
    CHECK(after_ball->image_enhance == tuned());                  // kept, whole
    CHECK_FALSE(after_ball->setup_complete);                      // the switch ran

    const auto to_digit = denso::mode::switch_and_reset(
        db->handle(), denso::mode::TargetMode::DigitReader);
    REQUIRE(to_digit.ok);
    CHECK(denso::camera::get(db->handle(), *id)->image_enhance == tuned());
}
