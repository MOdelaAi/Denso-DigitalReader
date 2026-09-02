// The typed Zone-number field: what the operator may enter, and what the page
// is told about it.
//
// This widget replaced a combo box that could only offer legal values. Typing
// can express anything, so the acceptance rule moved from "what the list
// contains" to code — and every case below is a value a real operator can put in
// the box on a touch panel. Its verdict gates Save on both wizard steps, so a
// gap here is a save the repository refuses with no explanation on screen.
#include "ui/camera/dialog/zone_number_edit.h"

#include "camera/camera.h"

#include <catch2/catch_test_macros.hpp>

#include <QCheckBox>
#include <QLabel>
#include <QLineEdit>
#include <QString>

#include <map>
#include <optional>

using denso::ui::ZoneNumberEdit;

namespace {

QLineEdit* field(ZoneNumberEdit& w) {
    auto* e = w.findChild<QLineEdit*>(QStringLiteral("zoneNumberEdit"));
    REQUIRE(e != nullptr);
    return e;
}

QCheckBox* report_box(ZoneNumberEdit& w) {
    return w.findChild<QCheckBox*>(QStringLiteral("zoneReportCheck"));
}

QString used_text(ZoneNumberEdit& w) {
    auto* l = w.findChild<QLabel*>(QStringLiteral("zoneUsedLabel"));
    REQUIRE(l != nullptr);
    return l->text();
}

/// The operator-facing message under the field. Read off the LABEL, not off
/// problem(), so the tests prove what is actually on screen.
QString warning_text(ZoneNumberEdit& w) {
    auto* l = w.findChild<QLabel*>(QStringLiteral("zoneWarningLabel"));
    REQUIRE(l != nullptr);
    return l->text();
}

/// Counts zone_changed emissions. QSignalSpy would need Qt6::Test, which this
/// target does not link.
struct ChangeCounter {
    int count = 0;
    explicit ChangeCounter(ZoneNumberEdit& w) {
        QObject::connect(&w, &ZoneNumberEdit::zone_changed, &w,
                         [this](std::optional<int>) { ++count; });
    }
};

}  // namespace

TEST_CASE("the whole 1..99 range can be typed", "[zone_edit][ui]") {
    ZoneNumberEdit w(/*allow_unassigned=*/false);
    QLineEdit* e = field(w);

    for (const int legal : {1, 9, 10, 12, 45, 98, 99}) {
        e->setText(QString::number(legal));
        INFO("typed " << legal);
        CHECK(w.is_valid());
        REQUIRE(w.zone().has_value());
        CHECK(*w.zone() == legal);
        CHECK(warning_text(w).isEmpty());
    }
}

TEST_CASE("a value outside the range is refused and named", "[zone_edit][ui]") {
    ZoneNumberEdit w(/*allow_unassigned=*/false);
    QLineEdit* e = field(w);

    SECTION("zero") {
        // 0 is refused for the same reason 100 is: it is not in the range. It
        // used to mean "detection only", so an operator who learned that habit
        // must be TOLD, not silently given an unreported area.
        e->setText(QStringLiteral("0"));
        CHECK_FALSE(w.is_valid());
        CHECK(warning_text(w).contains(QStringLiteral("0")));
        CHECK_FALSE(w.zone().has_value());
    }
    SECTION("negative") {
        e->setText(QStringLiteral("-1"));
        CHECK_FALSE(w.is_valid());
        CHECK_FALSE(warning_text(w).isEmpty());
    }
    SECTION("one past the ceiling") {
        e->setText(QStringLiteral("100"));
        CHECK_FALSE(w.is_valid());
        CHECK(warning_text(w).contains(QStringLiteral("100")));
    }
    SECTION("far past the ceiling") {
        e->setText(QStringLiteral("4242"));
        CHECK_FALSE(w.is_valid());
    }
    SECTION("long enough to overflow an int") {
        // Guarded by the length cap before toInt(): without it this parses to
        // something in range and quietly becomes a legal zone.
        e->setText(QStringLiteral("100000000000000"));
        CHECK_FALSE(w.is_valid());
    }
}

