// The Areas page's single Save is ONE transaction.
//
// The page writes two things behind one button: the camera's ROI set (polygons,
// zones, number formats) and — when the operator changed it — that camera's
// Digital ROI enhancement level. They were once two separate writes, and the
// reachable failure was ugly and quiet: the level landed, the areas were refused
// and rolled themselves back, the dialog said only "the areas could not be
// written", and the grid was then rebuilt with the NEW level against the OLD
// polygons.
//
// So the property every case here defends is a disjunction with no third term:
//
//     everything persisted        OR        nothing persisted
//
// The failure paths are driven through the AUTHORITATIVE refusals — a zone that
// another camera owns, a zone out of range, a duplicate inside one save — rather
// than through an injected fault, because those are the refusals that actually
// happen and they are the ones that must unwind the enhancement with them.
#include <catch2/catch_test_macros.hpp>

#include "camera/camera.h"
#include "camera/repo.h"
#include "camera/roi_enhancement.h"
#include "db/db.h"

#include <QSqlQuery>
#include <QString>
#include <QVariant>

#include <optional>
#include <string>
#include <vector>

using denso::camera::Camera;
using denso::camera::CameraArea;
using denso::camera::Point;
using denso::camera::ImageEnhancement;
using denso::camera::RoiEnhancement;
using denso::camera::save_areas_and_enhancement;
using denso::db::run_migrations;

namespace {

Camera usb_camera(const std::string& name, uint32_t index) {
    Camera c;
    c.name = name;
    c.camera_type = "usb";
    c.index = index;
    c.width = 1280;
    c.height = 720;
    c.fps = 30;
    c.active = true;
    c.setup_complete = true;
    return c;
}

CameraArea area(const std::string& name, std::optional<int> zone, int decimals,
                float ox) {
    CameraArea a;
    a.name = name;
    a.zone = zone;
    a.decimal_places = decimals;
    a.points = {Point{ox, 0.1f}, Point{ox + 0.2f, 0.1f}, Point{ox + 0.2f, 0.4f},
                Point{ox, 0.4f}};
    return a;
}

/// The stored bundle, read straight out of SQLite — several cases here are about
/// which integers are on disk, and going through the parser would hide them.
std::vector<int> stored_bundle(const QSqlDatabase& db, int64_t id) {
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "SELECT img_enh_enabled, img_enh_local_contrast, img_enh_brightness, "
        "img_enh_contrast, img_enh_gamma, img_enh_saturation "
        "FROM camera WHERE id = ?"));
    q.addBindValue(static_cast<qlonglong>(id));
    REQUIRE(q.exec());
    REQUIRE(q.next());
    std::vector<int> out;
    for (int i = 0; i < 6; ++i) out.push_back(q.value(i).toInt());
    return out;
}

/// The bundle the fixture starts from, and the one every failure case must find
/// unchanged afterwards.
ImageEnhancement seeded() {
    ImageEnhancement e;
    e.enabled = true;
    e.local_contrast = RoiEnhancement::Medium;
    e.brightness = 12;
    e.contrast = -8;
    e.gamma = 140;
    e.saturation = 20;
    return e;
}

/// A DIFFERENT bundle — every field moved — so a partial write cannot pass by
/// accidentally matching on the fields nobody changed.
ImageEnhancement retuned() {
    ImageEnhancement e;
    e.enabled = true;
    e.local_contrast = RoiEnhancement::High;
    e.brightness = -44;
    e.contrast = 55;
    e.gamma = 210;
    e.saturation = -66;
    return e;
}

int review_flag(const QSqlDatabase& db, int64_t id) {
    QSqlQuery q(db);
    q.prepare(QStringLiteral("SELECT areas_need_review FROM camera WHERE id = ?"));
    q.addBindValue(static_cast<qlonglong>(id));
    REQUIRE(q.exec());
    REQUIRE(q.next());
    return q.value(0).toInt();
}

/// Rows for ONE camera. Scoped deliberately: several cases below seed a SECOND
/// camera to own the zone that makes the save clash, so a table-wide count would
/// be measuring the fixture rather than the rollback.
int area_rows(const QSqlDatabase& db, int64_t camera_id) {
    QSqlQuery q(db);
    q.prepare(QStringLiteral("SELECT COUNT(*) FROM camera_area WHERE camera_id = ?"));
    q.addBindValue(static_cast<qlonglong>(camera_id));
    REQUIRE(q.exec());
    REQUIRE(q.next());
    return q.value(0).toInt();
}

/// A camera with one saved area at Medium — the "before" state every failure
/// case is measured against.
struct Fixture {
    std::optional<denso::db::Db> db;
    int64_t cam = 0;

