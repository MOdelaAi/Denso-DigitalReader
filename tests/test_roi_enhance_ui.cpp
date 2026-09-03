// The Areas step's IMAGE ENHANCEMENT control and its live preview, driven
// through the REAL CameraAreasPage, the REAL RoiCanvas and the REAL
// CameraWizardController against a real database.
//
// The control is per CAMERA but lives on the Areas page, because that is the only
// screen where the operator can see the ROI polygons and judge the result. Two
// consequences are worth stating, because both are load-bearing and neither is
// obvious:
//
//   * The Areas page is UNREACHABLE in ball_leveler — the wizard's fourth step is
//     the level calibration page, and the per-row Areas shortcut routes there
//     too. So "the Digital-only control is not offered in Ball mode" is
//     structural, not a visibility flag someone has to remember to set.
//
//   * The preview and the runtime share ONE authority (ui::enhance_preview ->
//     RoiEnhancer). The operator therefore cannot be shown one transformation
//     while the model receives another.
//
// No camera is contacted: every fixture pushes a generated QImage straight into
// the page as its snapshot.
#include <catch2/catch_test_macros.hpp>

#include "camera/camera.h"
#include "camera/repo.h"
#include "camera/roi_enhance.h"
#include "camera/roi_enhancement.h"
#include "db/db.h"
#include "ui/camera/dialog/add_page.h"
#include "ui/camera/dialog/areas_page.h"
#include "ui/camera/dialog/configure_page.h"
#include "ui/camera/dialog/models_page.h"
#include "ui/camera/dialog/roi_canvas.h"
#include "ui/camera/wizard_controller.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QSlider>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QImage>
#include <QLabel>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPushButton>
#include <QPointF>
#include <QString>
#include <QTemporaryDir>
#include <QTextStream>
#include <QElapsedTimer>
#include <QTimer>

#include <opencv2/core.hpp>

#include <optional>
#include <string>
#include <vector>

using denso::camera::Camera;
using denso::camera::CameraArea;
using denso::camera::Point;
using denso::camera::ImageEnhancement;
using denso::camera::RoiEnhancement;
using denso::ui::CameraAreasPage;
using denso::ui::RoiCanvas;

