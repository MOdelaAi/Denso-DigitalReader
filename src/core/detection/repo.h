// Persistence for the detection model catalog and each camera's attached
// models. Mirrors camera/repo: typed structs in C++, rows in SQLite; write
// failures surface as bool / nullopt. `detection_for` resolves the runtime
// bundle (CameraDetection) a DetectionProcessor consumes.
#pragma once

#include "detection/detection.h"
#include "models/model_identity.h"   // ManifestView, PlatformInfo, resolve_model_metadata
#include "mode/mode.h"               // TargetMode

#include <QSqlDatabase>

#include <cstdint>
#include <optional>
#include <vector>

namespace denso::detection {

// ─── model catalog ───────────────────────────────────────────────────────────

/// Insert a model by unique filename, or update its name + class_names if the
/// filename already exists. Returns the row id, or nullopt on a write error.
std::optional<int64_t> upsert_model(const QSqlDatabase& db, const DetectionModel& m);

/// Every catalog model, ordered by id.
std::vector<DetectionModel> list_models(const QSqlDatabase& db);

/// Every distinct model filename attached to any camera that the compatibility
/// policy ALLOWS in `mode` — the engines the app must be able to load at
/// warm-up. Each attached catalog model is resolved through
/// `models::resolve_model_metadata` (for the view's active backend) and reduced
/// by the central `models::model_compatibility`: a rejected attachment
/// (undeclared, unknown id, family/metadata mismatch, provenance fault, or wrong
/// mode) is EXCLUDED, so it can never enter the fail-loud required set nor abort
/// startup (spec §7.0). Allowed models keep their existing fail-loud behaviour.
/// The returned filename is the active-backend artifact from the resolved
/// metadata. `mode` and `view` have NO default — a forgotten call site must fail
/// to compile, never silently authorize. `platform` is the caller-supplied
/// measured platform (read only on the TensorRt backend), required because
/// resolve_model_metadata takes it and ManifestView deliberately does not carry
/// it (spec §3.2.1 rule 4).
std::vector<std::string> attached_model_filenames(
    const QSqlDatabase& db, denso::mode::TargetMode mode,
    const denso::models::ManifestView& view,
    const denso::models::PlatformInfo& platform);

/// Like attached_model_filenames, but distinguishes "no attachments" (an empty
/// vector) from "the query failed" (nullopt) — attached_model_filenames returns
/// {} for both, which would let a corrupt database look like a fresh install to
/// a validation gate. Prefer this wherever the difference matters. Non-throwing:
/// resolution is pure + I/O-via-optional, so a fault surfaces as nullopt/exclusion,
/// never an exception.
std::optional<std::vector<std::string>>
try_attached_model_filenames(const QSqlDatabase& db, denso::mode::TargetMode mode,
                             const denso::models::ManifestView& view,
                             const denso::models::PlatformInfo& platform);

// ─── per-camera attachments ──────────────────────────────────────────────────

/// The models attached to a camera with their class selections, ordered by id.
std::vector<CameraModel> models_for(const QSqlDatabase& db, int64_t camera_id);

/// Why an attachment write was refused by the compatibility policy. Carries the
/// camera id, the canonical ID (or "<undeclared>"), the filename and the stable
/// reason code — and NOTHING else, so a refusal is diagnosable without ever
/// exposing a credential (spec §7.1, §12).
struct AttachRefusal {
    int64_t     camera_id = 0;
    int64_t     model_id  = 0;   // catalog row id — an integer, so it can never
                                 // carry a secret; it keeps a model whose FILENAME
                                 // is not safe to print (see
                                 // models::diagnostic_filename) precisely
                                 // identifiable.
    std::string canonical_id;    // "<undeclared>" when the model declares none
    std::string filename;        // reduced; "<invalid>" if not a plain filename
    std::string policy_reason;   // stable code from models::model_compatibility
};

/// Replace a camera's entire attached-model set (+ class selections) in one
/// transaction. Empty clears it. Returns false on error (rolled back).
///
/// THE domain chokepoint for binding a model to a camera (spec §7.1): the wizard,
/// any future Leveler UI and any future CLI all pass through here. BEFORE any row
/// changes, every requested model is resolved through
/// `models::resolve_model_metadata` and judged by the central
/// `models::model_compatibility`. If ANY member of the requested set is not
/// Allowed the WHOLE set is refused and the transaction rolls back — a mixed
/// allowed/rejected request stores nothing, so a partial attachment cannot exist.
///
/// `mode`, `view` and `platform` have NO default: a forgotten call site must fail
/// to compile, never silently authorize. `refusal`, when non-null, receives the
/// diagnosis for a COMPATIBILITY refusal specifically — which is what lets a
/// caller surface a named reason instead of a generic write failure. It is left
/// untouched for a plain SQL failure, so the two remain distinguishable.
bool set_camera_models(const QSqlDatabase& db, int64_t camera_id,
                       const std::vector<CameraModel>& models,
                       denso::mode::TargetMode mode,
                       const denso::models::ManifestView& view,
                       const denso::models::PlatformInfo& platform,
                       AttachRefusal* refusal = nullptr);

/// Resolve a camera's detection config for the runtime: each attached model
/// joined with its filename + class_names. Empty `models` == no detection.
///
/// Every attached model is resolved through the central policy (spec §7.2). If
/// ANY of them is rejected the camera is inhibited as a whole: `models` comes back
/// EMPTY (never the allowed subset) with `compatibility_rejected` set, so the
/// caller requests no engine, builds no DetectionProcessor, and does not demote
/// the camera to orientation-only. Same no-default rule as above.
CameraDetection detection_for(const QSqlDatabase& db, int64_t camera_id,
                              denso::mode::TargetMode mode,
                              const denso::models::ManifestView& view,
                              const denso::models::PlatformInfo& platform);

} // namespace denso::detection
