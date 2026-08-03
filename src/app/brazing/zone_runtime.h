// The read-only PROJECTION of zone state consumed by the camera-grid overlay.
// A projection, never a second authority: ZoneAggregator remains the sole owner
// of debounce, accepted value, hold, expiry and inhibition, and the UI only
// renders what it is handed here. Deliberately carries no counters, no
// last-sent payload and no delivery state — the overlay reports what the
// appliance READ, not what a backend accepted.
#pragma once

#include "brazing/zone_reading.h"   // ZoneValue

#include <cstdint>
#include <optional>

namespace denso::ui {

/// Zone-level state, as the aggregator sees it (no camera knowledge).
enum class ZoneRuntimeState {
    Acquiring,          ///< no trusted value yet — never shows a number
    Healthy,            ///< currently accepted value
    HoldingLastValid,   ///< last accepted value retained; a hold is NOT an inhibit
    Inhibited,          ///< publication suppressed — never shows a number
};

/// One zone's projected state, keyed by zone number alone. Only ZoneAggregator
/// produces these; the UI must never consume them without a camera identity.
struct ZoneRuntime {
    int                      zone_no = 0;
    ZoneRuntimeState         state   = ZoneRuntimeState::Acquiring;
    std::optional<ZoneValue> value;   ///< present ONLY for Healthy / HoldingLastValid
};

/// What a tile renders. Adds the camera-level facts the aggregator cannot know.
enum class ZoneDisplayState {
    Acquiring,
    Healthy,
    HoldingLastValid,
    Inhibited,
    Paused,     ///< the whole CAMERA is inhibited — overrides any zone state
    Conflict,   ///< this zone number is claimed by more than one camera
};

/// One overlay row. Carries BOTH identifiers: a zone number alone is ambiguous
/// across cameras, and joining by it would let one camera display a value the
/// other produced.
struct ZoneRuntimeEntry {
    int64_t                  camera_id = 0;
    int                      zone_no   = 0;
    ZoneDisplayState         state     = ZoneDisplayState::Acquiring;
    std::optional<ZoneValue> value;   ///< present ONLY for Healthy / HoldingLastValid

    /// Value equality — lets the grid skip repainting a tile whose rows are
    /// unchanged since the last poll.
    friend bool operator==(const ZoneRuntimeEntry& a, const ZoneRuntimeEntry& b) {
        return a.camera_id == b.camera_id && a.zone_no == b.zone_no &&
               a.state == b.state && a.value == b.value;
    }
    friend bool operator!=(const ZoneRuntimeEntry& a, const ZoneRuntimeEntry& b) {
        return !(a == b);
    }
};

/// One zone-level inhibit ONSET, drained from the aggregator's event set and
/// joined to the camera that owns the zone. An EVENT, not state: the standing
/// condition is visible in the projection above and in status.json's
/// `inhibited_zones`. Carries no value, no delivery state and no URL — an alarm
/// says which zone stopped reading, nothing about what a backend did with it.
struct ZoneInhibitOnset {
    int64_t camera_id = 0;
    int     zone_no   = 0;

    friend bool operator==(const ZoneInhibitOnset& a, const ZoneInhibitOnset& b) {
        return a.camera_id == b.camera_id && a.zone_no == b.zone_no;
    }
};

} // namespace denso::ui