TEST_CASE("a value that is not a whole number is refused", "[zone_edit][ui]") {
    ZoneNumberEdit w(/*allow_unassigned=*/false);
    QLineEdit* e = field(w);

    for (const char* junk : {"1.5", "0.0", "abc", "1 2", "1a", "+5", " -0", "٣"}) {
        e->setText(QString::fromUtf8(junk));
        INFO("typed " << junk);
        CHECK_FALSE(w.is_valid());
        CHECK_FALSE(warning_text(w).isEmpty());
    }
}

TEST_CASE("an empty field is invalid when a zone is required", "[zone_edit][ui]") {
    ZoneNumberEdit w(/*allow_unassigned=*/false);
    QLineEdit* e = field(w);

    CHECK_FALSE(w.is_valid());          // as constructed
    e->setText(QStringLiteral("7"));
    REQUIRE(w.is_valid());
    e->setText(QString());              // cleared again
    CHECK_FALSE(w.is_valid());
    // Whitespace is not a number either.
    e->setText(QStringLiteral("   "));
    CHECK_FALSE(w.is_valid());
}

TEST_CASE("unassigned is the checkbox, and no number spells it", "[zone_edit][ui]") {
    ZoneNumberEdit w(/*allow_unassigned=*/true);
    QCheckBox* box = report_box(w);
    REQUIRE(box != nullptr);
    QLineEdit* e = field(w);

    // Detection-only: valid, no zone, and the field is not even usable — the
    // operator cannot mistake this state for having typed a number.
    CHECK_FALSE(box->isChecked());
    CHECK(w.is_valid());
    CHECK_FALSE(w.zone().has_value());
    CHECK_FALSE(e->isEnabled());

    // Turning reporting ON does not stand in for a number: the field is empty
    // and the widget says so, so nothing can be saved until one is entered.
    box->setChecked(true);
    CHECK(e->isEnabled());
    CHECK_FALSE(w.is_valid());
    CHECK_FALSE(w.zone().has_value());

    // And 0 is NOT a way to get back to unassigned. Typing it with reporting on
    // is an error, not a second route to the state the checkbox already owns —
    // otherwise the same intent would have two spellings again, which is exactly
    // what v17 retired.
    e->setText(QStringLiteral("0"));
    CHECK_FALSE(w.is_valid());
    CHECK_FALSE(w.zone().has_value());

    // A real zone is accepted from that same state.
    e->setText(QStringLiteral("1"));
    CHECK(w.is_valid());
    REQUIRE(w.zone().has_value());
    CHECK(*w.zone() == 1);

    // Back to detection-only clears it to nullopt.
    box->setChecked(false);
    CHECK(w.is_valid());
    CHECK_FALSE(w.zone().has_value());
}

TEST_CASE("the used zones are listed, and only the used ones", "[zone_edit][ui]") {
    ZoneNumberEdit w(/*allow_unassigned=*/false);
    CHECK(used_text(w).contains(QStringLiteral("No zone numbers are in use")));

    w.set_unavailable({{1, QStringLiteral("Line 1")},
                       {2, QStringLiteral("Line 1")},
                       {5, QStringLiteral("Line 2")},
                       {12, QStringLiteral("Line 2")},
                       {24, QStringLiteral("Tank 1")}});

    // Ascending, and the numbers only — 99 mostly-free candidates would tell
    // the operator nothing about what to avoid.
    CHECK(used_text(w).contains(QStringLiteral("1, 2, 5, 12, 24")));
    CHECK_FALSE(used_text(w).contains(QStringLiteral("99")));
}

TEST_CASE("a duplicate is warned about immediately and names the holder",
          "[zone_edit][ui]") {
    ZoneNumberEdit w(/*allow_unassigned=*/false);
    w.set_unavailable({{1, QStringLiteral("Line 1")}, {24, QStringLiteral("Tank 1")}});
    QLineEdit* e = field(w);

    e->setText(QStringLiteral("1"));
    CHECK_FALSE(w.is_valid());
    CHECK(warning_text(w).contains(QStringLiteral("Line 1")));

    e->setText(QStringLiteral("24"));
    CHECK_FALSE(w.is_valid());
    CHECK(warning_text(w).contains(QStringLiteral("Tank 1")));

    // A free number clears it on the very next keystroke, not on Save.
    e->setText(QStringLiteral("25"));
    CHECK(w.is_valid());
    CHECK(warning_text(w).isEmpty());
}

