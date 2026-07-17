#include "camera/preview_gate.h"

namespace denso::ui {

uint64_t PreviewGate::begin() {
    in_flight_ = true;
    // Liveness drops the INSTANT a new attempt starts, not when it settles.
    // Otherwise the hole stays open for the duration of the request: refresh a
    // camera that has gone offline, and before the failure lands the operator can
    // still hit "Verify & save" — the gate would say live (from the PREVIOUS
    // success) and the quarantine would clear against a view nothing has
    // re-confirmed.
    //
    // This does NOT blank the ROI canvas: the displayed image is the
    // controller's `last_frame_`, which is untouched here. Display state and
    // verification state are deliberately separate — that conflation was the
    // original bug.
    live_ = false;
    return ++gen_;
}

bool PreviewGate::settle(uint64_t gen, bool ok) {
    if (gen != gen_) {
        return false;  // superseded — not the newest attempt; drop it
    }
    // Burn the token so a duplicate delivery of this same generation cannot
    // apply twice (a second settle would otherwise flip liveness again).
    ++gen_;
    in_flight_ = false;
    live_ = ok;
    return true;
}

void PreviewGate::invalidate() {
    ++gen_;  // supersede anything in flight against the old source
    in_flight_ = false;
    live_ = false;
}

} // namespace denso::ui
