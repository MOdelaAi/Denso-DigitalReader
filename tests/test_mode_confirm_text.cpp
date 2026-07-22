// Slice 5 — the pure confirmation-copy builder for the destructive Switch-and-
// Reset dialog (spec §7.1). mode_confirm_body is Qt-Core-only, so it is unit-
// tested here in the backend-free denso_tests and compiled into BOTH denso_tests
// and denso_app (like grid_layout.cpp). The copy must state, from REAL counts,
// what is KEPT (camera connections) and what is destroyed (processing setup) —
// and must NEVER imply the cameras/connections themselves are deleted.
#include <catch2/catch_test_macros.hpp>

#include "mode/mode.h"
#include "mode/reset.h"                    // mode::SwitchCounts
#include "ui/settings/mode_confirm_text.h"  // mode_confirm_body

#include <QString>

using denso::mode::SwitchCounts;
using denso::mode::TargetMode;

namespace {
// The plan's example counts (spec §7.1): 3 cameras, 3 model bindings, 7 areas,
// zones {3,4,5,7}, 1284 readings, 2 receipts.
SwitchCounts example_counts() {
    SwitchCounts c;
    c.cameras = 3;
    c.model_bindings = 3;
    c.areas = 7;
    c.zones = {3, 4, 5, 7};
    c.readings = 1284;
    c.receipts = 2;
    return c;
}
}  // namespace

TEST_CASE("confirmation body renders every real count", "[mode_confirm]") {
    const QString body =
        denso::ui::mode_confirm_body(TargetMode::BallLeveler, example_counts());

    // 1. camera count; 2. area count; 3. model-binding count; 5. receipt count.
    CHECK(body.contains(QStringLiteral("3 camera connections")));
    CHECK(body.contains(QStringLiteral("7 detection areas")));
    CHECK(body.contains(QStringLiteral("3 model bindings")));
    CHECK(body.contains(QStringLiteral("2 model-rollback receipts")));
    // 4. reading count uses grouped thousands (an explicit en-US locale, never the
    //    non-grouping C locale — which would render "1284").
    CHECK(body.contains(QStringLiteral("1,284")));
    CHECK_FALSE(body.contains(QStringLiteral("1284")));
    // 6. the distinct zone numbers are shown (count + the list).
    CHECK(body.contains(QStringLiteral("4 reported zones (3, 4, 5, 7)")));
}

TEST_CASE("confirmation body states connections are KEPT and setup is destroyed",
          "[mode_confirm]") {
    const QString body =
        denso::ui::mode_confirm_body(TargetMode::BallLeveler, example_counts());

    // 7. camera connections are explicitly stated as kept, with preserved fields.
    CHECK(body.contains(QStringLiteral("camera connections will be kept")));
    CHECK(body.contains(QStringLiteral(
        "sources, credentials, resolution and orientation are preserved")));
    // The processing setup being deleted names the mode being LEFT (digit_reader).
    CHECK(body.contains(QStringLiteral("Digital Number Reader setup will be deleted")));
    CHECK(body.contains(QStringLiteral("processing setup again")));
    // 8. reporting is explicitly stated as disabled.
    CHECK(body.contains(QStringLiteral("reporting will be turned off")));
    // 9. the server URL/address is explicitly stated as retained + manual re-enable.
    CHECK(body.contains(QStringLiteral("server address is kept")));
    CHECK(body.contains(QStringLiteral("re-enable reporting yourself")));
    // 10. the operation is stated as irreversible.
    CHECK(body.contains(QStringLiteral("cannot be undone")));
}

TEST_CASE("Ball Leveler unavailability appears ONLY for the Ball Leveler target",
          "[mode_confirm]") {
    // 11. present for ball_leveler...
    const QString to_ball =
        denso::ui::mode_confirm_body(TargetMode::BallLeveler, example_counts());
    CHECK(to_ball.contains(QStringLiteral("not available in this release")));
    CHECK(to_ball.contains(QStringLiteral("Switch to Floating Ball Leveler?")));

    // ...and absent for digit_reader (the leaving mode is then ball_leveler).
    const QString to_digit =
        denso::ui::mode_confirm_body(TargetMode::DigitReader, example_counts());
    CHECK_FALSE(to_digit.contains(QStringLiteral("not available in this release")));
    CHECK(to_digit.contains(QStringLiteral("Switch to Digital Number Reader?")));
    CHECK(to_digit.contains(QStringLiteral("Floating Ball Leveler setup will be deleted")));
    // Connections are still kept regardless of direction.
    CHECK(to_digit.contains(QStringLiteral("camera connections will be kept")));
}

TEST_CASE("confirmation body never implies the cameras themselves are deleted",
          "[mode_confirm]") {
    // 12. The valid copy legitimately contains both "camera" and "deleted" (the
    //     PROCESSING setup is deleted), so we reject exact forbidden implications,
    //     NOT a naive camera+delete co-occurrence.
    for (const TargetMode t : {TargetMode::BallLeveler, TargetMode::DigitReader}) {
        const QString body = denso::ui::mode_confirm_body(t, example_counts());
        CHECK_FALSE(body.contains(QStringLiteral("camera connections will be deleted")));
        CHECK_FALSE(body.contains(QStringLiteral("delete cameras")));
        CHECK_FALSE(body.contains(QStringLiteral("remove camera connections")));
        CHECK_FALSE(body.contains(QStringLiteral("cameras will be deleted")));
    }
}

TEST_CASE("zero zones renders a count with no empty parenthesis", "[mode_confirm]") {
    SwitchCounts c = example_counts();
    c.zones.clear();
    const QString body = denso::ui::mode_confirm_body(TargetMode::BallLeveler, c);
    CHECK(body.contains(QStringLiteral("0 reported zones")));
    CHECK_FALSE(body.contains(QStringLiteral("()")));
}
