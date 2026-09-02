// The Save-refusal contract, for BOTH wizard steps that carry a zone number.
//
// The rule this file defends: attempting Save on an invalid or conflicting
// configuration must produce an EXPLICIT refusal the operator can act on — a
// modal naming the offending zone and, where the information exists, who holds
// it — while nothing invalid ever reaches the write path.
//
// Two things are deliberately NOT tested by driving pixels. The refusal TEXT is
// asserted through the same authority the dialog is built from, and the "no
// dialog on a valid save" case is asserted by a modal-answering timer that never
// fires. Both are stable against styling; scraping a rendered dialog is not.
//
// The inline red validation is unchanged and still fires while typing — these
// cases only pin that the SAVE ATTEMPT is additionally explicit, and that the
// two never disagree, because they read one authority.
#include "ui/camera/dialog/areas_page.h"
#include "ui/camera/dialog/level_calibration_page.h"

#include "camera/camera.h"
#include "level/calibration.h"

#include <catch2/catch_test_macros.hpp>

#include <QAbstractButton>
#include <QApplication>
#include <QImage>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QString>
#include <QTimer>

#include <map>
#include <optional>
#include <string>
#include <vector>

using denso::ui::CameraAreasPage;
using denso::ui::LevelCalibrationPage;

namespace {

/// What a modal said, captured as it is dismissed. QMessageBox::warning spins a
/// nested event loop, so a timer is the only way to reach in, read it and close
/// it without deadlocking the test.
struct ModalCapture {
    bool seen = false;
    QString title;
    QString text;
};

/// Arm a one-shot answerer. `cap.seen` stays false when no dialog appears, which
/// is exactly how the "a valid save is silent" cases are expressed.
QTimer* catch_modal(ModalCapture& cap) {
    auto* t = new QTimer;
    t->setInterval(10);
    QObject::connect(t, &QTimer::timeout, [t, &cap] {
        for (QWidget* w : QApplication::topLevelWidgets()) {
            auto* box = qobject_cast<QMessageBox*>(w);
            if (box && box->isVisible()) {
                t->stop();
                cap.seen = true;
                cap.title = box->windowTitle();
                cap.text = box->text();
                box->button(QMessageBox::Ok)->click();
                return;
            }
        }
    });
    t->start();
    return t;
}

QImage frame() {
    QImage img(1280, 720, QImage::Format_RGB888);
    img.fill(Qt::darkGray);
    return img;
}

QAbstractButton* button_named(const QWidget& w, const QString& name) {
    for (QAbstractButton* b : w.findChildren<QAbstractButton*>()) {
        if (b->objectName() == name) return b;
    }
    return nullptr;
}

/// A calibration that passes every geometry rule, so the ONLY thing a case can
/// be refused for is its zone number.
denso::level::LevelCalibration sound() {
    denso::level::LevelCalibration c;
    c.rect_x = 0.20;
    c.rect_y = 0.20;
    c.rect_w = 0.50;
    c.rect_h = 0.50;
    c.y_100 = 0.30;
    c.y_0 = 0.60;
    return c;
}

denso::level::LevelZone ball_zone(int no) {
    denso::level::LevelZone z;
    z.zone_no = no;
    z.calibration = sound();
    return z;
}

/// A triangle with real area, so polygon_is_degenerate never fires first.
denso::camera::CameraArea area(const std::string& name, std::optional<int> zone,
                               float ox) {
    denso::camera::CameraArea a;
    a.name = name;
    a.zone = zone;
    a.points = {{ox, 0.10f}, {ox + 0.25f, 0.10f}, {ox + 0.12f, 0.45f}};
    return a;
}

}  // namespace

// ─── Floating Ball Leveler ───────────────────────────────────────────────────

TEST_CASE("Ball: saving a zone held by another camera is refused out loud",
          "[zone_refusal][ui][ball]") {
    LevelCalibrationPage page;
    page.resize(960, 640);
    page.set_background(frame());
    // Zone 1 is already claimed on another camera, and this camera is sitting on
    // it — the exact state an operator reaches by accepting the default.
    page.load({ball_zone(1)}, {{1, "Tank 2"}});

    int emitted = 0;
    QObject::connect(&page, &LevelCalibrationPage::save_requested,
                     [&emitted](const std::vector<denso::level::LevelZone>&) {
                         ++emitted;
                     });

    QAbstractButton* save = button_named(page, QStringLiteral("levelSave"));
    REQUIRE(save != nullptr);
    CHECK(save->isEnabled());   // attemptable, so the refusal is reachable

    ModalCapture cap;
    QTimer* t = catch_modal(cap);
    save->click();
    t->stop();
    t->deleteLater();

    REQUIRE(cap.seen);
    INFO("modal title: " << cap.title.toStdString());
    INFO("modal text : " << cap.text.toStdString());
    CHECK(cap.title == QStringLiteral("Cannot save zones"));
    CHECK(cap.text.contains(QStringLiteral("Zone 1")));
    CHECK(cap.text.contains(QStringLiteral("Tank 2")));      // WHO holds it
    CHECK(cap.text.contains(QStringLiteral("Choose another Zone number")));
    CHECK(emitted == 0);                                     // nothing persisted
}

