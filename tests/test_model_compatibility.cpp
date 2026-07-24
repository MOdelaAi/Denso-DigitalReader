// Slice 6 — the central model/mode compatibility policy.
//
// The registry lives in exactly one production source file
// (src/core/models/compatibility.cpp). This test contains table-driven EXPECTED
// cases only; it must NOT re-implement the authorization logic as a second copy
// of the matrix. Every expectation below is written out by hand.
#include <catch2/catch_test_macros.hpp>

#include "models/compatibility.h"

#include <string>
#include <vector>

using denso::mode::TargetMode;
using denso::models::CompatibilityResult;
using denso::models::loadable_model_files;
using denso::models::model_compatibility;
using denso::models::ModelMetadata;
using denso::models::Verdict;

namespace {

// A fully-valid, declared, corroborated metadata for one of the registered
// models. Tests mutate a single field to build each negative case, so the fault
// under test is the ONLY difference from an Allowed baseline.
ModelMetadata good(const std::string& canonical_id, const std::string& family,
                   const std::string& filename) {
    ModelMetadata m;
    m.canonical_id = canonical_id;
    m.family = family;
    m.task = "detect";
    m.input_size = 640;
    m.class_names = {"a"};
    m.class_count = 1;
    m.filename = filename;
    m.declared = true;
    m.artifact_matches = true;
    m.provenance_ok = true;
    return m;
}

ModelMetadata digitv3() { return good("digitv3", "digit_numeric", "digitv3.engine"); }
ModelMetadata float_small() { return good("float-small", "float_ball", "float-small.engine"); }
ModelMetadata float_big() { return good("float-big", "float_ball", "float-big.engine"); }

}  // namespace

// ─── the (mode x model) matrix ───────────────────────────────────────────────

TEST_CASE("digit_reader allows digitv3 only", "[compat]") {
    auto r = model_compatibility(TargetMode::DigitReader, digitv3());
    REQUIRE(r.verdict == Verdict::Allowed);
    REQUIRE(r.reason_code == "model_allowed");
    REQUIRE(r.allowed());
}

TEST_CASE("digit_reader rejects the float models as wrong-mode", "[compat]") {
    for (const auto& m : {float_small(), float_big()}) {
        auto r = model_compatibility(TargetMode::DigitReader, m);
        REQUIRE(r.verdict == Verdict::RejectedWrongMode);
        REQUIRE(r.reason_code == "model_mode_incompatible");
        REQUIRE_FALSE(r.allowed());
    }
}

TEST_CASE("ball_leveler allows both float models", "[compat]") {
    for (const auto& m : {float_small(), float_big()}) {
        auto r = model_compatibility(TargetMode::BallLeveler, m);
        REQUIRE(r.verdict == Verdict::Allowed);
        REQUIRE(r.reason_code == "model_allowed");
    }
}

TEST_CASE("ball_leveler rejects digitv3 as wrong-mode", "[compat]") {
    auto r = model_compatibility(TargetMode::BallLeveler, digitv3());
    REQUIRE(r.verdict == Verdict::RejectedWrongMode);
    REQUIRE(r.reason_code == "model_mode_incompatible");
}

// No model is allowed in both modes — the mutation "digitv3 also allows
// ball_leveler" is killed by the ball_leveler+digitv3 case above; this pins the
// complementary property for the float family too.
TEST_CASE("no registered model is allowed in both modes", "[compat]") {
    // digitv3 is allowed in digit_reader; the float models in ball_leveler. Each is
    // allowed in exactly one mode — never both.
    REQUIRE(model_compatibility(TargetMode::DigitReader, digitv3()).allowed());
    REQUIRE_FALSE(model_compatibility(TargetMode::BallLeveler, digitv3()).allowed());
    REQUIRE(model_compatibility(TargetMode::BallLeveler, float_small()).allowed());
    REQUIRE_FALSE(model_compatibility(TargetMode::DigitReader, float_small()).allowed());
    REQUIRE(model_compatibility(TargetMode::BallLeveler, float_big()).allowed());
    REQUIRE_FALSE(model_compatibility(TargetMode::DigitReader, float_big()).allowed());
}

// ─── rejection branches (both modes where applicable) ────────────────────────

TEST_CASE("undeclared metadata is RejectedUnknown in both modes", "[compat]") {
    for (auto mode : {TargetMode::DigitReader, TargetMode::BallLeveler}) {
        auto m = digitv3();
        m.declared = false;
        auto r = model_compatibility(mode, m);
        REQUIRE(r.verdict == Verdict::RejectedUnknown);
        REQUIRE(r.reason_code == "model_undeclared");
    }
}

