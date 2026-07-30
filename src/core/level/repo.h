// Ball Leveler persistence — THE sole durable Ball Leveler model-binding and
// calibration authority (`ball_level_calibration`, migration v14).
//
// Deliberately NOT `camera_model`. That table's contract is a digit-reader
// ENSEMBLE: N models, a per-model class subset, a per-class confidence, and
// cross-model merge/NMS. Ball Leveler v1 is exactly one model, one class and one
// threshold, so reusing it would make five invalid states representable (zero
// models, multiple models, zero classes, multiple classes, and a confidence in
// camera_model_class disagreeing with the calibration's). One binding authority
// means ONE authoritative write API per domain, not one physical table shared by
// unrelated domains.
//
// This unit holds NO model/mode rule. Authorization is asked of the ONE central
// policy (models::model_compatibility) exactly as detection::set_camera_models
// does; the family->mode matrix lives only in models/compatibility.cpp.
#pragma once

#include "level/calibration.h"
#include "models/model_identity.h"  // ManifestView, PlatformInfo
#include "mode/mode.h"              // TargetMode

#include <QSqlDatabase>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace denso::level {

/// A requested model binding. A vector of these is accepted so that "zero models"
/// and "an ensemble" are REPRESENTABLE at the API boundary and can therefore be
/// REFUSED with a named reason — rather than being made unsayable by the signature
/// and so silently mis-saved by a caller that meant one of them.
struct LevelBinding {
    int64_t model_id = 0;
    std::vector<int> class_ids;
};

/// A camera's stored Ball Leveler configuration.
struct LevelConfig {
    int64_t camera_id = 0;
    int64_t model_id = 0;
    int class_id = 0;
    LevelCalibration calibration;
    /// Fingerprint of the view-significant camera fields the calibration was drawn
    /// against. Geometry is in oriented-frame coordinates, so a change to rotation
    /// / pitch / roll / width / height / source makes it refer to a DIFFERENT
    /// physical view. A mismatch means CalibrationInvalid — the camera stops
    /// measuring, and the operator's work is NOT deleted.
    std::string view_revision;
};

/// Why a save was refused. Carries the camera id, the model row id, the canonical
/// id (or "<undeclared>"), the reduced filename and the stable reason code — and
/// NOTHING else, so a refusal is diagnosable without ever exposing a credential.
struct SaveRefusal {
    int64_t camera_id = 0;
    int64_t model_id = 0;
    std::string canonical_id;
    std::string filename;
    std::string reason_code;
};

/// THE Ball Leveler write chokepoint. In ONE transaction it:
///   1. requires exactly one binding            -> level_model_count
///   2. requires exactly one class on it        -> level_class_count
///   3. resolves the model through the manifest for the ACTIVE backend
///   4. asks the CENTRAL policy for BallLeveler -> the policy's own reason code
///   5. requires the class to be DECLARED by that model -> level_class_unknown
///   6. validates the calibration               -> the calibration's reason code
///   7. upserts the single row for this camera
///
/// Any failure rolls the WHOLE operation back, so a refused save cannot leave a
/// partial write and cannot damage the configuration already stored for that
/// camera. `view` and `platform` have NO default: a forgotten call site must fail
/// to compile, never silently authorize.
///
/// There is deliberately NO `mode` parameter. THIS TABLE IS THE MODE: a row in
/// `ball_level_calibration` is by definition a Ball Leveler binding, so the
/// compatibility question can only ever be "is this model allowed in
/// BallLeveler?". Letting a caller pass the mode would let a mistaken or
/// malicious call site authorize a DIGIT model against DigitReader and then
/// store it as the Ball table's sole binding — the one thing this chokepoint
/// exists to prevent. The mode is hardcoded below; only the FAMILY->MODE matrix
/// (in models/compatibility.cpp) decides the answer.
bool save_level_configuration(const QSqlDatabase& db, int64_t camera_id,
                              const std::vector<LevelBinding>& models,
                              const LevelCalibration& calibration,
                              const std::string& view_revision,
                              const denso::models::ManifestView& view,
                              const denso::models::PlatformInfo& platform,
                              SaveRefusal* refusal = nullptr);

/// A camera's stored configuration, distinguishing all THREE outcomes:
///   nullopt                -> the QUERY FAILED (missing/broken table, bad schema)
///   optional{} (inner nul) -> the query succeeded and the camera has NO row
///   a value                -> the query succeeded and returned the row
///
/// The distinction is load-bearing. A missing or broken `ball_level_calibration`
/// is an INFRASTRUCTURE fault (Blocked / exit 78); a camera that simply has not
/// been calibrated yet is an ordinary per-camera setup gap (Degraded / exit 10).
/// Collapsing the two would report a corrupt database as a routine "not set up
/// yet" — the exact class of lie the fallible-query pattern exists to stop.
/// Mirrors detection::try_attached_model_filenames.
std::optional<std::optional<LevelConfig>> try_level_config_for(
    const QSqlDatabase& db, int64_t camera_id);

/// Convenience over try_level_config_for for the callers that genuinely cannot
/// act on the difference. A query failure collapses into nullopt here, so DO NOT
/// use this on any path that reports readiness — use try_level_config_for.
std::optional<LevelConfig> level_config_for(const QSqlDatabase& db,
                                            int64_t camera_id);

/// Every camera id with a stored configuration whose geometry still validates,
/// ascending — or nullopt when the QUERY FAILED. "Stored" alone is not enough,
/// because a row can be hand-edited or restored from a backup.
///
/// Fallible because an empty result and a broken table mean opposite things to
/// `mode_setup_required`: empty means "setup is required", broken means
/// "undeterminable", and that contract forbids guessing.
std::optional<std::vector<int64_t>> try_cameras_with_valid_config(
    const QSqlDatabase& db);

/// Convenience over try_cameras_with_valid_config. A query failure collapses
/// into an empty vector, which is INDISTINGUISHABLE from "no camera is
/// calibrated" — never use this to decide readiness or setup state.
std::vector<int64_t> cameras_with_valid_config(const QSqlDatabase& db);

/// Does this camera have a stored configuration whose geometry validates?
bool has_valid_config(const QSqlDatabase& db, int64_t camera_id);

/// Remove a camera's configuration. Returns false on a write error.
bool clear_level_configuration(const QSqlDatabase& db, int64_t camera_id);

}  // namespace denso::level