    Fixture() {
        db = denso::db::Db::open_in_memory();
        REQUIRE(db.has_value());
        REQUIRE(run_migrations(h()));

        Camera c = usb_camera("Line 1", 0);
        c.image_enhance = seeded();
        const auto id = denso::camera::insert(h(), c);
        REQUIRE(id.has_value());
        cam = *id;
        REQUIRE(denso::camera::replace_areas(h(), cam, {area("original", 3, 1, 0.1f)}));
        REQUIRE(denso::camera::get(h(), cam)->image_enhance == seeded());
        REQUIRE(denso::camera::areas_for(h(), cam).size() == 1);
    }
    QSqlDatabase h() { return db->handle(); }

    /// Everything the save could have touched, as it stands on disk.
    void expect_untouched() {
        // EVERY field, not just one: a partial write that moved only some of the
        // six would otherwise slip through on the ones it left alone.
        CHECK(stored_bundle(h(), cam) == std::vector<int>{1, 2, 12, -8, 140, 20});
        CHECK(denso::camera::get(h(), cam)->image_enhance == seeded());
        const auto areas = denso::camera::areas_for(h(), cam);
        REQUIRE(areas.size() == 1);                         // still the one area
        CHECK(areas[0].name == "original");
        REQUIRE(areas[0].zone.has_value());
        CHECK(*areas[0].zone == 3);
        CHECK(areas[0].decimal_places == 1);
        CHECK(area_rows(h(), cam) == 1);                    // and no strays
    }
};

}  // namespace

// ─── 1. The success paths ────────────────────────────────────────────────────

TEST_CASE("both the areas and the enhancement land together",
          "[areas_atomicity]") {
    Fixture f;
    REQUIRE(save_areas_and_enhancement(
        f.h(), f.cam,
        {area("meter", 7, 2, 0.2f), area("gauge", 8, 0, 0.5f)},
        retuned()));

    CHECK(denso::camera::get(f.h(), f.cam)->image_enhance == retuned());
    CHECK(stored_bundle(f.h(), f.cam) == std::vector<int>{1, 3, -44, 55, 210, -66});
    const auto areas = denso::camera::areas_for(f.h(), f.cam);
    REQUIRE(areas.size() == 2);
    CHECK(areas[0].name == "meter");
    CHECK(*areas[0].zone == 7);
    CHECK(areas[0].decimal_places == 2);
    CHECK(areas[1].name == "gauge");
    CHECK(*areas[1].zone == 8);
    CHECK(areas[1].decimal_places == 0);
}

TEST_CASE("an unchanged strength issues no camera-row level write",
          "[areas_atomicity]") {
    // Disengaged is not "write the same value again": it must be byte-for-byte
    // the old area-only save, so an ordinary save cannot fail in a new way.
    Fixture f;
    REQUIRE(save_areas_and_enhancement(f.h(), f.cam, {area("meter", 7, 2, 0.2f)},
                                       std::nullopt));
    CHECK(denso::camera::get(f.h(), f.cam)->image_enhance == seeded());  // untouched
    const auto areas = denso::camera::areas_for(f.h(), f.cam);
    REQUIRE(areas.size() == 1);
    CHECK(areas[0].name == "meter");
}

TEST_CASE("an empty area set with a level change still saves both",
          "[areas_atomicity]") {
    Fixture f;
    REQUIRE(save_areas_and_enhancement(f.h(), f.cam, {}, retuned()));
    CHECK(denso::camera::get(f.h(), f.cam)->image_enhance == retuned());
    CHECK(denso::camera::areas_for(f.h(), f.cam).empty());
}

// ─── 2. The failure paths: the enhancement must NOT escape ───────────────────

TEST_CASE("a zone owned by another camera rolls the enhancement back too",
          "[areas_atomicity]") {
    // THE case this function exists for. The level write happens first inside the
    // transaction; the clash is discovered afterwards, in the area logic. If the
    // two were still separate writes, the level would be on disk right now.
    Fixture f;
    const auto other = denso::camera::insert(f.h(), usb_camera("Line 2", 1));
    REQUIRE(other.has_value());
    REQUIRE(denso::camera::replace_areas(f.h(), *other, {area("theirs", 42, 0, 0.6f)}));

    CHECK_FALSE(save_areas_and_enhancement(
        f.h(), f.cam, {area("mine", 42, 0, 0.2f)}, retuned()));
    f.expect_untouched();
    // …and the other camera's row is untouched as well.
    CHECK(denso::camera::areas_for(f.h(), *other).size() == 1);
}