TEST_CASE("an unknown canonical_id is RejectedUnknown in both modes", "[compat]") {
    for (auto mode : {TargetMode::DigitReader, TargetMode::BallLeveler}) {
        auto m = good("mystery", "digit_numeric", "mystery.engine");
        auto r = model_compatibility(mode, m);
        REQUIRE(r.verdict == Verdict::RejectedUnknown);
        REQUIRE(r.reason_code == "model_unknown_id");
    }
}

TEST_CASE("a declared family that disagrees with the registry is rejected", "[compat]") {
    auto m = digitv3();
    m.family = "float_ball";  // digitv3 is digit_numeric in the registry
    auto r = model_compatibility(TargetMode::DigitReader, m);
    REQUIRE(r.verdict == Verdict::RejectedUnknown);
    REQUIRE(r.reason_code == "model_family_mismatch");
}

TEST_CASE("a wrong task is a shape rejection", "[compat]") {
    auto m = digitv3();
    m.task = "classify";
    auto r = model_compatibility(TargetMode::DigitReader, m);
    REQUIRE(r.verdict == Verdict::RejectedMetadataMismatch);
    REQUIRE(r.reason_code == "model_shape_unsupported");
}

TEST_CASE("a wrong input size is a shape rejection", "[compat]") {
    auto m = digitv3();
    m.input_size = 416;
    auto r = model_compatibility(TargetMode::DigitReader, m);
    REQUIRE(r.verdict == Verdict::RejectedMetadataMismatch);
    REQUIRE(r.reason_code == "model_shape_unsupported");
}

TEST_CASE("a failed artifact corroboration is a classes mismatch", "[compat]") {
    // artifact_matches is a resolved bool: reordered / renamed / miscounted class
    // names all arrive here as false. The policy reports one reason for all.
    auto m = digitv3();
    m.artifact_matches = false;
    auto r = model_compatibility(TargetMode::DigitReader, m);
    REQUIRE(r.verdict == Verdict::RejectedMetadataMismatch);
    REQUIRE(r.reason_code == "model_classes_mismatch");
}

TEST_CASE("a provenance failure is reported as such", "[compat]") {
    auto m = digitv3();
    m.provenance_ok = false;
    auto r = model_compatibility(TargetMode::DigitReader, m);
    REQUIRE(r.verdict == Verdict::RejectedProvenance);
    REQUIRE(r.reason_code == "model_provenance_failed");
}

// ─── precedence: the FIRST matching rule wins ────────────────────────────────

TEST_CASE("undeclared beats wrong-mode", "[compat]") {
    // A float model in digit_reader is wrong-mode, but undeclared is checked first.
    auto m = float_small();
    m.declared = false;
    auto r = model_compatibility(TargetMode::DigitReader, m);
    REQUIRE(r.reason_code == "model_undeclared");
}

TEST_CASE("unknown id beats bad metadata", "[compat]") {
    auto m = good("mystery", "digit_numeric", "mystery.engine");
    m.task = "classify";
    m.artifact_matches = false;
    auto r = model_compatibility(TargetMode::DigitReader, m);
    REQUIRE(r.reason_code == "model_unknown_id");
}

TEST_CASE("family mismatch beats provenance failure", "[compat]") {
    auto m = digitv3();
    m.family = "float_ball";
    m.provenance_ok = false;
    auto r = model_compatibility(TargetMode::DigitReader, m);
    REQUIRE(r.reason_code == "model_family_mismatch");
}

TEST_CASE("wrong shape beats classes mismatch", "[compat]") {
    auto m = digitv3();
    m.input_size = 320;
    m.artifact_matches = false;
    auto r = model_compatibility(TargetMode::DigitReader, m);
    REQUIRE(r.reason_code == "model_shape_unsupported");
}

TEST_CASE("classes mismatch beats provenance failure", "[compat]") {
    auto m = digitv3();
    m.artifact_matches = false;
    m.provenance_ok = false;
    auto r = model_compatibility(TargetMode::DigitReader, m);
    REQUIRE(r.reason_code == "model_classes_mismatch");
}

TEST_CASE("provenance failure beats wrong mode", "[compat]") {
    // digitv3 in ball_leveler is wrong-mode, but provenance is checked first.
    auto m = digitv3();
    m.provenance_ok = false;
    auto r = model_compatibility(TargetMode::BallLeveler, m);
    REQUIRE(r.reason_code == "model_provenance_failed");
}