TEST_CASE("the current owner may keep its own number", "[zone_edit][ui]") {
    // The caller EXCLUDES the target being edited from the unavailable map, so
    // re-saving an unchanged zone never reads as a clash with itself.
    ZoneNumberEdit w(/*allow_unassigned=*/false);
    w.set_unavailable({{7, QStringLiteral("another area here")}});

    QLineEdit* e = field(w);
    e->setText(QStringLiteral("7"));
    REQUIRE_FALSE(w.is_valid());        // held by someone else

    w.set_unavailable({});              // now editing the holder itself
    CHECK(w.is_valid());
    REQUIRE(w.zone().has_value());
    CHECK(*w.zone() == 7);
}

TEST_CASE("availability is re-judged when the taken set changes",
          "[zone_edit][ui]") {
    // A number typed while free can be claimed by another area a moment later.
    // Keeping the old verdict would let Save through on a value the repo rejects.
    ZoneNumberEdit w(/*allow_unassigned=*/false);
    QLineEdit* e = field(w);
    e->setText(QStringLiteral("3"));
    REQUIRE(w.is_valid());

    w.set_unavailable({{3, QStringLiteral("Line 9")}});
    CHECK_FALSE(w.is_valid());
    CHECK(warning_text(w).contains(QStringLiteral("Line 9")));
}

TEST_CASE("zone_changed reports only usable values", "[zone_edit][ui]") {
    ZoneNumberEdit w(/*allow_unassigned=*/false);
    ChangeCounter spy(w);
    QLineEdit* e = field(w);

    e->setText(QStringLiteral("4"));
    CHECK(spy.count == 1);

    // Typing on toward an illegal value must NOT push a half-typed number into
    // the area behind the field. "45" is legal and reports; "456" is not and
    // stays silent.
    e->setText(QStringLiteral("45"));
    CHECK(spy.count == 2);
    e->setText(QStringLiteral("456"));
    CHECK(spy.count == 2);
    CHECK_FALSE(w.is_valid());

    // Junk reports nothing either.
    e->setText(QStringLiteral("abc"));
    CHECK(spy.count == 2);
}

TEST_CASE("set_zone paints a value without reporting it back", "[zone_edit][ui]") {
    // Loading a selection is not an edit. If it emitted, selecting an area would
    // mark the page dirty and could write the zone back over itself.
    ZoneNumberEdit w(/*allow_unassigned=*/true);
    ChangeCounter spy(w);

    w.set_zone(12);
    CHECK(spy.count == 0);
    REQUIRE(w.zone().has_value());
    CHECK(*w.zone() == 12);
    CHECK(w.is_valid());
    CHECK(field(w)->text() == QStringLiteral("12"));
    CHECK(report_box(w)->isChecked());

    // A stored value the namespace no longer contains — a legacy 0 read from a
    // database written before v17 — is PAINTED and NAMED, not silently blanked
    // into detection-only. Blanking it would hide from the operator that their
    // saved configuration changed meaning under them.
    w.set_zone(0);
    CHECK(spy.count == 0);                          // painting is not an edit
    CHECK(field(w)->text() == QStringLiteral("0"));
    CHECK(report_box(w)->isChecked());
    CHECK_FALSE(w.is_valid());
    CHECK_FALSE(warning_text(w).isEmpty());

    w.set_zone(std::nullopt);
    CHECK(spy.count == 0);
    CHECK(field(w)->text().isEmpty());
    CHECK_FALSE(report_box(w)->isChecked());
    CHECK_FALSE(w.zone().has_value());
}

TEST_CASE("the field bound matches the core range authority", "[zone_edit][ui]") {
    // The widget must not carry its own copy of the bound. If core widens the
    // namespace, the top of the range has to become typeable with no edit here.
    ZoneNumberEdit w(/*allow_unassigned=*/false);
    QLineEdit* e = field(w);

    e->setText(QString::number(denso::camera::kMinZone));
    CHECK(w.is_valid());
    e->setText(QString::number(denso::camera::kMaxZone));
    CHECK(w.is_valid());
    // Both bounds are exclusive on the outside, and BOTH are read from core —
    // a widening in either direction must need no edit in this file.
    e->setText(QString::number(denso::camera::kMinZone - 1));
    CHECK_FALSE(w.is_valid());
    e->setText(QString::number(denso::camera::kMaxZone + 1));
    CHECK_FALSE(w.is_valid());
}
