// Ball Leveler RUNTIME state — what a camera is doing right now, as opposed to
// what it is configured to do (level/calibration.h) or how it measures
// (level/measure.h). PURE: no Qt widgets, no SQL, no OpenCV, no inference
// backend, so every state and every rendering decision is unit-testable without
// a device.
//
// Keyed by camera_id, NEVER by zone. Ball Leveler measures one tank per camera;
// there is no zone concept in this mode and nothing here may grow one.
//
// ZoneRuntimeEntry is deliberately NOT reused: it is zone-keyed, carries an int
// value rather than a percentage, and its ZoneDisplayState includes a
// hold/debounce policy (HoldingLastValid) that Lean V1 explicitly does not
// implement. Reusing it would import a second policy authority.
#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace denso::level {

/// The Lean V1 runtime states — five, not eight (Lean V1 amendment).
///
/// There is deliberately no HoldingLastValid: an old measurement must never be
/// presented as a current live one, and the only way to guarantee that is to
/// have no state whose meaning is "showing you a number from the past".
enum class LevelState {
    Unconfigured,        ///< no Ball configuration stored for this camera
    Acquiring,           ///< configured and running, but no measurement yet
    Healthy,             ///< a current measurement is available
    Unavailable,         ///< cannot measure right now; `reason` says why
    CalibrationInvalid,  ///< stored calibration does not apply to this view
};

// ── Unavailable reason codes ────────────────────────────────────────────────
// These are a FILE FORMAT and an operator-visible surface: never rename, never
// reuse, only add. Each is safe to display and to log — no credential, no URL,
// no source string can travel this path.
inline constexpr const char* kReasonCameraOffline    = "camera_offline";
inline constexpr const char* kReasonPaused           = "paused";
inline constexpr const char* kReasonModelUnavailable = "model_unavailable";
inline constexpr const char* kReasonInhibited        = "inhibited";
inline constexpr const char* kReasonInferenceError   = "inference_error";
/// The camera's stored configuration could not be READ (broken table, bad
/// schema). Distinct from Unconfigured, which means the query succeeded and the
/// camera simply has no row — collapsing the two would report a corrupt
/// database as a routine "not set up yet".
inline constexpr const char* kReasonConfigUnreadable = "config_unreadable";

/// One camera's runtime picture.
///
/// INVARIANT: `percent` is engaged ONLY when `state == Healthy`. It is not a
/// convention to be re-checked at each draw site — it is established once, by
/// the constructors below, so no rendering path can show a number the state does
/// not justify. Build entries through them, never by aggregate initialization.
struct LevelRuntimeEntry {
    int64_t camera_id = 0;
    LevelState state = LevelState::Unconfigured;
    std::optional<double> percent;   ///< engaged iff state == Healthy
    std::string reason;              ///< non-empty only when state == Unavailable
    int64_t ts_ms = 0;               ///< when this picture was formed

    /// A live measurement. The ONLY way to produce an entry carrying a number.
    static LevelRuntimeEntry healthy(int64_t camera_id, double percent,
                                     int64_t ts_ms);
    /// Configured and running, nothing measured yet. Carries no number.
    static LevelRuntimeEntry acquiring(int64_t camera_id, int64_t ts_ms);
    /// Cannot measure right now. Carries no number — this is what makes
    /// "offline clears the live value" true by construction rather than by a
    /// rule each caller must remember.
    static LevelRuntimeEntry unavailable(int64_t camera_id, std::string reason,
                                         int64_t ts_ms);
    static LevelRuntimeEntry unconfigured(int64_t camera_id, int64_t ts_ms);
    static LevelRuntimeEntry calibration_invalid(int64_t camera_id, int64_t ts_ms);
};

/// Operator-facing state word, e.g. `CALIBRATION INVALID`. Stable text: it is
/// burned into the displayed frame and read off a machine-side screen.
const char* level_state_label(LevelState s);

}  // namespace denso::level
