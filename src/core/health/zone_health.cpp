#include "health/zone_health.h"

#include <utility>

namespace denso::health {

ZoneHealth::ZoneHealth(std::function<void(int64_t, bool)> on_inhibit_changed)
    : on_inhibit_changed_(std::move(on_inhibit_changed)) {}

void ZoneHealth::set_cause(int64_t camera_id, ZoneCause c, bool on) {
    // Look up without operator[]: clearing a cause for a camera we have never seen
    // must NOT insert a phantom mask-0 entry, or all() would report a retired
    // camera and the map would grow unbounded as camera ids churn.
    const auto it = causes_.find(camera_id);
    const uint32_t before = (it == causes_.end()) ? 0u : it->second;
    const uint32_t bit = static_cast<uint32_t>(c);
    const uint32_t after = on ? (before | bit) : (before & ~bit);
    if (after == before) {
        return;   // no change — do not re-notify, and never insert a phantom entry
    }
    if (after == 0u) {
        // Last cause cleared: drop the entry so all() only ever lists currently
        // inhibited cameras.
        causes_.erase(it);
    } else if (it == causes_.end()) {
        causes_.emplace(camera_id, after);
    } else {
        it->second = after;
    }
    // A camera releases only when ALL causes clear.
    const bool was = before != 0;
    const bool now = after != 0;
    if (was != now && on_inhibit_changed_) {
        on_inhibit_changed_(camera_id, now);
    }
}

bool ZoneHealth::is_inhibited(int64_t camera_id) const {
    return causes(camera_id) != 0;
}

uint32_t ZoneHealth::causes(int64_t camera_id) const {
    const auto it = causes_.find(camera_id);
    return it == causes_.end() ? 0u : it->second;
}

} // namespace denso::health
