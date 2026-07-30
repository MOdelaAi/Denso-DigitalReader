// Slice 1 (revised) — the pure confirmation-copy builder for the Switch Target
// Mode dialog. mode_confirm_body is Qt-Core-only, so it is unit-tested here in
// the backend-free denso_tests and compiled into BOTH denso_tests and denso_app
// (like grid_layout.cpp).
//
// The copy's contract CHANGED with the non-destructive switch. It must now state
// what is PRESERVED — camera connections, both modes' configuration — that
// processing pauses while the target mode is prepared, and that reporting is
// disabled. It must NEVER promise deletion or irreversibility: the switch does
// neither, and a dialog that says otherwise would frighten an operator out of a
// safe, reversible action.
#include <catch2/catch_test_macros.hpp>

#include "mode/mode.h"
#include "ui/settings/mode_confirm_text.h"  // mode_confirm_body

#include <QString>

using denso::mode::TargetMode;

TEST_CASE("the confirmation promises no deletion and no irreversibility",
          "[mode_confirm]") {
    // MUTATION GUARD: this is the assertion that fails if the destructive copy is
    // ever restored. Both directions are checked, for both targets.
    for (auto target : {TargetMode::BallLeveler, TargetMode::DigitReader}) {
        const QString body = denso::ui::mode_confirm_body(target);
        CHECK_FALSE(body.contains(QStringLiteral("cannot be undone")));
        CHECK_FALSE(body.contains(QStringLiteral("will be deleted")));
        CHECK_FALSE(body.contains(QStringLiteral("destroy"), Qt::CaseInsensitive));
        CHECK_FALSE(body.contains(QStringLiteral("erase"), Qt::CaseInsensitive));
        CHECK_FALSE(body.contains(QStringLiteral("permanent"), Qt::CaseInsensitive));
        CHECK_FALSE(body.contains(QStringLiteral("setup again"), Qt::CaseInsensitive));
    }
}

TEST_CASE("the confirmation states camera connections are preserved",
          "[mode_confirm]") {
    const QString body = denso::ui::mode_confirm_body(TargetMode::BallLeveler);
    CHECK(body.contains(QStringLiteral("camera connections are kept")));
    CHECK(body.contains(QStringLiteral("credentials")));
}

TEST_CASE("the confirmation states BOTH modes' configuration is preserved",
          "[mode_confirm]") {
    // Leaving digit_reader: the digit workspace is named as kept, and so is any
    // Ball work already done. Naming BOTH matters — "nothing is deleted" alone
    // reads as "nothing happens".
    const QString to_ball = denso::ui::mode_confirm_body(TargetMode::BallLeveler);
    CHECK(to_ball.contains(QStringLiteral("Nothing is deleted")));
    CHECK(to_ball.contains(QStringLiteral("Digital Number Reader setup")));
    CHECK(to_ball.contains(QStringLiteral("Floating Ball Leveler setup you have")));
    CHECK(to_ball.contains(QStringLiteral("switch back")));

    // …and the mirror image on the way back.
    const QString to_digit = denso::ui::mode_confirm_body(TargetMode::DigitReader);
    CHECK(to_digit.contains(QStringLiteral("Floating Ball Leveler setup")));
    CHECK(to_digit.contains(QStringLiteral("Digital Number Reader setup you have")));
}

TEST_CASE("the confirmation discloses the processing pause honestly per target",
          "[mode_confirm]") {
    // digit_reader genuinely resumes, so it may say so.
    const QString to_digit = denso::ui::mode_confirm_body(TargetMode::DigitReader);
    CHECK(to_digit.contains(QStringLiteral("Processing pauses")));
    CHECK(to_digit.contains(QStringLiteral("starts again on its own")));

    // ball_leveler does NOT resume in this release - it lands on the guarded
    // "not available" page. Promising automatic resumption there would be a
    // straight falsehood, so the copy must only claim the stop.
    const QString to_ball = denso::ui::mode_confirm_body(TargetMode::BallLeveler);
    CHECK(to_ball.contains(QStringLiteral("Processing stops")));
    CHECK(to_ball.contains(QStringLiteral("Floating Ball Leveler is prepared")));
    CHECK_FALSE(to_ball.contains(QStringLiteral("starts again on its own")));
}

TEST_CASE("the confirmation states reporting is disabled and the address kept",
          "[mode_confirm]") {
    const QString body = denso::ui::mode_confirm_body(TargetMode::DigitReader);
    CHECK(body.contains(QStringLiteral("reporting will be turned off")));
    CHECK(body.contains(QStringLiteral("server address is kept")));
    CHECK(body.contains(QStringLiteral("re-enable reporting yourself")));
}

TEST_CASE("Ball Leveler unavailability appears ONLY for the Ball Leveler target",
          "[mode_confirm]") {
    // GUARD (Slice 1): the wizard is still not operator-accessible, and the copy
    // must say so before the operator commits — but only when that is the target.
    CHECK(denso::ui::mode_confirm_body(TargetMode::BallLeveler)
              .contains(QStringLiteral("not available in this release")));
    CHECK_FALSE(denso::ui::mode_confirm_body(TargetMode::DigitReader)
                    .contains(QStringLiteral("not available in this release")));
}

TEST_CASE("the confirmation names the destination in its question", "[mode_confirm]") {
    CHECK(denso::ui::mode_confirm_body(TargetMode::BallLeveler)
              .startsWith(QStringLiteral("Switch to Floating Ball Leveler?")));
    CHECK(denso::ui::mode_confirm_body(TargetMode::DigitReader)
              .startsWith(QStringLiteral("Switch to Digital Number Reader?")));
}
