#include "models/compatibility.h"

#include <array>
#include <string_view>

namespace denso::models {
namespace {

using denso::mode::TargetMode;

// ─── THE compiled registry — the SOLE home of the mode matrix (spec 4.3) ──────
//
// Two tables, one authority. The manifest declares WHAT AN ARTIFACT IS; this
// table alone decides WHAT THAT THING MAY DO. Because the manifest never states
// privileges, an operator edit to a file in the models directory cannot widen a
// model's authorization.
//
//   canonical_id  -> family          -> allowed modes
//   digitv3       -> digit_numeric   -> { digit_reader }
//   float-small   -> float_ball      -> { ball_leveler }
//   float-big     -> float_ball      -> { ball_leveler }
//
// No model is permitted in both modes; no mode has an empty allow-list; unknown
// IDs and families fail closed.

struct FamilyModes {
    std::string_view family;
    bool digit_reader;
    bool ball_leveler;
};

constexpr std::array<FamilyModes, 2> kFamilyModes{{
    {"digit_numeric", true, false},
    {"float_ball", false, true},
}};

struct ModelFamily {
    std::string_view canonical_id;
    std::string_view family;
};

constexpr std::array<ModelFamily, 3> kModels{{
    {"digitv3", "digit_numeric"},
    {"float-small", "float_ball"},
    {"float-big", "float_ball"},
}};

// ── Compile-time invariants proving the registry is well-formed ───────────────

// Every registered family must allow at least one mode — a family with an empty
// allow-list is a model that can never run, which is a registry bug, not a
// runtime state.
constexpr bool every_family_has_a_mode() {
    for (const auto& f : kFamilyModes)
        if (!f.digit_reader && !f.ball_leveler) return false;
    return true;
}
static_assert(every_family_has_a_mode(),
              "each registered family must allow at least one mode");

// No family may be allowed in BOTH modes — the property that makes a model
// mono-modal by construction. A family that granted both would erase the whole
// point of the mode gate.
constexpr bool no_family_allows_both_modes() {
    for (const auto& f : kFamilyModes)
        if (f.digit_reader && f.ball_leveler) return false;
    return true;
}
static_assert(no_family_allows_both_modes(),
              "no registered family may be allowed in both modes");

// Every model's declared family must exist in the family->modes table, or the
// mode lookup for that model would silently fail closed for a legitimate model.
constexpr bool family_is_registered(std::string_view family) {
    for (const auto& f : kFamilyModes)
        if (f.family == family) return true;
    return false;
}
constexpr bool every_model_family_is_registered() {
    for (const auto& m : kModels)
        if (!family_is_registered(m.family)) return false;
    return true;
}
static_assert(every_model_family_is_registered(),
              "every model's family must be present in the family->modes table");

// ── Lookups ───────────────────────────────────────────────────────────────────

const ModelFamily* find_model(std::string_view canonical_id) {
    for (const auto& m : kModels)
        if (m.canonical_id == canonical_id) return &m;
    return nullptr;
}

// Fail-closed: an unregistered family (which the static_asserts prevent for
// registered models, but which a corrupted lookup could still reach) allows
// nothing.
bool family_allows_mode(std::string_view family, TargetMode mode) {
    for (const auto& f : kFamilyModes) {
        if (f.family != family) continue;
        switch (mode) {
            case TargetMode::DigitReader: return f.digit_reader;
            case TargetMode::BallLeveler: return f.ball_leveler;
        }
        return false;
    }
    return false;
}

CompatibilityResult reject(Verdict v, const char* code) {
    CompatibilityResult r;
    r.verdict = v;
    r.reason_code = code;
    return r;
}

}  // namespace

CompatibilityResult model_compatibility(TargetMode mode, const ModelMetadata& model) {
    // The order below is a contract (spec 4.2). The first matching failure wins;
    // do NOT reorder for convenience — moving provenance before the class check,
    // say, would report the wrong reason for a doubly-faulted model.

    // 1. undeclared
    if (!model.declared)
        return reject(Verdict::RejectedUnknown, "model_undeclared");

    // 2. unknown canonical_id
    const ModelFamily* entry = find_model(model.canonical_id);
    if (entry == nullptr)
        return reject(Verdict::RejectedUnknown, "model_unknown_id");

    // 3. declared family disagrees with the registry
    if (model.family != entry->family)
        return reject(Verdict::RejectedUnknown, "model_family_mismatch");

    // 4. unsupported task / input shape
    if (model.task != "detect" || model.input_size != 640)
        return reject(Verdict::RejectedMetadataMismatch, "model_shape_unsupported");

    // 5. the loaded artifact disagrees with its declaration
    if (!model.artifact_matches)
        return reject(Verdict::RejectedMetadataMismatch, "model_classes_mismatch");

    // 6. provenance (this backend's hashes, + built_for on Jetson) failed
    if (!model.provenance_ok)
        return reject(Verdict::RejectedProvenance, "model_provenance_failed");

    // 7. the family is not allowed in the committed mode
    if (!family_allows_mode(entry->family, mode))
        return reject(Verdict::RejectedWrongMode, "model_mode_incompatible");

    // 8. allowed
    CompatibilityResult r;
    r.verdict = Verdict::Allowed;
    r.reason_code = "model_allowed";
    return r;
}

std::set<std::string> loadable_model_files(TargetMode mode,
                                           const std::vector<ModelMetadata>& models) {
    std::set<std::string> out;
    for (const auto& m : models)
        if (model_compatibility(mode, m).allowed())
            out.insert(m.filename);
    return out;
}

}  // namespace denso::models
