// Persistence for Reading in the SQLite `reading` table (one row per captured
// reading). Append + range-read only — readings are an immutable log. Mirrors
// the camera/detection repos: write/read failures surface as nullopt/empty so
// callers can react.
#pragma once

#include "reading/reading.h"

#include <QSqlDatabase>

#include <cstdint>
#include <optional>
#include <vector>

namespace denso::reading {

/// Insert a reading; returns its assigned id, or nullopt on a write error.
std::optional<int64_t> insert(const QSqlDatabase& db, const Reading& r);

/// Every reading for a camera with `from_ms <= ts_ms <= to_ms`, ordered by
/// ts_ms ascending then id. Empty when none match (or on error).
std::vector<Reading> query(const QSqlDatabase& db, int64_t camera_id,
                           int64_t from_ms, int64_t to_ms);

} // namespace denso::reading
