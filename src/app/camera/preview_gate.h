// Pure state for the wizard's snapshot preview: which capture attempt is the
// newest, and whether what the operator is looking at is a REAL frame from the
// current source. No Qt — unit-tested like the other camera policy units. The
// controller holds the QImage and calls in here to decide what to apply and
// whether the ROI quarantine may be cleared.
//
// This owns two rules that used to be ad-hoc in CameraWizardController:
//
//  1. Supersede. A late result from a previous source must not overwrite the
//     preview for the current one (the operator would "verify" ROIs against the
//     wrong image). The controller had this as a bare `capture_gen_` counter.
//
//  2. Liveness — the rule the controller was MISSING. It asked only whether the
//     stored image was null, so a refresh that FAILED left the previous success
//     on screen and still counted as verification: the Areas page would clear
//     `areas_need_review` and resume zone reporting against an image that no
//     longer reflects the camera. Liveness is the LAST settled attempt's result,
//     never "some attempt once worked".
#pragma once

#include <cstdint>

namespace denso::ui {

class PreviewGate {
public:
    /// Begin a capture attempt. Returns its generation token; hand the SAME
    /// token to settle() when the attempt finishes. Beginning supersedes any
    /// attempt still in flight AND immediately drops liveness — nothing is
    /// verified while a fresh answer is outstanding. The caller's displayed image
    /// is untouched; only verification is revoked.
    uint64_t begin();

    /// Report a finished attempt. Returns true when `gen` is the newest attempt,
    /// i.e. the caller should apply this result to the UI; false when it has been
    /// superseded (by a newer begin(), an invalidate(), or an earlier settle of
    /// the same token) and must be dropped. Only an applied result moves
    /// liveness — so a stale failure cannot revoke a newer success.
    bool settle(uint64_t gen, bool ok);

    /// The source or its capture geometry changed: nothing on screen reflects it
    /// any more. Drops liveness and supersedes any attempt in flight.
    void invalidate();

    /// True only when the most recent settled attempt SUCCEEDED. This is the
    /// verification gate — a quarantined camera's ROIs may only be confirmed
    /// against a live view.
    bool has_live_frame() const { return live_; }

    /// True while an attempt is outstanding AND still applicable. NOT a claim
    /// about the worker thread: after invalidate() this reads false while the
    /// superseded capture is still physically running inside grab_snapshot().
    /// Never use it to decide whether a worker may start or whether teardown is
    /// complete — it answers "is an answer still coming that I would act on?".
    bool is_capturing() const { return in_flight_; }

private:
    // Monotonic: every begin()/invalidate() burns a token, so a superseded
    // result can never match again.
    uint64_t gen_ = 0;
    bool in_flight_ = false;
    bool live_ = false;
};

} // namespace denso::ui
