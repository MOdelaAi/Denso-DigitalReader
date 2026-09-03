// The Digital Number Reader's per-camera IMAGE ENHANCEMENT configuration: the
// value type, its persisted encoding, its bounds, and its fail-safe decoding.
//
// PURE domain (std only), so it lives in denso_core beside camera::Camera, which
// carries the bundle. The IMAGE PROCESSING a configuration selects — the tone
// curve, the CLAHE parameters, the polygon union mask, the chroma scaling — is
// the runtime half and lives in `src/app/camera/roi_enhance.h`, because
// denso_core must never link OpenCV. That is the same domain/runtime line
// camera::CameraArea (core) and zone_assembly (app) already sit on: this header
// says WHICH values are legal and what a stored integer means; that one says what
// they DO. Neither restates the other.
//
// The bundle belongs to the CAMERA, not to the area and not to the model: what it
// compensates for is this camera's optics, exposure and lighting, and every area
// a camera owns is drawn on one frame under one light. Where it is APPLIED is a
// different question, and the answer there is the union of that camera's areas.
//
// EVERYTHING IS AN INTEGER. There is deliberately no float on this struct or in
// the schema: gamma is fixed-point hundredths (100 == 1.00), so every field has
// an exact representation, an exact equality, an exact CHECK constraint and an
// exact round trip through SQLite. Floats would give none of those and buy
// nothing an operator can perceive.
#pragma once

namespace denso::camera {

/// Local-contrast (CLAHE) strength. The persisted encoding is the enumerator's
/// integer value, so this ORDER IS A FILE FORMAT: never renumber, never reuse,
/// only append. `Off` must stay 0 — the column's `DEFAULT 0` is what makes the
/// upgrade behaviour-preserving, and that default means "Off" only because Off
/// is zero.
enum class RoiEnhancement {
    Off = 0,
    Low = 1,
    Medium = 2,
    High = 3,
};

inline constexpr int kMinRoiEnhancement = 0;
inline constexpr int kMaxRoiEnhancement = 3;

/// The ONE range predicate, so no validator re-spells the bound.
inline constexpr bool roi_enhancement_in_range(int raw) {
    return raw >= kMinRoiEnhancement && raw <= kMaxRoiEnhancement;
}

/// Decode a stored local-contrast level. ANY out-of-range, negative or unknown
/// value resolves to `Off` — never to a level.
///
/// The fail-safe DIRECTION is the contract, and it is the same one
/// `parse_display_mode` and `parse_target_mode` follow. A database can arrive
/// from a backup, a hand edit, or a build that appended a level this one does not
/// have; none of those may switch image processing ON for a camera whose operator
/// never asked for it.
RoiEnhancement parse_roi_enhancement(int raw);

/// The persisted integer for a level. Total, so a round trip cannot lose a value.
inline constexpr int to_int(RoiEnhancement level) { return static_cast<int>(level); }

/// Operator-facing label ("Off" / "Low" / "Medium" / "High"). UI text, NOT a
/// persisted token — the integer is the format. It lives here so no surface can
/// drift into naming the same level differently.
const char* roi_enhancement_label(RoiEnhancement level);

// ─── The tuning bounds ───────────────────────────────────────────────────────
//
// Bounded on purpose, and bounded HERE: the UI, the repository, the database
// CHECK constraints and the image pipeline all read these constants, so widening
// a range is one edit rather than five that can disagree.
//
// Every NEUTRAL value is the value that leaves the image alone, and every neutral
// value is also the column DEFAULT — which is what makes an upgraded database
// behave exactly as it did before this feature existed.

inline constexpr int kMinBrightness = -100;
inline constexpr int kMaxBrightness = 100;
inline constexpr int kNeutralBrightness = 0;

inline constexpr int kMinContrast = -100;
inline constexpr int kMaxContrast = 100;
inline constexpr int kNeutralContrast = 0;

/// Gamma in HUNDREDTHS: 50 = 0.50, 100 = 1.00, 300 = 3.00.
inline constexpr int kMinGamma = 50;
inline constexpr int kMaxGamma = 300;
inline constexpr int kNeutralGamma = 100;

inline constexpr int kMinSaturation = -100;
inline constexpr int kMaxSaturation = 100;
inline constexpr int kNeutralSaturation = 0;

/// One camera's complete Image Enhancement configuration.
///
/// `enabled` is a MASTER SWITCH held separately from the tuning, so turning the
/// feature off preserves what the operator tuned and turning it back on restores
/// it. Implementing "off" by zeroing the sliders would destroy field-calibration
/// work that can only be redone in front of the real meter.
struct ImageEnhancement {
    bool enabled = false;
    RoiEnhancement local_contrast = RoiEnhancement::Off;
    int brightness = kNeutralBrightness;
    int contrast = kNeutralContrast;
    int gamma = kNeutralGamma;      ///< hundredths; 100 == 1.00
    int saturation = kNeutralSaturation;

    bool operator==(const ImageEnhancement&) const = default;
};

/// The configuration a fresh camera gets, and the one every migrated row lands
/// on: disabled and neutral.
inline constexpr ImageEnhancement neutral_enhancement() { return ImageEnhancement{}; }

/// True when every tuning value is its neutral value — i.e. this configuration
/// would leave the image unchanged even if it were enabled.
constexpr bool is_neutral(const ImageEnhancement& e) {
    return e.local_contrast == RoiEnhancement::Off &&
           e.brightness == kNeutralBrightness && e.contrast == kNeutralContrast &&
           e.gamma == kNeutralGamma && e.saturation == kNeutralSaturation;
}

/// True when this configuration would actually change pixels: the master switch
/// is on AND at least one control is off its neutral value.
///
/// THE gate. `RoiEnhancer::make` returns nullptr when this is false, so a
/// disabled — or enabled-but-neutral — camera constructs no CLAHE, builds no
/// mask, allocates no scratch buffer and does no colour-space round trip. That
/// last part matters for correctness as well as cost: BGR->Lab->BGR is not
/// bit-exact, so a neutral configuration that still made the trip would perturb
/// the model's input by a rounding step for no reason at all.
constexpr bool has_effect(const ImageEnhancement& e) {
    return e.enabled && !is_neutral(e);
}

/// Clamp every field into its legal range, leaving already-legal values alone.
ImageEnhancement clamp_enhancement(ImageEnhancement e);

/// Decode a stored bundle. Out-of-range numeric fields are CLAMPED into range
/// (they are continuous, so the nearest legal value is the nearest intent), while
/// an unknown local-contrast level resolves to Off (it is an enumeration, so a
/// value this build does not know is not "nearly" any level it does know).
///
/// Fail-safe in the direction that matters: nothing a corrupt database can say
/// turns enhancement ON for a camera whose operator never enabled it, because
/// `enabled` is decoded as "exactly 1", not "non-zero-ish".
ImageEnhancement parse_enhancement(int enabled, int local_contrast, int brightness,
                                   int contrast, int gamma, int saturation);

} // namespace denso::camera
