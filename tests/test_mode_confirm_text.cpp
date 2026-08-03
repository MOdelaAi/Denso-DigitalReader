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

TEST_CASE("the confirmation states deletion and irreversibility",
          "[mode_confirm]") {
    // MUTATION GUARD, inverted at the operator decision of 2026-07-31: the switch
    // IS destructive again, so the copy that must never come back is the
    // reassuring one. Both directions are checked, for both targets.
    for (auto target : {TargetMode::BallLeveler, TargetMode::DigitReader}) {
        const QString body = denso::ui::mode_confirm_body(target);
        CHECK(body.contains(QStringLiteral("cannot be undone")));
        CHECK(body.contains(QStringLiteral("will be deleted")));
        CHECK(body.contains(QStringLiteral("start unconfigured")));
        // The superseded promises must be gone, not merely outweighed: an
        // operator who reads "Nothing is deleted" stops reading.
        CHECK_FALSE(body.contains(QStringLiteral("Nothing is deleted")));
        CHECK_FALSE(body.contains(QStringLiteral("kept exactly as")));
    }
}

TEST_CASE("the confirmation states camera connections are preserved",
          "[mode_confirm]") {
    const QString body = denso::ui::mode_confirm_body(TargetMode::BallLeveler);
    CHECK(body.contains(QStringLiteral("camera connections are kept")));
    CHECK(body.contains(QStringLiteral("credentials")));
}

TEST_CASE("the confirmation names what the leaving mode loses",
          "[mode_confirm]") {
    // Leaving digit_reader: the digit workspace is named as DELETED, in the
    // operator's own nouns. Naming it matters — "the configured setup" is not a
    // phrase anyone recognises as the areas they drew.
    const QString to_ball = denso::ui::mode_confirm_body(TargetMode::BallLeveler);
    CHECK(to_ball.contains(QStringLiteral("Digital Number Reader setup")));
    CHECK(to_ball.contains(QStringLiteral("detection areas")));
    CHECK(to_ball.contains(QStringLiteral("number format")));
    CHECK(to_ball.contains(QStringLiteral("will be deleted")));
    CHECK(to_ball.contains(QStringLiteral("up again from the beginning")));

    // …and the mirror image on the way back: Ball loses its zones and calibration.
    const QString to_digit = denso::ui::mode_confirm_body(TargetMode::DigitReader);
    CHECK(to_digit.contains(QStringLiteral("Floating Ball Leveler setup")));
    CHECK(to_digit.contains(QStringLiteral("level zones")));
    CHECK(to_digit.contains(QStringLiteral("calibration")));
    CHECK(to_digit.contains(QStringLiteral("will be deleted")));
}

TEST_CASE("the confirmation never promises processing resumes on its own",
          "[mode_confirm]") {
    // The destination opens UNCONFIGURED, so processing cannot resume by itself —
    // there is nothing left for it to run. The old copy promised exactly that,
    // and repeating it under a destructive switch would be the most damaging
    // sentence in the dialog: the operator would wait for a recovery that never
    // comes instead of setting the mode up.
    for (auto target : {TargetMode::BallLeveler, TargetMode::DigitReader}) {
        const QString body = denso::ui::mode_confirm_body(target);
        CHECK_FALSE(body.contains(QStringLiteral("starts again on its own")));
        CHECK_FALSE(body.contains(QStringLiteral("Processing pauses")));
        CHECK(body.contains(QStringLiteral("start unconfigured")));
    }
}

TEST_CASE("the confirmation states reporting is disabled and the address kept",
          "[mode_confirm]") {
    const QString body = denso::ui::mode_confirm_body(TargetMode::DigitReader);
    CHECK(body.contains(QStringLiteral("reporting will be turned off")));
    CHECK(body.contains(QStringLiteral("server address")));
    CHECK(body.contains(QStringLiteral("re-enable it yourself")));
    // The address surviving is the ONE reassurance the copy still makes, and it
    // must not be allowed to grow into a general one.
    CHECK(body.contains(QStringLiteral("Nothing else is")));
}

TEST_CASE("the confirmation no longer calls any mode unavailable",
          "[mode_confirm]") {
    // ACTIVATION. The Slice-1 guard paragraph is gone, and it must not creep
    // back: it warned that the destination had no runtime, and both modes now
    // have one. A stray reintroduction would tell an operator the feature they
    // are about to use does not exist.
    //
    // Asserted for BOTH targets, so the sentence cannot return for either.
    CHECK_FALSE(denso::ui::mode_confirm_body(TargetMode::BallLeveler)
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