// ─── default behaviour: fail closed ──────────────────────────────────────────

TEST_CASE("a default-constructed result denies", "[compat]") {
    REQUIRE_FALSE(CompatibilityResult{}.allowed());
    REQUIRE(CompatibilityResult{}.verdict == Verdict::RejectedUnknown);
}

// ─── reason-code stability — these strings are a file format (status.json) ────

TEST_CASE("every reason code is exact, unique and lowercase snake case", "[compat]") {
    // The exact expected strings, written out. A rename anywhere in the policy —
    // even a well-intentioned one — breaks this test, which is the point.
    const std::vector<std::string> expected = {
        "model_undeclared",     "model_unknown_id",       "model_family_mismatch",
        "model_shape_unsupported", "model_classes_mismatch", "model_provenance_failed",
        "model_mode_incompatible", "model_allowed",
    };

    // uniqueness + shape
    for (size_t i = 0; i < expected.size(); ++i) {
        REQUIRE_FALSE(expected[i].empty());
        for (char c : expected[i])
            REQUIRE(((c >= 'a' && c <= 'z') || c == '_'));  // lowercase snake only
        for (size_t j = i + 1; j < expected.size(); ++j)
            REQUIRE(expected[i] != expected[j]);
    }

    // Drive the policy to each code and confirm the exact string it emits.
    auto code = [](TargetMode mode, const ModelMetadata& m) {
        return model_compatibility(mode, m).reason_code;
    };
    ModelMetadata undeclared = digitv3(); undeclared.declared = false;
    ModelMetadata unknown = good("mystery", "digit_numeric", "m.engine");
    ModelMetadata fambad = digitv3(); fambad.family = "float_ball";
    ModelMetadata shapebad = digitv3(); shapebad.input_size = 1;
    ModelMetadata classbad = digitv3(); classbad.artifact_matches = false;
    ModelMetadata provbad = digitv3(); provbad.provenance_ok = false;

    REQUIRE(code(TargetMode::DigitReader, undeclared) == "model_undeclared");
    REQUIRE(code(TargetMode::DigitReader, unknown) == "model_unknown_id");
    REQUIRE(code(TargetMode::DigitReader, fambad) == "model_family_mismatch");
    REQUIRE(code(TargetMode::DigitReader, shapebad) == "model_shape_unsupported");
    REQUIRE(code(TargetMode::DigitReader, classbad) == "model_classes_mismatch");
    REQUIRE(code(TargetMode::DigitReader, provbad) == "model_provenance_failed");
    REQUIRE(code(TargetMode::BallLeveler, digitv3()) == "model_mode_incompatible");
    REQUIRE(code(TargetMode::DigitReader, digitv3()) == "model_allowed");
}

// ─── loadable_model_files ────────────────────────────────────────────────────

TEST_CASE("loadable_model_files returns only the mode's allowed filenames", "[compat]") {
    std::vector<ModelMetadata> catalog = {digitv3(), float_small(), float_big()};

    auto digit = loadable_model_files(TargetMode::DigitReader, catalog);
    REQUIRE(digit == std::set<std::string>{"digitv3.engine"});

    auto ball = loadable_model_files(TargetMode::BallLeveler, catalog);
    REQUIRE(ball == std::set<std::string>{"float-small.engine", "float-big.engine"});
}

TEST_CASE("loadable_model_files excludes undeclared, unknown and mismatched models",
          "[compat]") {
    auto undeclared = digitv3(); undeclared.declared = false;
    auto unknown = good("mystery", "digit_numeric", "mystery.engine");
    auto classbad = digitv3(); classbad.artifact_matches = false; classbad.filename = "bad.engine";
    auto provbad = digitv3(); provbad.provenance_ok = false; provbad.filename = "prov.engine";
    std::vector<ModelMetadata> catalog = {undeclared, unknown, classbad, provbad};

    REQUIRE(loadable_model_files(TargetMode::DigitReader, catalog).empty());
}

TEST_CASE("loadable_model_files deduplicates identical allowed filenames", "[compat]") {
    std::vector<ModelMetadata> catalog = {digitv3(), digitv3()};
    REQUIRE(loadable_model_files(TargetMode::DigitReader, catalog) ==
            std::set<std::string>{"digitv3.engine"});
}

TEST_CASE("loadable_model_files on an empty catalog is empty", "[compat]") {
    REQUIRE(loadable_model_files(TargetMode::DigitReader, {}).empty());
    REQUIRE(loadable_model_files(TargetMode::BallLeveler, {}).empty());
}
