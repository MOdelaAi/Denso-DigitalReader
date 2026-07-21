// Per-camera inhibit causes. Causes are evaluated PER CAMERA and conservatively
// projected onto every zone that camera owns — this is NOT precise per-zone
// isolation, and the spec (§3.1) says so deliberately.
//
// THREADING: every cause transition is made on the GUI thread. Three of the four
// sources are already GUI-thread; the inference-worker source is marshalled with
// common::post_to_gui. That single owner is why this class needs no mutex and no
// revision counter. Do not call it from a worker thread.
#pragma once

#include <cstdint>
#include <functional>
#include <map>

namespace denso::health {

// ONLY causes with a real producer are declared (the no-speculative-enum rule).
// Bit values are stable — assign the NEXT free bit when adding one, never renumber
// (status.json consumers read the bitmask). 1u << 2 is intentionally free: it was
// a speculative ModelInvalid with no producer, removed rather than left dangling.
enum class ZoneCause : uint32_t {
    AreasNeedReview       = 1u << 0,
    ModelUnavailable      = 1u << 1,
    CaptureOffline        = 1u << 3,
    InferenceWorkerFailed = 1u << 4,
};

class ZoneHealth {
public:
    /// `on_inhibit_changed(camera_id, inhibited)` fires ONLY on a 0<->non-0
    /// transition, so the reporter is not churned by every cause edit.
    explicit ZoneHealth(std::function<void(int64_t, bool)> on_inhibit_changed);

    void set_cause(int64_t camera_id, ZoneCause c, bool on);
    bool is_inhibited(int64_t camera_id) const;
    uint32_t causes(int64_t camera_id) const;
    const std::map<int64_t, uint32_t>& all() const { return causes_; }

private:
    std::function<void(int64_t, bool)> on_inhibit_changed_;
    std::map<int64_t, uint32_t> causes_;
};

} // namespace denso::health