TEST_CASE("Ball: a zone duplicated on this same camera is refused by number",
          "[zone_refusal][ui][ball]") {
    LevelCalibrationPage page;
    page.resize(960, 640);
    page.set_background(frame());
    page.load({ball_zone(1), ball_zone(2)}, {});

    auto* list = page.findChild<QListWidget*>(QStringLiteral("levelZoneList"));
    REQUIRE(list != nullptr);
    REQUIRE(list->count() == 2);
    list->setCurrentRow(1);                       // editing Zone 2...

    auto* field = page.findChild<QLineEdit*>(QStringLiteral("zoneNumberEdit"));
    REQUIRE(field != nullptr);
    field->setText(QStringLiteral("1"));          // ...and typing its sibling's

    int emitted = 0;
    QObject::connect(&page, &LevelCalibrationPage::save_requested,
                     [&emitted](const std::vector<denso::level::LevelZone>&) {
                         ++emitted;
                     });

    ModalCapture cap;
    QTimer* t = catch_modal(cap);
    button_named(page, QStringLiteral("levelSave"))->click();
    t->stop();
    t->deleteLater();

    REQUIRE(cap.seen);
    INFO("modal title: " << cap.title.toStdString());
    INFO("modal text : " << cap.text.toStdString());
    CHECK(cap.text.contains(QStringLiteral("Zone 1")));
    CHECK(cap.text.contains(QStringLiteral("Choose another Zone number")));
    CHECK(emitted == 0);
}

TEST_CASE("Ball: a sound configuration saves with no dialog at all",
          "[zone_refusal][ui][ball]") {
    LevelCalibrationPage page;
    page.resize(960, 640);
    page.set_background(frame());
    page.load({ball_zone(3)}, {});

    CHECK_FALSE(page.save_block().has_value());

    int emitted = 0;
    QObject::connect(&page, &LevelCalibrationPage::save_requested,
                     [&emitted](const std::vector<denso::level::LevelZone>&) {
                         ++emitted;
                     });

    ModalCapture cap;
    QTimer* t = catch_modal(cap);
    button_named(page, QStringLiteral("levelSave"))->click();
    t->stop();
    t->deleteLater();

    CHECK_FALSE(cap.seen);   // a refusal dialog, never a confirmation step
    CHECK(emitted == 1);
}

TEST_CASE("Ball: the inline line and the refusal dialog are one sentence",
          "[zone_refusal][ui][ball]") {
    // One authority, so they cannot drift. A second set of rules for the modal
    // is exactly how a dialog ends up contradicting the field above it.
    LevelCalibrationPage page;
    page.resize(960, 640);
    page.set_background(frame());
    page.load({ball_zone(1)}, {{1, "Tank 2"}});

    const auto blocked = page.save_block();
    REQUIRE(blocked.has_value());

    ModalCapture cap;
    QTimer* t = catch_modal(cap);
    button_named(page, QStringLiteral("levelSave"))->click();
    t->stop();
    t->deleteLater();

    REQUIRE(cap.seen);
    INFO("modal title: " << cap.title.toStdString());
    INFO("modal text : " << cap.text.toStdString());
    CHECK(cap.text == blocked->message);
    CHECK(cap.title == blocked->title);
}

// ─── Digital Number Reader ───────────────────────────────────────────────────

