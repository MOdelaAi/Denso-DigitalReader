#pragma once
#include <map>
#include <optional>
#include <string>
#include <vector>
namespace denso::models {
struct ClassMapResult {
    std::optional<std::map<int, int>> map;   // old id -> new id (injective); omits ids absent from new
    std::string error;
};
// Resolve an old-model class-id -> new-model class-id remap BY NAME.
// explicit_remap redirects an old NAME to a new NAME before lookup.
ClassMapResult resolve_class_map(const std::vector<std::string>& old_names,
                                 const std::vector<std::string>& new_names,
                                 const std::map<std::string, std::string>& explicit_remap);
}
