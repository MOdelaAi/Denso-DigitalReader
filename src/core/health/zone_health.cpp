#include "health/zone_health.h"

#include <utility>

namespace denso::health {

ZoneHealth::ZoneHealth(std::function<void(int64_t, bool)> on_inhibit_changed)
    : on_inhibit_changed_(std::move(on_inhibit_changed)) {}

void ZoneHealth::set_cause(int64_t camera_id, ZoneCause c, bool on) {
    uint32_t& mask = causes_[camera_id];
    const uint32_t before = mask;
    const uint32_t bit = static_cast<uint32_t>(c);
    if (on) {
        mask |= bit;
    } else {
        mask &= ~bit;
    }
    if (mask == before) {
        return;   // no change — do not re-notify
    }
    // A camera releases only when ALL causes clear.
    const bool was = before != 0;
    const bool now = mask != 0;
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
