#include "camera/warmup_gate.h"

#include <algorithm>
#include <utility>

namespace denso::ui {

void PendingStart::add(int64_t camera_id,
                       std::vector<std::string> not_yet_ready_models) {
    entries_.push_back({camera_id, std::move(not_yet_ready_models)});
}

std::vector<int64_t> PendingStart::ready(const std::string& model) {
    std::vector<int64_t> satisfied;
    for (Entry& e : entries_) {
        auto it = std::find(e.remaining.begin(), e.remaining.end(), model);
        if (it != e.remaining.end()) {
            e.remaining.erase(it);
        }
        if (e.remaining.empty()) {
            satisfied.push_back(e.camera_id);
        }
    }
    entries_.erase(
        std::remove_if(entries_.begin(), entries_.end(),
                       [](const Entry& e) { return e.remaining.empty(); }),
        entries_.end());
    return satisfied;
}

std::vector<int64_t> PendingStart::drain() {
    std::vector<int64_t> ids;
    ids.reserve(entries_.size());
    for (const Entry& e : entries_) {
        ids.push_back(e.camera_id);
    }
    entries_.clear();
    return ids;
}

} // namespace denso::ui
