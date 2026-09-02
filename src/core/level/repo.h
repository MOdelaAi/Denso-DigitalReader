// Ball Leveler persistence — THE sole durable Ball Leveler model-binding and
// zone-geometry authority (`ball_level_binding` + `ball_level_zone`,
// migration v15).
//
// Deliberately NOT `camera_model`. That table's contract is a digit-reader
// ENSEMBLE: N models, a per-model class subset, a per-class confidence, and
// cross-model merge/NMS. Ball Leveler binds exactly one model and one class per
// CAMERA, so reusing it would make several invalid states representable (zero
// models, multiple models, zero classes, multiple classes). One binding
// authority means ONE authoritative write API per domain, not one physical table
// shared by unrelated domains.
//
// Two tables, not one (amendment §10.5): the camera-level binding is separate
// from the 1..kMaxBallZones geometry rows precisely so the model is stored ONCE
// per camera. A model column repeated in every zone row would make "four zones
// bound to four different models" representable, which is the state the
// operator's one-model-per-camera rule exists to forbid.
//
// This unit holds NO model/mode rule. Authorization is asked of the ONE central
// policy (models::model_compatibility) exactly as detection::set_camera_models
// does; the family->mode matrix lives only in models/compatibility.cpp. It also
// holds no zone-NUMBERING rule of its own: the namespace bound is
// camera::zone_in_range and cross-camera/cross-mode ownership is asked of
// camera::zones_owned_by_other_cameras, the same authority the digit Areas page
// consults.
#pragma once

#include "level/calibration.h"
#include "models/model_identity.h"  // ManifestView, PlatformInfo
#include "mode/mode.h"              // TargetMode

#include <QSqlDatabase>

#include <cstdint>
#include <map>
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

// LevelZone lives in level/calibration.h — a pure value type, so the evaluation
// step and the overlay can take a zone set without QtSql.

/// A camera's stored Ball Leveler configuration: ONE model binding and its
/// 1..kMaxBallZones zones, ascending by `zone_no`.
struct LevelConfig {
    int64_t camera_id = 0;
    int64_t model_id = 0;
    int class_id = 0;
    std::vector<LevelZone> zones;
    /// Fingerprint of the view-significant camera fields the geometry was drawn
    /// against. Geometry is in oriented-frame coordinates, so a change to rotation
    /// / pitch / roll / width / height / source makes it refer to a DIFFERENT
    /// physical view. A mismatch means CalibrationInvalid — the camera stops
    /// measuring, and the operator's work is NOT deleted.
    ///
    /// One per CAMERA, not per zone: every zone of a camera is drawn on the same
    /// frame, so per-zone copies could only ever disagree with themselves.
    std::string view_revision;
};

/// Why a save was refused. Carries the camera id, the model row id, the canonical
/// id (or "<undeclared>"), the reduced filename and the stable reason code — and
/// NOTHING else, so a refusal is diagnosable without ever exposing a credential.
///
/// `zone_no` names the offending zone for the zone-scoped reason codes
/// (`level_zone_*`, and the geometry codes from validate_calibration), and is
/// DISENGAGED for camera-scoped ones. Without it a four-zone save that fails
/// geometry validation would tell the operator only that "a" zone is wrong.
///
/// It is an optional rather than a 0 sentinel because the two facts are
/// genuinely different — "refused, and here is the zone" versus "refused, and no
/// zone is implicated" — and the type should say so. That 0 happens to be
/// outside the namespace does not make it a good spelling for "no zone": a
/// sentinel only reads correctly for as long as nobody widens the range onto
/// it, and this file has already been through that once.
struct SaveRefusal {
    int64_t camera_id = 0;
    int64_t model_id = 0;
    std::string canonical_id;
    std::string filename;
    std::string reason_code;
    std::optional<int> zone_no;
};