TEST_CASE("a refusal DISCOVERED PART-WAY still unwinds everything",
          "[areas_atomicity]") {
    // The first area inserts cleanly and the second is refused, so at the moment
    // of failure the transaction already holds a level UPDATE, a DELETE of the
    // old set and one INSERT. All three have to go.
    Fixture f;
    const auto other = denso::camera::insert(f.h(), usb_camera("Line 2", 1));
    REQUIRE(other.has_value());
    REQUIRE(denso::camera::replace_areas(f.h(), *other, {area("theirs", 42, 0, 0.6f)}));

    CHECK_FALSE(save_areas_and_enhancement(
        f.h(), f.cam,
        {area("first-ok", 7, 0, 0.2f), area("second-clashes", 42, 0, 0.4f)},
        retuned()));
    f.expect_untouched();
}

TEST_CASE("an out-of-range zone rolls the enhancement back", "[areas_atomicity]") {
    Fixture f;
    CHECK_FALSE(save_areas_and_enhancement(
        f.h(), f.cam, {area("bad", 500, 0, 0.2f)}, retuned()));
    f.expect_untouched();

    CHECK_FALSE(save_areas_and_enhancement(
        f.h(), f.cam, {area("zero", 0, 0, 0.2f)}, retuned()));
    f.expect_untouched();
}

TEST_CASE("a zone duplicated inside one save rolls the enhancement back",
          "[areas_atomicity]") {
    Fixture f;
    CHECK_FALSE(save_areas_and_enhancement(
        f.h(), f.cam, {area("a", 9, 0, 0.2f), area("b", 9, 0, 0.5f)},
        retuned()));
    f.expect_untouched();
}

TEST_CASE("a zone the BALL mode owns rolls the enhancement back",
          "[areas_atomicity]") {
    // The cross-mode half of the ownership check. One namespace, one authority —
    // and it must unwind the enhancement exactly like the cross-camera half.
    Fixture f;
    const auto ball = denso::camera::insert(f.h(), usb_camera("Tank", 2));
    REQUIRE(ball.has_value());
    QSqlQuery z(f.h());
    z.prepare(QStringLiteral(
        "INSERT INTO ball_level_zone (camera_id, zone_no, conf, rect_x, rect_y, "
        "rect_w, rect_h, y_100, y_0, hold_ms) "
        "VALUES (?, 55, 0.5, 0.1, 0.1, 0.5, 0.5, 0.2, 0.8, 500)"));
    z.addBindValue(static_cast<qlonglong>(*ball));
    REQUIRE(z.exec());

    CHECK_FALSE(save_areas_and_enhancement(
        f.h(), f.cam, {area("mine", 55, 0, 0.2f)}, retuned()));
    f.expect_untouched();
}

TEST_CASE("a write error inside the transaction rolls the enhancement back",
          "[areas_atomicity]") {
    // Not a validation refusal but a genuine statement failure — the shape a
    // busy/locked connection or a damaged schema takes. Removing the table makes
    // it deterministic; what is under test is the unwinding, not the cause.
    Fixture f;
    const std::vector<int> before = stored_bundle(f.h(), f.cam);
    REQUIRE(QSqlQuery(f.h()).exec(QStringLiteral("DROP TABLE camera_area")));

    CHECK_FALSE(save_areas_and_enhancement(
        f.h(), f.cam, {area("meter", 7, 0, 0.2f)}, retuned()));
    CHECK(stored_bundle(f.h(), f.cam) == before);   // no field escaped
}

TEST_CASE("EVERY field of the bundle is rolled back, not just some",
          "[areas_atomicity]") {
    // The bundle is six columns written by one statement. A rollback that
    // recovered, say, the enabled flag but left brightness moved would be worse
    // than no rollback at all: the operator would see a camera that says it is
    // untouched and reads differently. Each field is asserted individually.
    Fixture f;
    const auto other = denso::camera::insert(f.h(), usb_camera("Line 2", 1));
    REQUIRE(other.has_value());
    REQUIRE(denso::camera::replace_areas(f.h(), *other, {area("theirs", 42, 0, 0.6f)}));

    CHECK_FALSE(save_areas_and_enhancement(
        f.h(), f.cam, {area("mine", 42, 0, 0.2f)}, retuned()));

    const auto after = denso::camera::get(f.h(), f.cam);
    REQUIRE(after.has_value());
    CHECK(after->image_enhance.enabled == seeded().enabled);
    CHECK(after->image_enhance.local_contrast == seeded().local_contrast);
    CHECK(after->image_enhance.brightness == seeded().brightness);
    CHECK(after->image_enhance.contrast == seeded().contrast);
    CHECK(after->image_enhance.gamma == seeded().gamma);
    CHECK(after->image_enhance.saturation == seeded().saturation);
    // And nothing unrelated on the row moved either.
    CHECK(after->name == "Line 1");
    CHECK(after->width == 1280);
    CHECK(after->rotation == 0);
}