namespace {

constexpr int kW = 256;
constexpr int kH = 192;   // same 4:3 aspect as the canvas below, so the fitted
                          // image rect is the whole widget and widget == norm*size

CameraArea rect_area(const std::string& name, float x1, float y1, float x2,
                     float y2) {
    CameraArea a;
    a.name = name;
    a.points = {Point{x1, y1}, Point{x2, y1}, Point{x2, y2}, Point{x1, y2}};
    return a;
}

/// A faded frame — the situation the feature exists for.
QImage faded_frame() {
    QImage img(kW, kH, QImage::Format_RGB888);
    for (int y = 0; y < kH; ++y) {
        for (int x = 0; x < kW; ++x) {
            const int v = 118 + (x * 14) / kW + (y * 5) / kH;
            img.setPixel(x, y, qRgb(v, v, v));
        }
    }
    return img;
}

/// The same faded frame with a colour cast. Needed wherever SATURATION is under
/// test: scaling chroma cannot change a greyscale image, because there is no
/// chroma to scale — a grey fixture would report the control as broken.
QImage faded_tinted_frame() {
    QImage img(kW, kH, QImage::Format_RGB888);
    for (int y = 0; y < kH; ++y) {
        for (int x = 0; x < kW; ++x) {
            const int v = 118 + (x * 14) / kW + (y * 5) / kH;
            img.setPixel(x, y, qRgb(v + 18, v, v - 14));
        }
    }
    return img;
}

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

/// A page with a snapshot, sized so widget coordinates are normalized * size.
struct PageFixture {
    CameraAreasPage page;
    PageFixture() {
        page.resize(kW + 360, kH + 200);
        page.load({}, {});
        REQUIRE(page.canvas() != nullptr);
        page.canvas()->resize(kW, kH);
        page.set_background(faded_frame());
    }
    RoiCanvas* canvas() { return page.canvas(); }
    QComboBox* strength() {
        return page.findChild<QComboBox*>(QStringLiteral("areasEnhanceStrength"));
    }
    QCheckBox* preview() {
        return page.findChild<QCheckBox*>(QStringLiteral("areasEnhancePreview"));
    }
    QCheckBox* enable() {
        return page.findChild<QCheckBox*>(QStringLiteral("areasEnhanceEnable"));
    }
    QPushButton* reset() {
        return page.findChild<QPushButton*>(QStringLiteral("areasEnhanceReset"));
    }
    QSlider* slider(const char* name) {
        return page.findChild<QSlider*>(QString::fromLatin1(name));
    }
    QLabel* label(const char* name) {
        return page.findChild<QLabel*>(QString::fromLatin1(name));
    }
    /// Switch the feature on and pick a local-contrast level, exactly as the
    /// operator would: the master switch first, then the control.
    void choose(RoiEnhancement level) {
        enable()->setChecked(true);
        const int idx = strength()->findData(QVariant(denso::camera::to_int(level)));
        REQUIRE(idx >= 0);
        strength()->setCurrentIndex(idx);
    }
};

bool identical(const QImage& a, const QImage& b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (int y = 0; y < a.height(); ++y) {
        for (int x = 0; x < a.width(); ++x) {
            if (a.pixel(x, y) != b.pixel(x, y)) {
                return false;
            }
        }
    }
    return true;
}

int differing_pixels(const QImage& a, const QImage& b) {
    int n = 0;
    for (int y = 0; y < a.height(); ++y) {
        for (int x = 0; x < a.width(); ++x) {
            if (a.pixel(x, y) != b.pixel(x, y)) {
                ++n;
            }
        }
    }
    return n;
}

/// Every pixel outside `areas` must be identical between the two images.
bool unchanged_outside(const QImage& before, const QImage& after,
                       const std::vector<CameraArea>& areas) {
    const cv::Mat mask = denso::ui::build_area_mask(areas, before.width(),
                                                    before.height());
    REQUIRE_FALSE(mask.empty());
    for (int y = 0; y < before.height(); ++y) {
        for (int x = 0; x < before.width(); ++x) {
            if (mask.at<uchar>(y, x) == 0 &&
                before.pixel(x, y) != after.pixel(x, y)) {
                return false;
            }
        }
    }
    return true;
}

void click(QWidget* w, const QPointF& at) {
    QMouseEvent press(QEvent::MouseButtonPress, at, w->mapToGlobal(at.toPoint()),
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(w, &press);
    QMouseEvent release(QEvent::MouseButtonRelease, at,
                        w->mapToGlobal(at.toPoint()), Qt::LeftButton,
                        Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(w, &release);
}

QPointF at_norm(double nx, double ny) {
    return QPointF(nx * kW, ny * kH);
}

/// Let the page's coalesced preview timer fire.
///
/// A SLIDER change is deliberately coalesced (a drag emits a change per pixel of
/// travel), so unlike a combo pick it does not repaint synchronously. A test that
/// asserted straight after moving a slider would be asserting on the previous
/// render. Discrete controls need none of this.
void settle() {
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < 300) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
}

}  // namespace

// ─── 1. Preview off ──────────────────────────────────────────────────────────

TEST_CASE("preview off shows the original snapshot", "[roi_enhance][ui]") {
    PageFixture f;
    f.page.load({rect_area("meter", 0.25f, 0.25f, 0.75f, 0.75f)}, {});
    f.page.set_background(faded_frame());

    // Off + preview off.
    CHECK(identical(f.canvas()->frame(), faded_frame()));

    // A level chosen but the preview NOT enabled is still the original: the
    // checkbox is what puts the enhancement on screen, and the wall is never
    // enhanced at all.
    f.choose(RoiEnhancement::High);
    CHECK(identical(f.canvas()->frame(), faded_frame()));
}

TEST_CASE("preview on at Off is still the original", "[roi_enhance][ui]") {
    PageFixture f;
    f.page.load({rect_area("meter", 0.25f, 0.25f, 0.75f, 0.75f)}, {});
    f.page.set_background(faded_frame());

    // The checkbox is disabled at Off — there is nothing to preview — so it can
    // never be a control that silently does nothing.
    CHECK_FALSE(f.preview()->isEnabled());
    f.preview()->setChecked(true);
    CHECK(identical(f.canvas()->frame(), faded_frame()));
}

// ─── 2. Preview on ───────────────────────────────────────────────────────────

TEST_CASE("preview on enhances inside the areas and nowhere else",
          "[roi_enhance][ui]") {
    const std::vector<CameraArea> areas{
        rect_area("meter", 0.25f, 0.25f, 0.75f, 0.75f)};
    PageFixture f;
    f.page.load(areas, {});
    f.page.set_background(faded_frame());
    const QImage original = f.canvas()->frame();

    f.choose(RoiEnhancement::Medium);
    REQUIRE(f.preview()->isEnabled());
    f.preview()->setChecked(true);

    const QImage shown = f.canvas()->frame();
    CHECK(shown.size() == original.size());
    CHECK(differing_pixels(shown, original) > 0);   // something happened…
    CHECK(unchanged_outside(original, shown, areas));   // …only inside the areas

    // Turning it back off restores the original exactly — the page always
    // renders from the stored snapshot, so this is a return, not an inverse.
    f.preview()->setChecked(false);
    CHECK(identical(f.canvas()->frame(), original));
}

TEST_CASE("changing strength re-renders from the ORIGINAL, never from the "
          "previous enhanced output", "[roi_enhance][ui]") {
    // The failure this guards is compounding: if the page ever fed its own
    // rendered output back in, walking High -> Low -> High would drift further
    // from the snapshot each time, and the operator would be comparing levels
    // against a moving baseline.
    PageFixture f;
    f.page.load({rect_area("meter", 0.2f, 0.2f, 0.8f, 0.8f)}, {});
    f.page.set_background(faded_frame());

    f.choose(RoiEnhancement::High);
    f.preview()->setChecked(true);
    const QImage first_high = f.canvas()->frame();

    f.choose(RoiEnhancement::Low);
    const QImage low = f.canvas()->frame();
    f.choose(RoiEnhancement::Medium);
    f.choose(RoiEnhancement::High);
    const QImage second_high = f.canvas()->frame();

    CHECK(identical(first_high, second_high));
    CHECK(differing_pixels(low, first_high) > 0);   // the levels really differ

    // And Off, mid-session, is the untouched snapshot again.
    f.choose(RoiEnhancement::Off);
    CHECK(identical(f.canvas()->frame(), faded_frame()));
}

TEST_CASE("the preview matches what the runtime authority produces",
          "[roi_enhance][ui]") {
    // Same input, same level, same areas: the picture on the canvas must be the
    // picture ui::enhance_preview produces — which is the same object the
    // inference worker applies. One authority, asserted rather than assumed.
    const std::vector<CameraArea> areas{
        rect_area("meter", 0.3f, 0.3f, 0.7f, 0.7f)};
    PageFixture f;
    f.page.load(areas, {});
    f.page.set_background(faded_frame());
    f.choose(RoiEnhancement::High);
    f.preview()->setChecked(true);

    // The bundle the page is holding, handed straight to the shared authority.
    // Reading it back off the page rather than rebuilding it here is the point:
    // if the page ever diverged from what it renders, this would catch it.
    const QImage expected =
        denso::ui::enhance_preview(faded_frame(), f.page.enhancement(), areas);
    CHECK(identical(f.canvas()->frame(), expected));
}

// ─── 3. Working geometry, not persisted geometry ─────────────────────────────

TEST_CASE("the preview mask follows the page's working areas",
          "[roi_enhance][ui]") {
    // The page holds the working set and never reads the database, so what is
    // previewed is what a Save would write — including edits that are not saved.
    PageFixture f;
    const std::vector<CameraArea> left{rect_area("left", 0.05f, 0.3f, 0.35f, 0.7f)};
    f.page.load(left, {});
    f.page.set_background(faded_frame());
    f.choose(RoiEnhancement::High);
    f.preview()->setChecked(true);
    const QImage with_left = f.canvas()->frame();
    CHECK(unchanged_outside(faded_frame(), with_left, left));

    // Replace the working set with a different polygon: the enhanced region
    // moves with it.
    const std::vector<CameraArea> right{
        rect_area("right", 0.65f, 0.3f, 0.95f, 0.7f)};
    f.page.load(right, {});
    f.page.set_background(faded_frame());
    f.choose(RoiEnhancement::High);
    f.preview()->setChecked(true);
    const QImage with_right = f.canvas()->frame();
    CHECK(unchanged_outside(faded_frame(), with_right, right));
    CHECK(differing_pixels(with_left, with_right) > 0);
}

TEST_CASE("a polygon still being drawn is already in the preview mask",
          "[roi_enhance][ui]") {
    // An operator must not have to finish and save an area merely to see the
    // effect of enhancing it — the whole workflow is draw, preview, compare.
    PageFixture f;
    f.page.load({}, {});
    f.page.set_background(faded_frame());
    f.choose(RoiEnhancement::High);

    // Three clicks on the canvas: a triangle in the top-left quadrant, not yet
    // closed and not yet in the working set.
    f.canvas()->begin_draw();
    click(f.canvas(), at_norm(0.10, 0.10));
    click(f.canvas(), at_norm(0.45, 0.10));
    click(f.canvas(), at_norm(0.45, 0.45));
    REQUIRE(f.canvas()->point_count() == 3);

    // Enabling the preview renders immediately, using that in-progress shape.
    f.preview()->setChecked(true);
    const QImage shown = f.canvas()->frame();
    CHECK(differing_pixels(shown, faded_frame()) > 0);

    // The bottom-right corner is outside the triangle, so it is untouched —
    // proof the mask really is the drawn shape and not the whole frame.
    CHECK(shown.pixel(kW - 3, kH - 3) == faded_frame().pixel(kW - 3, kH - 3));
}

TEST_CASE("with no areas the preview enhances the whole frame and says so",
          "[roi_enhance][ui]") {
    // Runtime semantics: no areas already means whole-frame detection. The
    // preview matches it rather than showing an unexplained blank change, and
    // the hint states it so the operator is not left guessing.
    PageFixture f;
    f.page.load({}, {});
    f.page.set_background(faded_frame());
    f.choose(RoiEnhancement::High);
    f.preview()->setChecked(true);

    const QImage shown = f.canvas()->frame();
    CHECK(differing_pixels(shown, faded_frame()) > 0);
    // The corners moved too — nothing was confined.
    CHECK(shown.pixel(2, 2) != faded_frame().pixel(2, 2));

    auto* hint = f.page.findChild<QLabel*>(QStringLiteral("areasEnhanceHint"));
    REQUIRE(hint != nullptr);
    CHECK(hint->text().contains(QStringLiteral("whole frame")));
}

// ─── 4. Persistence: only Save writes ────────────────────────────────────────

namespace {

/// The wizard pages plus the real controller, wired as CameraDialog wires them.
struct WizardFixture {
    denso::ui::CameraAddPage add;
    denso::ui::CameraConfigurePage configure;
    denso::ui::ModelsPage models;
    CameraAreasPage areas;
    int shown = -1;
    denso::ui::CameraWizardController controller;

