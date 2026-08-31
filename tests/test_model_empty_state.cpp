// The Models step's empty-state copy — pure, no widgets, no database.
//
// The property under test is NARROW and deliberate: this unit turns the policy's
// own stable reason codes into operator-facing text, and does NOT decide which
// model belongs to which mode. Every test therefore either (a) checks that a code
// the policy really emits produces a sentence containing that code verbatim, or
// (b) checks that the unit refuses to invent a reason it was not given.
//
// The reason codes asserted here are the exact string literals in
// src/core/models/compatibility.cpp. A test below reads that file and fails if a
// reject() code exists there with no arm here — the pairing is machine-checked,
// not maintained by memory.
#include <catch2/catch_test_macros.hpp>

#include "mode/mode.h"
#include "ui/camera/dialog/model_empty_state.h"

#include <QByteArray>
#include <QFile>
#include <QRegularExpression>
#include <QSet>
#include <QString>

#include <vector>

using denso::mode::TargetMode;
using denso::ui::model_empty_state_text;
using denso::ui::model_reason_text;
using denso::ui::RejectedModelNote;

namespace {

RejectedModelNote note(const char* display, const char* code) {
    return RejectedModelNote{QString::fromLatin1(display), std::string(code)};
}

}  // namespace

// ═════════════════════════════════════════════════════════════════════════════
// The empty state is never empty, and always names the mode.
// ═════════════════════════════════════════════════════════════════════════════

TEST_CASE("an empty catalog reports an empty catalog, not a mode problem",
          "[model_empty_state]") {
    const QString t = model_empty_state_text(TargetMode::DigitReader, {}, true);
    REQUIRE_FALSE(t.isEmpty());
    CHECK(t.contains(QStringLiteral("No detection models are installed")));
    CHECK(t.contains(QStringLiteral("digit_reader")));
    // Must NOT tell the operator to go change modes: no model exists in EITHER
    // mode, so "no compatible models for <mode>" would send them somewhere useless.
    CHECK_FALSE(t.contains(QStringLiteral("No compatible models")));
}

TEST_CASE("a missing manifest is reported as model_undeclared, verbatim",
          "[model_empty_state]") {
    const QString t = model_empty_state_text(
        TargetMode::DigitReader, {note("digitv3.engine (catalog #1)", "model_undeclared")}, true);
    CHECK(t.contains(QStringLiteral("No compatible models for digit_reader")));
    CHECK(t.contains(QStringLiteral("digitv3.engine (catalog #1)")));
    // The stable code itself must survive into the text: it is the token the
    // operator quotes and the log confirms.
    CHECK(t.contains(QStringLiteral("model_undeclared")));
    CHECK(t.contains(QStringLiteral("manifest")));
}

TEST_CASE("a provenance failure is reported as provenance, not as a mode fault",
          "[model_empty_state]") {
    const QString t = model_empty_state_text(
        TargetMode::BallLeveler,
        {note("float-small.engine (catalog #2)", "model_provenance_failed"),
         note("float-big.engine (catalog #3)", "model_provenance_failed")},
        false);
    CHECK(t.contains(QStringLiteral("ball_leveler")));
    CHECK(t.contains(QStringLiteral("model_provenance_failed")));
    CHECK(t.contains(QStringLiteral("float-small.engine (catalog #2)")));
    CHECK(t.contains(QStringLiteral("float-big.engine (catalog #3)")));
    CHECK(t.contains(QStringLiteral("approved artifact")));
    // Both models are named AND counted — a truncated list must not read as the
    // whole story.
    CHECK(t.contains(QStringLiteral("2 models in the catalog are")));
}

TEST_CASE("a wrong-mode model says so, and names the mode it is refused in",
          "[model_empty_state]") {
    const QString t = model_empty_state_text(
        TargetMode::BallLeveler,
        {note("digitv3.engine (catalog #1)", "model_mode_incompatible")}, false);
    CHECK(t.contains(QStringLiteral("No compatible models for ball_leveler")));
    CHECK(t.contains(QStringLiteral("model_mode_incompatible")));
    CHECK(t.contains(QStringLiteral("not allowed in this operating mode")));
    // Singular grammar, so a one-model catalog does not read as machine output.
    CHECK(t.contains(QStringLiteral("1 model in the catalog is")));
}

TEST_CASE("mixed reasons are reported per model, not collapsed to one cause",
          "[model_empty_state]") {
    const QString t = model_empty_state_text(
        TargetMode::DigitReader,
        {note("float-small.engine (catalog #2)", "model_mode_incompatible"),
         note("weird.engine (catalog #7)", "model_provenance_failed")},
        false);
    CHECK(t.contains(QStringLiteral("model_mode_incompatible")));
    CHECK(t.contains(QStringLiteral("model_provenance_failed")));
    CHECK(t.contains(QStringLiteral("2 models in the catalog are")));
}

TEST_CASE("an unrecognised reason code is passed through, never guessed at",
          "[model_empty_state]") {
    // Fail-honest: a code this unit does not know must still reach the operator
    // rather than being replaced with a plausible-sounding wrong explanation.
    CHECK(model_reason_text("model_something_new_2030") ==
          QStringLiteral("model_something_new_2030"));
    const QString t = model_empty_state_text(
        TargetMode::DigitReader,
        {note("x.engine (catalog #9)", "model_something_new_2030")}, false);
    CHECK(t.contains(QStringLiteral("model_something_new_2030")));
}

// ═════════════════════════════════════════════════════════════════════════════
// The remedy line: offered for the one code it actually fixes, and no other.
// ═════════════════════════════════════════════════════════════════════════════

