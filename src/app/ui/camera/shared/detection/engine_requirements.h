// Pure, header-only warm-up requirement check — kept free of the inference
// backend (ORT/TensorRT) so it is unit-testable without linking a runtime.
#pragma once

#include <set>
#include <string>
#include <vector>

namespace denso::ui {

/// The required model filenames absent from `warmed` — the models whose absence
/// must abort warm-up (engine-only/no-fallback contract). Order follows
/// `required`. Empty result means every required model loaded.
inline std::vector<std::string> missing_required_models(
    const std::vector<std::string>& required,
    const std::set<std::string>& warmed) {
    std::vector<std::string> missing;
    for (const std::string& req : required) {
        if (warmed.find(req) == warmed.end()) {
            missing.push_back(req);
        }
    }
    return missing;
}

} // namespace denso::ui