TEST_CASE("Digit: a duplicate names the OTHER area holding the number",
          "[zone_refusal][ui][digit]") {
    CameraAreasPage page;
    page.resize(960, 640);
    page.set_background(frame());
    page.load({area("Area 1", 2, 0.10f), area("Area 2", 2, 0.55f)}, {});

    int emitted = 0;
    QObject::connect(&page, &CameraAreasPage::save_requested,
                     [&emitted](const std::vector<denso::camera::CameraArea>&) {
                         ++emitted;
                     });

    QAbstractButton* save = button_named(page, QStringLiteral("areasSave"));
    REQUIRE(save != nullptr);

    ModalCapture cap;
    QTimer* t = catch_modal(cap);
    save->click();
    t->stop();
    t->deleteLater();

    REQUIRE(cap.seen);
    INFO("modal title: " << cap.title.toStdString());
    INFO("modal text : " << cap.text.toStdString());
    CHECK(cap.title == QStringLiteral("Cannot save areas"));
    CHECK(cap.text.contains(QStringLiteral("Zone 2")));
    // The OTHER area is named — the one the operator is NOT editing. Loading
    // selects "Area 1", so the zone field reports who else holds 2, and telling
    // them "Area 1 holds it" would be reporting their own selection back at
    // them. "already used" alone would be true and useless; the point of the
    // sentence is to say where to go and change something.
    CHECK(cap.text.contains(QStringLiteral("Area 2")));
    CHECK(cap.text.contains(QStringLiteral("on this camera")));
    CHECK(cap.text.contains(QStringLiteral("Choose another Zone number")));
    CHECK(emitted == 0);
}

TEST_CASE("Digit: a zone held by another camera names that camera",
          "[zone_refusal][ui][digit]") {
    CameraAreasPage page;
    page.resize(960, 640);
    page.set_background(frame());
    page.load({area("Area 1", 5, 0.10f)}, {{5, "Line 2 Cam"}});

    int emitted = 0;
    QObject::connect(&page, &CameraAreasPage::save_requested,
                     [&emitted](const std::vector<denso::camera::CameraArea>&) {
                         ++emitted;
                     });

    ModalCapture cap;
    QTimer* t = catch_modal(cap);
    button_named(page, QStringLiteral("areasSave"))->click();
    t->stop();
    t->deleteLater();

    REQUIRE(cap.seen);
    INFO("modal title: " << cap.title.toStdString());
    INFO("modal text : " << cap.text.toStdString());
    CHECK(cap.text.contains(QStringLiteral("Zone 5")));
    CHECK(cap.text.contains(QStringLiteral("Line 2 Cam")));
    // NOT "on this camera" — it is held elsewhere, and saying otherwise would
    // send the operator hunting through the wrong camera's areas.
    CHECK_FALSE(cap.text.contains(QStringLiteral("on this camera")));
    CHECK(emitted == 0);
}

TEST_CASE("Digit: the SELECTED area's own bad number is what gets reported",
          "[zone_refusal][ui][digit]") {
    CameraAreasPage page;
    page.resize(960, 640);
    page.set_background(frame());
    // 0 is the one an operator trained on the old sentinel will try.
    page.load({area("Area 1", 0, 0.10f)}, {});

    int emitted = 0;
    QObject::connect(&page, &CameraAreasPage::save_requested,
                     [&emitted](const std::vector<denso::camera::CameraArea>&) {
                         ++emitted;
                     });

    ModalCapture cap;
    QTimer* t = catch_modal(cap);
    button_named(page, QStringLiteral("areasSave"))->click();
    t->stop();
    t->deleteLater();

    REQUIRE(cap.seen);
    INFO("modal title: " << cap.title.toStdString());
    INFO("modal text : " << cap.text.toStdString());
    CHECK(cap.title == QStringLiteral("Cannot save areas"));
    // Loading a single area selects it, so its stored 0 is painted into the zone
    // field — and the FIELD's refusal is the one that fires. That is the right
    // one to report: it is the control the operator is looking at, already red,
    // and it names the offending number and the legal range. The set-level
    // check exists for an area that is NOT selected, and would name it instead.
    CHECK(cap.text.contains(QStringLiteral("Zone 0")));
    CHECK(cap.text.contains(QString::number(denso::camera::kMinZone)));
    CHECK(cap.text.contains(QString::number(denso::camera::kMaxZone)));
    CHECK(cap.text.contains(QStringLiteral("Choose another Zone number")));
    CHECK(emitted == 0);
}

TEST_CASE("Digit: distinct zones save with no dialog at all",
          "[zone_refusal][ui][digit]") {
    CameraAreasPage page;
    page.resize(960, 640);
    page.set_background(frame());
    page.load({area("Area 1", 1, 0.10f), area("Area 2", 99, 0.55f)}, {});

    int emitted = 0;
    QObject::connect(&page, &CameraAreasPage::save_requested,
                     [&emitted](const std::vector<denso::camera::CameraArea>&) {
                         ++emitted;
                     });

    ModalCapture cap;
    QTimer* t = catch_modal(cap);
    button_named(page, QStringLiteral("areasSave"))->click();
    t->stop();
    t->deleteLater();

    CHECK_FALSE(cap.seen);
    CHECK(emitted == 1);
}
