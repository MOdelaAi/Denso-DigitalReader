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
    const QString t = model_empty_state_text(TargetMode::DigitReader, {});
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
        TargetMode::DigitReader, {note("digitv3.engine (catalog #1)", "model_undeclared")});
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
         note("float-big.engine (catalog #3)", "model_provenance_failed")});
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
        {note("digitv3.engine (catalog #1)", "model_mode_incompatible")});
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
         note("weird.engine (catalog #7)", "model_provenance_failed")});
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
        TargetMode::DigitReader, {note("x.engine (catalog #9)", "model_something_new_2030")});
    CHECK(t.contains(QStringLiteral("model_something_new_2030")));
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
