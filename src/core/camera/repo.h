// Persistence for Camera in the SQLite `camera` table (one row per camera).
// User-editable (full CRUD). Typed struct in C++, rows in the DB; write/read
// failures surface as bool / nullopt so callers can react. Mirrors the
// network/settings repos. ROI areas (`camera_area`) get their own access in a
// later slice; `remove` already cascades them.
#pragma once

#include "camera/camera.h"

#include <QSqlDatabase>

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace denso::camera {

/// Insert a new camera; returns its assigned id, or nullopt on a write error.
std::optional<int64_t> insert(const QSqlDatabase& db, const Camera& c);

/// Update an existing camera by id. Returns false on a write error.
bool update(const QSqlDatabase& db, const Camera& c);

/// Delete a camera and its ROI areas. Returns false on a write error.
bool remove(const QSqlDatabase& db, int64_t id);

/// One camera by id, or nullopt if absent (or on error).
std::optional<Camera> get(const QSqlDatabase& db, int64_t id);

/// Every camera, ordered by id.
/// EVERY camera row, finished or not — the management list, duplicate-source
/// detection, edit and delete. Not what the runtime should stream.
std::vector<Camera> all(const QSqlDatabase& db);

/// The cameras the RUNTIME may stream: enabled AND finished. The one source of
/// truth for that decision — callers must not re-derive it with their own
/// `if (active)`, and it is filtered in SQL so an unfinished camera cannot eat
/// one of the grid's four tile slots.
std::vector<Camera> runtime(const QSqlDatabase& db);

/// The cameras the BALL LEVELER runtime may stream: enabled, full stop.
///
/// Deliberately NOT runtime(). `setup_complete` records that the DIGIT wizard
/// finished, and Ball Leveler must show an enabled-but-uncalibrated camera as an
/// explicit Unconfigured state on the wall rather than filter it out before it
/// can be seen — a camera silently missing from the grid is the failure this
/// exists to prevent. Per-camera Ball readiness is resolved in the grid from
/// `ball_level_calibration`, which is the only authority on it.
///
/// This is an ADMISSION query for one mode, not a second definition of
/// runtime(): digit_reader still uses runtime() and nothing here changes it.
std::vector<Camera> active(const QSqlDatabase& db);

/// Mark the add wizard finished for `id`. Call only AFTER the models/areas write
/// it completes has itself succeeded: completing first would let a failed write
/// leave a live camera whose setup never actually landed.
bool mark_setup_complete(const QSqlDatabase& db, int64_t id);

// ─── ROI areas (`camera_area`) ───────────────────────────────────────────────

/// Every ROI area for a camera, ordered by id. Empty when the camera has none.
std::vector<CameraArea> areas_for(const QSqlDatabase& db, int64_t camera_id);

/// Replace a camera's entire ROI set with `areas` (delete-all + re-insert in
/// one transaction). An empty `areas` clears them. Returns false on a write
/// error (the transaction is rolled back). Each area's polygon is stored as a
/// serialized point string; the passed `id` field is ignored and `camera_id`
/// is taken from the argument, not the struct.
bool replace_areas(const QSqlDatabase& db, int64_t camera_id,
                   const std::vector<CameraArea>& areas);

/// The Areas page's SINGLE Save, as one transaction: the camera's ROI set and
/// -- when the operator changed it -- that camera's Digital ROI enhancement
/// level, together, all or nothing.
///
/// It exists because those two edits are made on one page behind one button, so
/// there must be no outcome in which one of them lands. Before it, the level was
/// written first and the areas second; an area refusal then left the level moved
/// against the OLD polygons, the dialog reported only that "the areas could not
/// be written", and the grid was rebuilt from that mismatch.
///
/// `enhancement` DISENGAGED means the operator changed no enhancement control:
/// no camera-row write for it is issued at all and this is byte-for-byte the old
/// area-only save. Engaged, the whole SIX-column bundle is written by one
/// targeted UPDATE — never the generic full-row camera::update, which would let a
/// stale draft overwrite unrelated columns — and every value is clamped the way
/// every other write here clamps (out of range becomes the neutral value rather
/// than failing the save).
///
/// It runs the SAME authoritative area logic `replace_areas` runs -- one
/// implementation, shared, not a copy: zone range, duplicate-within-save,
/// cross-camera AND cross-mode (`camera_area` UNION `ball_level_zone`)
/// ownership, the decimal-format clamp, and the `areas_need_review` clear that
/// makes saving the set count as verifying it. A refusal from any of those
/// unwinds the enhancement with it.
///
/// Returns false on any refusal or write error, with NOTHING persisted.
bool save_areas_and_enhancement(const QSqlDatabase& db, int64_t camera_id,
                                const std::vector<CameraArea>& areas,
                                std::optional<ImageEnhancement> enhancement);

/// Zone numbers currently claimed by cameras OTHER than `camera_id`, mapped to
/// the owning camera's name. Zones are unique machine-wide (replace_areas
/// enforces it), so this is what the Areas page needs to show which numbers are
/// free and to name the owner when one is taken — rather than letting the save
/// fail with a generic error. The edited camera is excluded because its own
/// rows are deleted before re-insert, so re-saving its zones can't self-clash.
/// ROI-only areas (no zone, or the 0 sentinel) claim nothing and are omitted.
std::map<int, std::string> zones_owned_by_other_cameras(const QSqlDatabase& db,
                                                        int64_t camera_id);

/// The same answer, with READ FAILURE kept distinct from "nothing is taken".
///
/// Every write chokepoint must use this form. The overload above cannot express
/// the difference, so a broken database reaches it as an empty map — and a save
/// gated on an empty map grants whatever number was asked for. Since zone
/// numbers are the machine-wide brazing payload keys, that is not a degraded
/// answer but a silent cross-mode collision: two cameras writing one field.
/// `nullopt` means the question could not be answered, and a caller that is
/// about to WRITE must refuse rather than assume.
std::optional<std::map<int, std::string>> try_zones_owned_by_other_cameras(
    const QSqlDatabase& db, int64_t camera_id);

/// Set/clear the ROI-review quarantine flag for a camera (see
/// Camera::areas_need_review). The controller sets it when a view-significant
/// source/geometry change is saved; replace_areas() clears it on the next save.
bool set_areas_need_review(const QSqlDatabase& db, int64_t camera_id, bool need);

} // namespace denso::camera
