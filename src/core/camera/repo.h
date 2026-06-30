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

} // namespace denso::camera