TEST_CASE("an undeclared model with NO manifest names the command that seeds one",
          "[model_empty_state]") {
    // The incident this closes: a fresh appliance showed three undeclared models
    // and no way to act on them. The message stated the fault and stopped.
    const QString t = model_empty_state_text(
        TargetMode::DigitReader,
        {note("digitv3.engine (catalog #1)", "model_undeclared"),
         note("float-big.engine (catalog #2)", "model_undeclared")},
        /*manifest_declares_nothing=*/true);
    CHECK(t.contains(QStringLiteral("sudo denso-setup seed-manifest")));
    // The diagnosis must survive alongside the remedy, not be replaced by it.
    CHECK(t.contains(QStringLiteral("model_undeclared")));
}

TEST_CASE("an undeclared model is NOT offered seeding when a manifest already loaded",
          "[model_empty_state]") {
    // The counterexample that matters: `model_undeclared` says "the manifest does
    // not cover this artifact", which a perfectly good manifest also says about an
    // engine the operator dropped in themselves. seed-manifest inspects the same
    // state and REFUSES (data-artifact-orphan / target-differs), so recommending
    // it here would hand the operator a command that deterministically fails.
    const QString t = model_empty_state_text(
        TargetMode::DigitReader,
        {note("mine.engine (catalog #4)", "model_undeclared")},
        /*manifest_declares_nothing=*/false);
    CHECK(t.contains(QStringLiteral("model_undeclared")));
    CHECK_FALSE(t.contains(QStringLiteral("seed-manifest")));
}

TEST_CASE("the remedy is offered for a MIXED list containing an undeclared model",
          "[model_empty_state]") {
    // One undeclared model against an empty manifest is enough: seeding is a
    // real, correct action for that row even though it does nothing for the other.
    const QString t = model_empty_state_text(
        TargetMode::DigitReader,
        {note("a.engine (catalog #1)", "model_provenance_failed"),
         note("b.engine (catalog #2)", "model_undeclared")},
        /*manifest_declares_nothing=*/true);
    CHECK(t.contains(QStringLiteral("sudo denso-setup seed-manifest")));
}

TEST_CASE("the remedy is NOT offered for failures seeding cannot fix",
          "[model_empty_state]") {
    // seed-manifest would correctly refuse for each of these. Offered even with
    // an empty manifest, the suggestion would still be wrong: none of these rows
    // becomes selectable because a manifest arrived.
    for (const char* code : {"model_provenance_failed", "model_mode_incompatible",
                             "model_unknown_id", "model_family_mismatch",
                             "model_shape_unsupported", "model_classes_mismatch"}) {
        for (bool empty_manifest : {true, false}) {
            const QString t = model_empty_state_text(
                TargetMode::BallLeveler, {note("x.engine (catalog #1)", code)},
                empty_manifest);
            INFO("remedy wrongly offered for: " << code
                 << " (manifest_declares_nothing=" << empty_manifest << ")");
            CHECK_FALSE(t.contains(QStringLiteral("seed-manifest")));
        }
    }
}

TEST_CASE("an empty catalog is not offered the seeding remedy",
          "[model_empty_state]") {
    // Nothing to declare: there are no artifacts, so a manifest describing none
    // of them changes nothing the operator can see.
    const QString t = model_empty_state_text(TargetMode::DigitReader, {}, true);
    CHECK_FALSE(t.contains(QStringLiteral("seed-manifest")));
}

// ═════════════════════════════════════════════════════════════════════════════
// The unit holds NO compatibility matrix. These are the mutation guards.
// ═════════════════════════════════════════════════════════════════════════════

TEST_CASE("every reason code the policy can emit has an explanation here",
          "[model_empty_state][structural]") {
    // Reads the ONE authoritative policy file and extracts every reject() code.
    // A new reject() in compatibility.cpp with no arm in model_empty_state.cpp
    // fails HERE, at the seam, instead of shipping a raw token to an operator.
    QFile f(QStringLiteral(DENSO_SOURCE_DIR "/src/core/models/compatibility.cpp"));
    REQUIRE(f.open(QIODevice::ReadOnly));
    const QString src = QString::fromUtf8(f.readAll());

    QRegularExpression re(QStringLiteral("reject\\(Verdict::\\w+,\\s*\"([a-z_]+)\"\\)"));
    QSet<QString> codes;
    auto it = re.globalMatch(src);
    while (it.hasNext()) codes.insert(it.next().captured(1));
    REQUIRE(codes.size() >= 7);   // the policy's seven documented rejections

    for (const QString& code : codes) {
        const QString text = model_reason_text(code.toStdString());
        INFO("reason code without an explanation: " << code.toStdString());
        // Pass-through means "unexplained": the text would equal the code itself.
        CHECK(text != code);
        CHECK_FALSE(text.isEmpty());
    }
}

TEST_CASE("the empty-state unit names no model and no family",
          "[model_empty_state][structural]") {
    // The second-matrix guard. This unit may name a MODE (it is telling the
    // operator which mode they are in) but must never name a model id or family:
    // that would be an authorization rule leaking out of compatibility.cpp.
    QFile f(QStringLiteral(DENSO_SOURCE_DIR
                           "/src/app/ui/camera/dialog/model_empty_state.cpp"));
    REQUIRE(f.open(QIODevice::ReadOnly));
    const QString src = QString::fromUtf8(f.readAll());
    for (const char* forbidden :
         {"digitv3", "float-small", "float-big", "digit_numeric", "float_ball"}) {
        INFO("forbidden token in model_empty_state.cpp: " << forbidden);
        CHECK_FALSE(src.contains(QString::fromLatin1(forbidden)));
    }
}