    explicit WizardFixture(QSqlDatabase db)
        : controller(db,
                     denso::ui::CameraWizardController::Pages{&add, &configure,
                                                              &models, &areas,
                                                              nullptr},
                     [this](int p) { shown = p; }) {
        models.set_db(db);
    }
};

struct DbFixture {
    QTemporaryDir dir;
    std::optional<denso::db::Db> db;
    DbFixture() {
        db = denso::db::Db::open(QDir(dir.path()).filePath(QStringLiteral("denso.db")));
        REQUIRE(db.has_value());
        REQUIRE(denso::db::run_migrations(db->handle()));
    }
    QSqlDatabase h() { return db->handle(); }
};

}  // namespace

TEST_CASE("Save persists the strength; leaving does not", "[roi_enhance][ui]") {
    DbFixture h;
    const auto id = denso::camera::insert(h.h(), usb_camera("Line 1"));
    REQUIRE(id.has_value());
    Camera cam = *denso::camera::get(h.h(), *id);
    REQUIRE_FALSE(denso::camera::has_effect(cam.image_enhance));

    // ── Abandoning the page must write nothing ──────────────────────────────
    {
        WizardFixture f(h.h());
        f.controller.begin_edit(cam);
        f.areas.set_enhancement(cam.image_enhance);
        auto* strength =
            f.areas.findChild<QComboBox*>(QStringLiteral("areasEnhanceStrength"));
        REQUIRE(strength != nullptr);
        strength->setCurrentIndex(
            strength->findData(QVariant(denso::camera::to_int(RoiEnhancement::High))));
        REQUIRE(f.areas.enhancement().local_contrast == RoiEnhancement::High);
        // …and then simply leave. No save_areas() call at all.
    }
    CHECK_FALSE(denso::camera::has_effect(
        denso::camera::get(h.h(), *id)->image_enhance));

    // ── Save writes it ──────────────────────────────────────────────────────
    {
        WizardFixture f(h.h());
        f.controller.begin_edit(cam);
        f.areas.set_enhancement(cam.image_enhance);
        auto* strength =
            f.areas.findChild<QComboBox*>(QStringLiteral("areasEnhanceStrength"));
        strength->setCurrentIndex(
            strength->findData(QVariant(denso::camera::to_int(RoiEnhancement::Medium))));
        f.controller.save_areas({rect_area("meter", 0.2f, 0.2f, 0.8f, 0.8f)});
    }
    const auto saved = denso::camera::get(h.h(), *id);
    REQUIRE(saved.has_value());
    CHECK(saved->image_enhance.local_contrast == RoiEnhancement::Medium);
    // The areas landed in the same save.
    CHECK(denso::camera::areas_for(h.h(), *id).size() == 1);

    // ── Reopening restores it, with the preview OFF ─────────────────────────
    {
        WizardFixture f(h.h());
        f.areas.set_enhancement(saved->image_enhance);
        CHECK(f.areas.enhancement().local_contrast == RoiEnhancement::Medium);
        auto* strength =
            f.areas.findChild<QComboBox*>(QStringLiteral("areasEnhanceStrength"));
        CHECK(strength->currentData().toInt() ==
              denso::camera::to_int(RoiEnhancement::Medium));
        // The preview toggle is view state and is never persisted: every entry
        // starts from the picture the camera actually sends.
        auto* preview =
            f.areas.findChild<QCheckBox*>(QStringLiteral("areasEnhancePreview"));
        REQUIRE(preview != nullptr);
        CHECK_FALSE(preview->isChecked());
    }
}

TEST_CASE("an unchanged strength issues no camera write", "[roi_enhance][ui]") {
    // A save that touched no strength must behave exactly as it did before this
    // feature existed — the areas transaction and nothing else.
    DbFixture h;
    const auto id = denso::camera::insert(h.h(), usb_camera("Line 1"));
    REQUIRE(id.has_value());
    Camera cam = *denso::camera::get(h.h(), *id);

    WizardFixture f(h.h());
    f.controller.begin_edit(cam);
    f.areas.set_enhancement(cam.image_enhance);   // Off, unchanged
    f.controller.save_areas({rect_area("meter", 0.2f, 0.2f, 0.8f, 0.8f)});

    const auto after = denso::camera::get(h.h(), *id);
    REQUIRE(after.has_value());
    CHECK_FALSE(denso::camera::has_effect(after->image_enhance));
    CHECK(denso::camera::areas_for(h.h(), *id).size() == 1);
}

namespace {

/// Dismiss the next modal that appears. show_save_error() is a blocking
/// QMessageBox, so a failed save cannot be driven without one.
QTimer* dismiss_next_modal(bool* seen) {
    auto* t = new QTimer;
    t->setInterval(10);
    QObject::connect(t, &QTimer::timeout, [t, seen] {
        for (QWidget* w : QApplication::topLevelWidgets()) {
            auto* box = qobject_cast<QMessageBox*>(w);
            if (box && box->isVisible()) {
                t->stop();
                if (seen) *seen = true;
                box->button(QMessageBox::Ok)->click();
                return;
            }
        }
    });
    t->start();
    return t;
}

}  // namespace

TEST_CASE("a failed Save persists NOTHING and leaves the page ready to retry",
          "[roi_enhance][ui][areas_atomicity]") {
    DbFixture h;
    const auto id = denso::camera::insert(h.h(), usb_camera("Line 1"));
    REQUIRE(id.has_value());
    // The camera starts with a saved area and Off.
    REQUIRE(denso::camera::replace_areas(
        h.h(), *id, {rect_area("original", 0.1f, 0.1f, 0.3f, 0.3f)}));

    // Another camera owns zone 42, so a save claiming it is refused by the
    // repository — the authoritative check, discovered AFTER the level write
    // inside the same transaction.
    const auto other = denso::camera::insert(h.h(), usb_camera("Line 2"));
    REQUIRE(other.has_value());
    CameraArea theirs = rect_area("theirs", 0.6f, 0.6f, 0.9f, 0.9f);
    theirs.zone = 42;
    REQUIRE(denso::camera::replace_areas(h.h(), *other, {theirs}));

    const Camera cam = *denso::camera::get(h.h(), *id);
    REQUIRE_FALSE(denso::camera::has_effect(cam.image_enhance));

    WizardFixture f(h.h());
    f.controller.begin_edit(cam);
    f.areas.load({rect_area("original", 0.1f, 0.1f, 0.3f, 0.3f)}, {});
    f.areas.set_enhancement(cam.image_enhance);

    auto* strength =
        f.areas.findChild<QComboBox*>(QStringLiteral("areasEnhanceStrength"));
    REQUIRE(strength != nullptr);
    strength->setCurrentIndex(
        strength->findData(QVariant(denso::camera::to_int(RoiEnhancement::High))));
    REQUIRE(f.areas.enhancement().local_contrast == RoiEnhancement::High);

    CameraArea clashing = rect_area("mine", 0.2f, 0.2f, 0.5f, 0.5f);
    clashing.zone = 42;
    bool saw_modal = false;
    QTimer* t = dismiss_next_modal(&saw_modal);
    f.controller.save_areas({clashing});
    t->stop();
    t->deleteLater();
    CHECK(saw_modal);   // the operator was told

    // ── NOTHING persisted ───────────────────────────────────────────────────
    const auto after = denso::camera::get(h.h(), *id);
    REQUIRE(after.has_value());
    CHECK(after->image_enhance == cam.image_enhance);   // nothing escaped
    const auto areas = denso::camera::areas_for(h.h(), *id);
    REQUIRE(areas.size() == 1);
    CHECK(areas[0].name == "original");
    CHECK_FALSE(areas[0].zone.has_value());

    // ── the working edits are still on screen, and still owed ───────────────
    CHECK(f.areas.enhancement().local_contrast == RoiEnhancement::High);
    CHECK(f.areas.is_dirty());   // there IS an unsaved change, and it says so

    // ── reopening shows the OLD persisted state ─────────────────────────────
    const Camera reread = *denso::camera::get(h.h(), *id);
    WizardFixture again(h.h());
    again.areas.set_enhancement(reread.image_enhance);
    CHECK_FALSE(denso::camera::has_effect(again.areas.enhancement()));
    CHECK_FALSE(again.areas.is_dirty());
}

TEST_CASE("a successful Save re-baselines the page so nothing reads as unsaved",
          "[roi_enhance][ui][areas_atomicity]") {
    // finish_and_leave() deliberately STAYS on this page when the camera could
    // not be marked complete, so a page that never re-baselined would keep
    // offering to discard work that is already on disk.
    DbFixture h;
    const auto id = denso::camera::insert(h.h(), usb_camera("Line 1"));
    REQUIRE(id.has_value());
    const Camera cam = *denso::camera::get(h.h(), *id);

    WizardFixture f(h.h());
    f.controller.begin_edit(cam);
    f.areas.load({}, {});
    f.areas.set_enhancement(cam.image_enhance);

    auto* strength =
        f.areas.findChild<QComboBox*>(QStringLiteral("areasEnhanceStrength"));
    strength->setCurrentIndex(
        strength->findData(QVariant(denso::camera::to_int(RoiEnhancement::Medium))));
    CHECK(f.areas.is_dirty());

    f.controller.save_areas({rect_area("meter", 0.2f, 0.2f, 0.8f, 0.8f)});

    CHECK(denso::camera::get(h.h(), *id)->image_enhance.local_contrast ==
          RoiEnhancement::Medium);
    CHECK_FALSE(f.areas.is_dirty());   // saved work no longer reads as owed
}

TEST_CASE("a strength change alone counts as unsaved work", "[roi_enhance][ui]") {
    // Otherwise Back/Exit would discard it with no warning, because the polygons
    // are untouched and the old dirty check only looked at those.
    PageFixture f;
    f.page.load({rect_area("meter", 0.2f, 0.2f, 0.8f, 0.8f)}, {});
    f.page.set_enhancement(ImageEnhancement{});
    CHECK_FALSE(f.page.is_dirty());

    f.choose(RoiEnhancement::High);
    CHECK(f.page.is_dirty());

    // Back to what was loaded: nothing is owed again.
    f.enable()->setChecked(false);
    f.choose(RoiEnhancement::Off);
    f.enable()->setChecked(false);
    CHECK_FALSE(f.page.is_dirty());

    // EVERY control counts, not just the combo.
    for (const char* name :
         {"areasEnhanceBrightness", "areasEnhanceContrast", "areasEnhanceGamma",
          "areasEnhanceSaturation"}) {
        QSlider* sl = f.slider(name);
        REQUIRE(sl != nullptr);
        const int was = sl->value();
        sl->setValue(was == sl->maximum() ? sl->minimum() : sl->maximum());
        CHECK(f.page.is_dirty());
        sl->setValue(was);
        CHECK_FALSE(f.page.is_dirty());
    }

    // The preview checkbox is NOT work — it is never written anywhere.
    ImageEnhancement on;
    on.enabled = true;
    on.local_contrast = RoiEnhancement::High;
    f.page.set_enhancement(on);   // re-baseline
    CHECK_FALSE(f.page.is_dirty());
    f.preview()->setChecked(true);
    CHECK_FALSE(f.page.is_dirty());
}

// ─── 5. The wiring the dialog relies on ──────────────────────────────────────

TEST_CASE("entering the Areas step seeds the camera's persisted strength",
          "[roi_enhance][ui]") {
    // enter_areas() is the ONE place the page learns the camera's level. Asserted
    // against the source, in the same way test_ball_wizard asserts the dialog's
    // page↔controller connections: the alternative is a fixture that wires it
    // itself and passes whether or not the app does.
    QFile src(QStringLiteral(DENSO_SOURCE_DIR
                             "/src/app/ui/camera/wizard_controller.cpp"));
    REQUIRE(src.open(QIODevice::ReadOnly | QIODevice::Text));
    const QString text = QTextStream(&src).readAll();
    CHECK(text.contains(
        QStringLiteral("pages_.areas->set_enhancement(draft_.image_enhance)")));
    // …and that the save path reads it back off the page.
    CHECK(text.contains(QStringLiteral("pages_.areas->enhancement()")));
}

TEST_CASE("the Ball wizard branch never reaches the Areas page",
          "[roi_enhance][ui][ball]") {
    // Why the Digital-only control needs no visibility flag: in ball_leveler the
    // fourth step IS the level calibration page, and the per-row Areas shortcut
    // routes there too, so the page carrying this control is unreachable.
    QFile src(QStringLiteral(DENSO_SOURCE_DIR
                             "/src/app/ui/camera/wizard_controller.cpp"));
    REQUIRE(src.open(QIODevice::ReadOnly | QIODevice::Text));
    const QString text = QTextStream(&src).readAll();
    CHECK(text.contains(QStringLiteral("if (ball_mode()) {\n        enter_level(/*direct=*/true);")));
    CHECK(text.contains(QStringLiteral("enter_areas(/*direct=*/true);")));
}

// ─── 6. The master switch, the sliders and Reset ─────────────────────────────

TEST_CASE("the master switch gates the preview without erasing the tuning",
          "[roi_enhance][ui]") {
    PageFixture f;
    f.page.load({rect_area("meter", 0.25f, 0.25f, 0.75f, 0.75f)}, {});
    f.page.set_background(faded_frame());

    // Tune first, with the feature on.
    f.enable()->setChecked(true);
    f.slider("areasEnhanceBrightness")->setValue(45);
    f.slider("areasEnhanceGamma")->setValue(180);
    f.preview()->setChecked(true);
    const QImage tuned_view = f.canvas()->frame();
    CHECK(differing_pixels(tuned_view, faded_frame()) > 0);

    // Switch it off: the preview returns to the snapshot…
    f.enable()->setChecked(false);
    CHECK(identical(f.canvas()->frame(), faded_frame()));
    // …and the values are still there, which is the whole reason the switch is
    // separate from the tuning.
    CHECK(f.page.enhancement().brightness == 45);
    CHECK(f.page.enhancement().gamma == 180);
    CHECK_FALSE(f.page.enhancement().enabled);

    // Switch it back on and the identical picture returns.
    f.enable()->setChecked(true);
    CHECK(identical(f.canvas()->frame(), tuned_view));
}

TEST_CASE("the tuning controls are disabled while the feature is off",
          "[roi_enhance][ui]") {
    PageFixture f;
    f.page.set_enhancement(ImageEnhancement{});
    for (const char* name :
         {"areasEnhanceBrightness", "areasEnhanceContrast", "areasEnhanceGamma",
          "areasEnhanceSaturation"}) {
        CHECK_FALSE(f.slider(name)->isEnabled());
    }
    CHECK_FALSE(f.strength()->isEnabled());
    CHECK_FALSE(f.preview()->isEnabled());   // nothing to preview

    f.enable()->setChecked(true);
    for (const char* name :
         {"areasEnhanceBrightness", "areasEnhanceContrast", "areasEnhanceGamma",
          "areasEnhanceSaturation"}) {
        CHECK(f.slider(name)->isEnabled());
    }
    CHECK(f.strength()->isEnabled());
    // Still nothing to preview until something is actually moved off neutral.
    CHECK_FALSE(f.preview()->isEnabled());
    f.slider("areasEnhanceBrightness")->setValue(20);
    CHECK(f.preview()->isEnabled());
}

TEST_CASE("each slider changes the preview, from the ORIGINAL every time",
          "[roi_enhance][ui]") {
    PageFixture f;
    const std::vector<CameraArea> areas{
        rect_area("meter", 0.25f, 0.25f, 0.75f, 0.75f)};
    f.page.load(areas, {});
    // TINTED, because one of the four controls is saturation and chroma scaling
    // is a no-op on a grey image.
    const QImage base = faded_tinted_frame();
    f.page.set_background(base);
    f.enable()->setChecked(true);
    f.preview()->setChecked(false);

    for (const char* name :
         {"areasEnhanceBrightness", "areasEnhanceContrast", "areasEnhanceGamma",
          "areasEnhanceSaturation"}) {
        // Start from neutral each time so only this control is off its default.
        f.page.set_enhancement([] {
            ImageEnhancement e;
            e.enabled = true;
            return e;
        }());
        QSlider* sl = f.slider(name);
        REQUIRE(sl != nullptr);
        sl->setValue(sl->maximum());
        f.preview()->setChecked(true);
        // The coalescer defers geometry-driven renders; toggling the checkbox
        // renders immediately, which is what the operator experiences.
        const QImage shown = f.canvas()->frame();
        CHECK(differing_pixels(shown, base) > 0);
        CHECK(unchanged_outside(base, shown, areas));

        // Re-rendering at the same setting is identical — no compounding.
        f.preview()->setChecked(false);
        f.preview()->setChecked(true);
        CHECK(identical(f.canvas()->frame(), shown));
    }
}

TEST_CASE("Reset returns the working state to neutral without writing",
          "[roi_enhance][ui]") {
    DbFixture h;
    const auto id = denso::camera::insert(h.h(), usb_camera("Line 1"));
    REQUIRE(id.has_value());
    // A camera with tuning already SAVED, so a Reset that leaked to the database
    // would be visible.
    Camera cam = *denso::camera::get(h.h(), *id);
    cam.image_enhance.enabled = true;
    cam.image_enhance.local_contrast = RoiEnhancement::High;
    cam.image_enhance.brightness = 40;
    cam.image_enhance.gamma = 200;
    REQUIRE(denso::camera::update(h.h(), cam));

    WizardFixture f(h.h());
    f.controller.begin_edit(*denso::camera::get(h.h(), *id));
    f.areas.load({}, {});
    f.areas.set_enhancement(denso::camera::get(h.h(), *id)->image_enhance);
    CHECK_FALSE(f.areas.is_dirty());

    auto* reset = f.areas.findChild<QPushButton*>(QStringLiteral("areasEnhanceReset"));
    REQUIRE(reset != nullptr);
    reset->click();

    // Working state is neutral and disabled…
    CHECK(f.areas.enhancement() == denso::camera::neutral_enhancement());
    // …it counts as unsaved work…
    CHECK(f.areas.is_dirty());
    // …and NOTHING was written.
    CHECK(denso::camera::get(h.h(), *id)->image_enhance.brightness == 40);
    CHECK(denso::camera::get(h.h(), *id)->image_enhance.enabled);

    // Abandoning after a Reset restores the persisted settings on the next entry.
    WizardFixture again(h.h());
    again.areas.set_enhancement(denso::camera::get(h.h(), *id)->image_enhance);
    CHECK(again.areas.enhancement().brightness == 40);
    CHECK(again.areas.enhancement().local_contrast == RoiEnhancement::High);
    CHECK_FALSE(again.areas.is_dirty());
}

TEST_CASE("the numeric readouts track the sliders", "[roi_enhance][ui]") {
    // A slider whose value a technician cannot read is a setting they cannot
    // reproduce on the next camera or report back to us.
    PageFixture f;
    f.enable()->setChecked(true);
    f.slider("areasEnhanceBrightness")->setValue(37);
    f.slider("areasEnhanceContrast")->setValue(-22);
    f.slider("areasEnhanceGamma")->setValue(175);
    f.slider("areasEnhanceSaturation")->setValue(0);

    // The readouts are plain labels beside each slider, so they are found by
    // the text they carry rather than by an object name.
    const auto texts = [&] {
        QStringList out;
        for (QLabel* l : f.page.findChildren<QLabel*>()) out << l->text();
        return out;
    }();
    CHECK(texts.contains(QStringLiteral("+37")));
    CHECK(texts.contains(QStringLiteral("-22")));
    CHECK(texts.contains(QStringLiteral("1.75")));   // gamma in human form
    CHECK(texts.contains(QStringLiteral("0")));
}

TEST_CASE("the whole bundle is saved, and restored on reopening",
          "[roi_enhance][ui][areas_atomicity]") {
    DbFixture h;
    const auto id = denso::camera::insert(h.h(), usb_camera("Line 1"));
    REQUIRE(id.has_value());
    const Camera cam = *denso::camera::get(h.h(), *id);

    WizardFixture f(h.h());
    f.controller.begin_edit(cam);
    f.areas.load({}, {});
    f.areas.set_enhancement(cam.image_enhance);

    f.areas.findChild<QCheckBox*>(QStringLiteral("areasEnhanceEnable"))->setChecked(true);
    auto* strength =
        f.areas.findChild<QComboBox*>(QStringLiteral("areasEnhanceStrength"));
    strength->setCurrentIndex(
        strength->findData(QVariant(denso::camera::to_int(RoiEnhancement::Low))));
    f.areas.findChild<QSlider*>(QStringLiteral("areasEnhanceBrightness"))->setValue(-30);
    f.areas.findChild<QSlider*>(QStringLiteral("areasEnhanceContrast"))->setValue(45);
    f.areas.findChild<QSlider*>(QStringLiteral("areasEnhanceGamma"))->setValue(120);
    f.areas.findChild<QSlider*>(QStringLiteral("areasEnhanceSaturation"))->setValue(-15);

    f.controller.save_areas({rect_area("meter", 0.2f, 0.2f, 0.8f, 0.8f)});

    const auto saved = denso::camera::get(h.h(), *id);
    REQUIRE(saved.has_value());
    CHECK(saved->image_enhance.enabled);
    CHECK(saved->image_enhance.local_contrast == RoiEnhancement::Low);
    CHECK(saved->image_enhance.brightness == -30);
    CHECK(saved->image_enhance.contrast == 45);
    CHECK(saved->image_enhance.gamma == 120);
    CHECK(saved->image_enhance.saturation == -15);
    CHECK_FALSE(f.areas.is_dirty());   // re-baselined by the commit

    // Reopening restores every value, with the preview off.
    WizardFixture again(h.h());
    again.areas.set_enhancement(saved->image_enhance);
    CHECK(again.areas.enhancement() == saved->image_enhance);
    CHECK_FALSE(again.areas.findChild<QCheckBox*>(
                        QStringLiteral("areasEnhancePreview"))->isChecked());
}

TEST_CASE("a slider reaches the preview through the coalescer, a combo at once",
          "[roi_enhance][ui]") {
    // The two kinds of control are deliberately different. A combo pick and the
    // master switch are discrete operator actions and repaint immediately; a
    // slider is continuous and coalesces, because a drag emits a change per pixel
    // of travel and enhancing a 1080p snapshot on each one would make it
    // unusable. Both halves are asserted so neither can quietly change.
    PageFixture f;
    f.page.load({rect_area("meter", 0.2f, 0.2f, 0.8f, 0.8f)}, {});
    f.page.set_background(faded_frame());
    f.enable()->setChecked(true);
    f.slider("areasEnhanceBrightness")->setValue(10);
    f.preview()->setChecked(true);
    const QImage first = f.canvas()->frame();
    CHECK(differing_pixels(first, faded_frame()) > 0);

    // Continuous: no synchronous repaint...
    f.slider("areasEnhanceBrightness")->setValue(90);
    CHECK(identical(f.canvas()->frame(), first));
    // ...but the coalescer does deliver it.
    settle();
    const QImage after = f.canvas()->frame();
    CHECK(differing_pixels(after, first) > 0);

    // Discrete: the combo repaints on the spot, no waiting.
    f.strength()->setCurrentIndex(
        f.strength()->findData(QVariant(denso::camera::to_int(RoiEnhancement::High))));
    CHECK(differing_pixels(f.canvas()->frame(), after) > 0);
}
