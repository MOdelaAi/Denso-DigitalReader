// THE central model/mode compatibility policy — Slice 6.
//
// This translation unit is deliberately PURE: no Qt (Widgets OR Core), no
// OpenCV, no SQL, no filesystem, no inference backend, no application code. It
// takes resolved data and returns a verdict, so it is exhaustively unit-testable
// without a GPU, a display or a database.
//
// The backend is already resolved away before the policy is reached: a
// ModelMetadata is produced by resolve_model_metadata (model_identity.{h,cpp})
// FOR THE ACTIVE BACKEND, so model_compatibility is a pure function of
// mode + identity and never sees a Backend at all (spec 3.2.1 rule 4).
//
// The family->modes matrix lives in exactly ONE place: the compiled registry in
// compatibility.cpp. It is not in the manifest (which carries no allowed_modes),
// not in a UI branch, not in a SQL WHERE, not in a shell test. Adding a model
// family is a code change with a review and a test.
#pragma once

#include "mode/mode.h"

#include <set>
#include <string>
#include <vector>

namespace denso::models {

/// What the appliance knows about one model — resolved from its manifest entry
/// FOR THE ACTIVE BACKEND and corroborated against the catalog row. Never
/// constructed from a filename. The backend is already resolved away by
/// resolve_model_metadata: the policy below is platform-independent.
struct ModelMetadata {
    std::string canonical_id;               // "" when undeclared
    std::string family;                     // "" when undeclared
    std::string task;                       // "detect"
    int         input_size  = 0;            // 640
    int         class_count = 0;
    std::vector<std::string> class_names;   // ordered, from the artifact
    std::string filename;                   // the ACTIVE backend's artifact —
                                            // .onnx on Windows, .engine on Jetson.
                                            // Diagnostics + the warm-up allow-list key.
    bool declared         = false;          // a schema-2 entry WITH a block for the
                                            // active backend exists
    bool artifact_matches = false;          // 3.4 class corroboration passed
    bool provenance_ok    = false;          // this backend's hashes (+ built_for on
                                            // Jetson only) passed
};

enum class Verdict {
    Allowed,
    RejectedWrongMode,         // declared, known family, not allowed in this mode
    RejectedUnknown,           // undeclared, or an unrecognised canonical_id/family
    RejectedMetadataMismatch,  // artifact disagrees with its declaration
    RejectedProvenance,        // hash / platform mismatch
};

struct CompatibilityResult {
    Verdict     verdict = Verdict::RejectedUnknown;  // fail-closed default
    std::string reason_code;  // stable string; a FILE FORMAT (status.json)
    std::string detail;       // camera/model identity — NEVER a credential (spec 12)

    // The default-constructed result denies: a path that forgets to assign a
    // verdict rejects, it does not permit.
    bool allowed() const { return verdict == Verdict::Allowed; }
};

/// THE policy. Every enforcement path calls exactly this. Evaluation order is
/// fail-closed at every step (spec 4.2); the first matching failure wins.
CompatibilityResult model_compatibility(denso::mode::TargetMode mode,
                                        const ModelMetadata& model);

/// Every model filename the appliance may load in `mode`, given the resolved
/// metadata for the active backend. The ONLY set EngineRegistry may ever see
/// (spec 7.0). Deduplicated through std::set; only Allowed metadata contributes.
std::set<std::string> loadable_model_files(denso::mode::TargetMode mode,
                                           const std::vector<ModelMetadata>& models);

}  // namespace denso::models