/// THE Ball Leveler write chokepoint. In ONE transaction it:
///   1. requires exactly one binding                 -> level_model_count
///   2. requires exactly one class on it             -> level_class_count
///   3. requires 1..kMaxBallZones zones              -> level_zone_count
///   4. requires each zone_no in camera::kMinZone..kMaxZone (1..99)
///                                                    -> level_zone_out_of_range
///   5. requires zone numbers unique within the set  -> level_zone_duplicate
///   6. requires no OTHER camera (either mode) to
///      claim any of those numbers                   -> level_zone_taken
///   7. resolves the model through the manifest for the ACTIVE backend
///   8. asks the CENTRAL policy for BallLeveler      -> the policy's own code
///   9. requires the class to be DECLARED by that model -> level_class_unknown
///  10. validates EVERY zone's geometry              -> the calibration's code
///  11. upserts the binding and REPLACES the camera's zone rows wholesale
///
/// Every check runs BEFORE any mutation, and any failure rolls the WHOLE
/// operation back — so one invalid zone cannot leave three valid ones written,
/// and a refused save cannot damage the configuration already stored for that
/// camera. There is deliberately NO per-zone save entry point: a caller that
/// could write one zone could write a set this function would have refused.
///
/// `view` and `platform` have NO default: a forgotten call site must fail to
/// compile, never silently authorize.
///
/// There is deliberately NO `mode` parameter. THESE TABLES ARE THE MODE: a row in
/// `ball_level_binding` is by definition a Ball Leveler binding, so the
/// compatibility question can only ever be "is this model allowed in
/// BallLeveler?". Letting a caller pass the mode would let a mistaken or
/// malicious call site authorize a DIGIT model against DigitReader and then
/// store it as the Ball table's sole binding — the one thing this chokepoint
/// exists to prevent. The mode is hardcoded below; only the FAMILY->MODE matrix
/// (in models/compatibility.cpp) decides the answer.
bool save_level_configuration(const QSqlDatabase& db, int64_t camera_id,
                              const std::vector<LevelBinding>& models,
                              const std::vector<LevelZone>& zones,
                              const std::string& view_revision,
                              const denso::models::ManifestView& view,
                              const denso::models::PlatformInfo& platform,
                              SaveRefusal* refusal = nullptr);

/// A camera's stored configuration, distinguishing all THREE outcomes:
///   nullopt                -> the QUERY FAILED (missing/broken table, bad schema)
///   optional{} (inner nul) -> the query succeeded and the camera has NO binding
///   a value                -> the query succeeded and returned the configuration
///
/// The distinction is load-bearing. A missing or broken Ball table is an
/// INFRASTRUCTURE fault (Blocked / exit 78); a camera that simply has not been
/// configured yet is an ordinary per-camera setup gap (Degraded / exit 10).
/// Collapsing the two would report a corrupt database as a routine "not set up
/// yet" — the exact class of lie the fallible-query pattern exists to stop.
/// Mirrors detection::try_attached_model_filenames.
///
/// A binding with ZERO zone rows is reported as NO configuration: the chokepoint
/// cannot write that state, so encountering it means the rows were removed
/// outside the app, and a camera with a model but nothing to measure is not
/// configured in any useful sense.
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
/// A camera qualifies only when EVERY one of its zones validates. A camera with
/// one broken zone is not partially serviceable: the operator must see and fix
/// it, and reporting three of its four zones would hide that.
///
/// Fallible because an empty result and a broken table mean opposite things to
/// `mode_setup_required`: empty means "setup is required", broken means
/// "undeterminable", and that contract forbids guessing.
std::optional<std::vector<int64_t>> try_cameras_with_valid_config(
    const QSqlDatabase& db);

/// Convenience over try_cameras_with_valid_config. A query failure collapses
/// into an empty vector, which is INDISTINGUISHABLE from "no camera is
/// configured" — never use this to decide readiness or setup state.
std::vector<int64_t> cameras_with_valid_config(const QSqlDatabase& db);

/// Does this camera have a stored configuration whose zones all validate?
bool has_valid_config(const QSqlDatabase& db, int64_t camera_id);

/// Every Ball zone number claimed by a camera OTHER than `camera_id`, mapped to
/// the owning camera's name — the Ball counterpart of the digit Areas page's
/// call to camera::zones_owned_by_other_cameras, and delegating to exactly that
/// function so both modes read one answer from one authority.
std::map<int, std::string> zones_owned_elsewhere(const QSqlDatabase& db,
                                                 int64_t camera_id);

/// Remove a camera's configuration — binding and all zone rows, in one
/// transaction. Returns false on a write error.
bool clear_level_configuration(const QSqlDatabase& db, int64_t camera_id);

}  // namespace denso::level
