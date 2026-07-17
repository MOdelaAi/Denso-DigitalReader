// Persistence for the detection model catalog and each camera's attached
// models. Mirrors camera/repo: typed structs in C++, rows in SQLite; write
// failures surface as bool / nullopt. `detection_for` resolves the runtime
// bundle (CameraDetection) a DetectionProcessor consumes.
#pragma once

#include "detection/detection.h"

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

/// Every distinct model filename attached to any camera — the engines the app
/// must be able to load at warm-up. Used to fail loud when a configured model
/// is missing (rather than silently demoting the camera to no-detection).
std::vector<std::string> attached_model_filenames(const QSqlDatabase& db);

/// Like attached_model_filenames, but distinguishes "no attachments" (an empty
/// vector) from "the query failed" (nullopt) — attached_model_filenames returns
/// {} for both, which would let a corrupt database look like a fresh install to
/// a validation gate. Prefer this wherever the difference matters.
std::optional<std::vector<std::string>>
try_attached_model_filenames(const QSqlDatabase& db);

// ─── per-camera attachments ──────────────────────────────────────────────────

/// The models attached to a camera with their class selections, ordered by id.
std::vector<CameraModel> models_for(const QSqlDatabase& db, int64_t camera_id);

/// Replace a camera's entire attached-model set (+ class selections) in one
/// transaction. Empty clears it. Returns false on error (rolled back).
bool set_camera_models(const QSqlDatabase& db, int64_t camera_id,
                       const std::vector<CameraModel>& models);

/// Resolve a camera's detection config for the runtime: each attached model
/// joined with its filename + class_names. Empty `models` == no detection.
CameraDetection detection_for(const QSqlDatabase& db, int64_t camera_id);

} // namespace denso::detection
