// Persistence for Camera in the SQLite `camera` table (one row per camera).
// User-editable (full CRUD). Typed struct in C++, rows in the DB; write/read
// failures surface as bool / nullopt so callers can react. Mirrors the
// network/settings repos. ROI areas (`camera_area`) get their own access in a
// later slice; `remove` already cascades them.
#pragma once

#include "camera/model.h"

#include <QSqlDatabase>

#include <cstdint>
#include <optional>
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
std::vector<Camera> all(const QSqlDatabase& db);

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

} // namespace denso::camera
