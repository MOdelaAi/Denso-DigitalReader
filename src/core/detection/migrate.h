#pragma once
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>
#include <QSqlDatabase>
#include "detection/detection.h"   // ModelClassSelection
namespace denso::detection {
struct MigrateRequest {
    std::string old_filename, new_filename, new_name, new_engine_sha256;
    std::vector<std::string> new_class_names;
    std::map<std::string, std::string> explicit_remap;
    std::vector<int64_t> camera_ids;
    std::string created_utc;
};
struct MigrateResult { bool ok = false; std::string error; std::vector<int64_t> affected_cameras; };
// Non-empty cameras; positive ids; no dup ids; non-empty old/new filename, new_name,
// new_class_names, new_engine_sha256; old_filename != new_filename.
std::optional<std::string> validate_request(const MigrateRequest& r);

struct OldAttach {
    int64_t camera_model_id = 0;
    int64_t old_model_id = 0;
    std::vector<std::string> old_class_names;
    std::vector<ModelClassSelection> classes;
};
enum class LoadStatus { Ok, QueryFailed, NotAttached, Ambiguous };
struct LoadResult { LoadStatus status = LoadStatus::QueryFailed; OldAttach attach; };
// Exactly-one CAS: >1 matching attachment => Ambiguous; 0 => NotAttached; exec fail => QueryFailed.
LoadResult load_old_attachment(const QSqlDatabase& db, int64_t camera_id,
                               const std::string& old_filename);
}