TEST_CASE("the targeted update touches no unrelated camera column",
          "[areas_atomicity]") {
    // save_areas_and_enhancement deliberately does NOT go through camera::update,
    // which rewrites the whole row from an in-memory Camera. A stale draft must
    // not be able to overwrite a name or an RTSP URL edited elsewhere.
    Fixture f;
    Camera before = *denso::camera::get(f.h(), f.cam);

    REQUIRE(save_areas_and_enhancement(f.h(), f.cam, {area("meter", 7, 0, 0.2f)},
                                       retuned()));
    const Camera after = *denso::camera::get(f.h(), f.cam);

    CHECK(after.image_enhance == retuned());   // the bundle moved…
    // …and nothing else did.
    CHECK(after.name == before.name);
    CHECK(after.camera_type == before.camera_type);
    CHECK(after.index == before.index);
    CHECK(after.width == before.width);
    CHECK(after.height == before.height);
    CHECK(after.fps == before.fps);
    CHECK(after.pitch == before.pitch);
    CHECK(after.roll == before.roll);
    CHECK(after.rotation == before.rotation);
    CHECK(after.active == before.active);
    CHECK(after.setup_complete == before.setup_complete);
}

// ─── 3. areas_need_review clears only on a COMPLETE save ─────────────────────

TEST_CASE("the review quarantine survives a failed save and clears on a good one",
          "[areas_atomicity]") {
    // Saving the set IS the verification, so the flag must not be cleared by a
    // save that did not land — that would resume zone reporting against geometry
    // nobody re-verified.
    Fixture f;
    REQUIRE(denso::camera::set_areas_need_review(f.h(), f.cam, true));
    REQUIRE(review_flag(f.h(), f.cam) == 1);

    const auto other = denso::camera::insert(f.h(), usb_camera("Line 2", 1));
    REQUIRE(other.has_value());
    REQUIRE(denso::camera::replace_areas(f.h(), *other, {area("theirs", 42, 0, 0.6f)}));

    CHECK_FALSE(save_areas_and_enhancement(
        f.h(), f.cam, {area("mine", 42, 0, 0.2f)}, retuned()));
    CHECK(review_flag(f.h(), f.cam) == 1);   // still quarantined
    f.expect_untouched();

    REQUIRE(save_areas_and_enhancement(f.h(), f.cam, {area("mine", 7, 0, 0.2f)},
                                       retuned()));
    CHECK(review_flag(f.h(), f.cam) == 0);   // cleared by the save that landed
    CHECK(denso::camera::get(f.h(), f.cam)->image_enhance == retuned());
}

// ─── 4. The area fields stay atomic with each other, as before ───────────────

TEST_CASE("geometry, zone and decimal format still move as one set",
          "[areas_atomicity]") {
    Fixture f;
    // A three-area save where the last one is refused: none of the three may
    // land, and the previously stored set must be intact.
    CHECK_FALSE(save_areas_and_enhancement(
        f.h(), f.cam,
        {area("a", 11, 3, 0.1f), area("b", 12, 2, 0.4f), area("c", 900, 1, 0.7f)},
        std::nullopt));
    f.expect_untouched();

    // The same set with the bad zone corrected persists every field of every area.
    REQUIRE(save_areas_and_enhancement(
        f.h(), f.cam,
        {area("a", 11, 3, 0.1f), area("b", 12, 2, 0.4f), area("c", 13, 1, 0.7f)},
        std::nullopt));
    const auto areas = denso::camera::areas_for(f.h(), f.cam);
    REQUIRE(areas.size() == 3);
    CHECK(*areas[0].zone == 11);
    CHECK(areas[0].decimal_places == 3);
    CHECK(areas[0].points.size() == 4);
    CHECK(*areas[2].zone == 13);
    CHECK(areas[2].decimal_places == 1);
}

// ─── 5. replace_areas is unchanged by the refactor ───────────────────────────

TEST_CASE("replace_areas still owns its own transaction and its own refusals",
          "[areas_atomicity]") {
    // The shared area logic was factored out from under it; this is the guard
    // that factoring did not change what the area-only entry point does.
    Fixture f;
    const auto other = denso::camera::insert(f.h(), usb_camera("Line 2", 1));
    REQUIRE(other.has_value());
    REQUIRE(denso::camera::replace_areas(f.h(), *other, {area("theirs", 42, 0, 0.6f)}));

    CHECK_FALSE(denso::camera::replace_areas(f.h(), f.cam,
                                             {area("mine", 42, 0, 0.2f)}));
    f.expect_untouched();   // including the level, which it must never touch

    CHECK_FALSE(denso::camera::replace_areas(f.h(), f.cam,
                                             {area("bad", 500, 0, 0.2f)}));
    f.expect_untouched();

    REQUIRE(denso::camera::replace_areas(f.h(), f.cam, {area("ok", 21, 0, 0.2f)}));
    CHECK(denso::camera::get(f.h(), f.cam)->image_enhance == seeded());  // not its business
    CHECK(denso::camera::areas_for(f.h(), f.cam).size() == 1);
}
