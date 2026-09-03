// THE image-enhancement authority for the Digital Number Reader: what a
// configuration does to a frame, and where on that frame it does it.
//
// It knows nothing about TensorRT, ONNX Runtime, Qt Widgets, SQLite or the
// wizard. It takes a configuration (camera::ImageEnhancement — the domain half,
// in denso_core), a camera's Area polygons, and a BGR cv::Mat. Everything the
// algorithm needs lives here so the runtime and the wizard preview share ONE
// implementation instead of two that drift.
//
// ── What it does ────────────────────────────────────────────────────────────
//
//   inference-copy BGR ─► union mask of ALL the camera's Areas
//                      ─► ONE BGR->Lab conversion over the working region
//                      ─► tone LUT on L   (brightness -> contrast -> gamma)
//                      ─► CLAHE on L      (local contrast)
//                      ─► chroma LUT on a,b (saturation)
//                      ─► Lab->BGR, copied back only where mask != 0
//
// ── The order, and why it is this order ─────────────────────────────────────
//
// 1. BRIGHTNESS, 2. CONTRAST and 3. GAMMA are per-pixel monotone functions of
//    luminance alone, so they compose into a SINGLE 256-entry lookup table built
//    once at construction. Brightness before contrast before gamma is the
//    conventional tone-curve order and the one an operator expects: set the
//    exposure, then stretch it, then bend the midtones.
// 4. CLAHE runs AFTER that curve, so local contrast is equalised on the image
//    the operator actually tuned rather than on the raw one.
// 5. SATURATION touches only the a/b chroma planes, so it neither reads nor
//    disturbs anything above it; its position in the order is not observable.
//
// Three properties are load-bearing and each is defended by a test:
//
//  * LUMINANCE ONLY for brightness/contrast/gamma/CLAHE. Applying any of them to
//    B, G and R independently shifts the channels against each other and
//    visibly recolours the image; the digit model was trained on ordinary colour
//    frames, so a hue shift is a new, unqualified input distribution. Lab keeps
//    the tone work on L and the colour work on a/b.
//  * ONE CLAHE PASS PER FRAME, never one per Area. Areas are unioned into a
//    single mask first, so overlapping Areas cannot enhance a pixel twice and
//    there is no per-Area strength to reconcile.
//  * PIXELS OUTSIDE THE MASK ARE UNTOUCHED, byte for byte. The masked copy-back
//    is what makes that structural rather than incidental.
//
// And one that is easy to lose: a configuration with NO EFFECT builds no
// enhancer at all (see make()), so a disabled — or enabled-but-neutral — camera
// does not even make the Lab round trip. That trip is not bit-exact, so doing it
// "harmlessly" would perturb the model's input by a rounding step for nothing.
//
// ── Thread safety ───────────────────────────────────────────────────────────
//
// A RoiEnhancer is NOT thread-safe and is not meant to be: it owns a cv::CLAHE
// plus reusable scratch buffers, and each DetectionProcessor owns exactly one,
// touched only from that processor's own inference worker. There is deliberately
// no shared instance — a cv::CLAHE shared between camera workers would need a
// mutex on the hottest path in the app for no benefit.
#pragma once

#include "camera/camera.h"            // CameraArea
#include "camera/roi_enhancement.h"   // ImageEnhancement (domain)

#include <QImage>

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>   // cv::CLAHE lives here, not in core

#include <memory>
#include <vector>

namespace denso::ui {

/// The CLAHE configuration one local-contrast level selects.
///
/// `tile_grid` is the NOMINAL grid; the grid actually used is reduced for a small
/// region so a tile never falls below kMinClaheTilePx — see effective_tiles().
struct ClaheParams {
    double clip_limit = 0.0;
    int tile_grid = 0;   ///< nominal N for an N x N grid

    bool operator==(const ClaheParams&) const = default;
};

/// The smallest tile edge, in pixels, that CLAHE is allowed to work on.
///
/// A meter ROI can easily be 120 x 40 px. An 8 x 8 grid over that gives 15 x 5 px
/// tiles, and a histogram of 75 pixels is mostly noise — equalising it amplifies
/// sensor grain into something the detector reads as strokes. Reducing the grid
/// instead keeps every tile statistically meaningful at any ROI size.
inline constexpr int kMinClaheTilePx = 16;

// ── Slider -> pixel-maths scale factors ─────────────────────────────────────
//
// The operator sees bounded, unitless numbers; these turn them into the actual
// coefficients. They live here, next to the code that applies them, and nowhere
// else — the UI never sees a coefficient and the domain never sees a float.

/// Brightness: UI -100..+100 -> an additive offset on L of -80..+80 (L is 0..255).
/// Stops short of the full range so the extremes stay usable rather than clipping
/// the whole ROI to black or white.
inline constexpr double kBrightnessScale = 0.8;

/// Contrast: UI -100..+100 -> a multiplier about mid-grey of 0.25 .. 1.75.
inline constexpr double kContrastScale = 0.75;

/// Saturation: UI -100..+100 -> a chroma multiplier of 0.0 .. 2.0, so -100 is
/// fully desaturated and +100 is double chroma.
inline constexpr double kSaturationScale = 0.01;

/// Where the enhancement pass is computed. Both scopes copy back through the SAME
/// union mask, so both leave the identical set of pixels untouched; they differ
/// only in which pixels take part in the CLAHE histograms.
enum class EnhanceScope {
    /// Over the bounding rectangle of the union mask (the default). The histogram
    /// that brightens a digit is then built from the instrument face and its
    /// immediate surroundings, not diluted by an unrelated background that may
    /// differ between two cameras pointed at the same meter. It is also the
    /// cheaper path by a wide margin: for a small ROI on a 1080p frame it does a
    /// few percent of the colour-space conversion work.
    MaskBounds,
    /// Over the whole frame, then masked back. Kept as a first-class option
    /// because it is the only version whose result is independent of where the
    /// Areas happen to sit, which is the safer answer if field tuning shows
    /// MaskBounds is too sensitive to ROI size.
    WholeFrame,
};

/// THE local-contrast -> CLAHE mapping. `Off` yields `{0, 0}`, which is never
/// used: no CLAHE pass runs at that level.
///
/// One function, one place to retune after the on-site calibration. Nothing else
/// in the tree may hold a clip limit or a tile count.
ClaheParams clahe_params(camera::RoiEnhancement level);

/// The grid actually used for a `w` x `h` region: the nominal grid reduced until
/// every tile is at least kMinClaheTilePx on a side, floored at 1 x 1. Pure and
/// deterministic, so a camera's enhancement cannot drift between runs.
cv::Size effective_tiles(const ClaheParams& params, int w, int h);

/// The 256-entry tone curve for L: brightness, then contrast, then gamma, with
/// every result saturated into 0..255. Returns a 1x256 CV_8U Mat ready for
/// cv::LUT. An all-neutral configuration yields the identity curve.
///
/// Exposed because it is the whole of the tone maths and is worth asserting
/// directly — monotonicity, neutrality and the absence of out-of-range output are
/// properties of this table, not of a frame.
cv::Mat build_tone_lut(const camera::ImageEnhancement& cfg);

/// The 256-entry chroma curve for the Lab a/b planes, which are centred on 128.
/// Neutral saturation yields the identity curve.
cv::Mat build_chroma_lut(const camera::ImageEnhancement& cfg);

/// The union mask of every polygon in `areas`, rasterised at `w` x `h`
/// (CV_8UC1, 0 or 255). Overlap is a plain union: a pixel is either in or out.
///
/// Returns an EMPTY Mat when no usable polygon exists (no areas, or none with
/// three or more vertices). Empty means "no confinement", which callers read as
/// the whole frame — the same meaning an empty `areas` already has for detection
/// filtering (`camera::inside_any_area`), so the enhancement region and the
/// detection region are the same region by construction.
cv::Mat build_area_mask(const std::vector<camera::CameraArea>& areas, int w, int h);

/// One camera's enhancement pass: owns its lookup tables, its cv::CLAHE, its
/// rasterised mask and its scratch buffers, and applies them in place.
class RoiEnhancer {
public:
    /// The ONLY constructor. Returns nullptr when the configuration has no effect
    /// — disabled, or enabled with every control neutral — so "off costs nothing"
    /// is a structural property of the type rather than a branch some caller has
    /// to remember. A null enhancer is the unenhanced state everywhere
    /// downstream.
    static std::unique_ptr<RoiEnhancer> make(const camera::ImageEnhancement& cfg,
                                             std::vector<camera::CameraArea> areas,
                                             EnhanceScope scope = EnhanceScope::MaskBounds);

    /// Enhance `bgr` in place inside the union mask. Size, type and channel count
    /// are preserved exactly.
    ///
    /// A no-op — leaving `bgr` byte-identical — when it is empty or not CV_8UC3.
    /// Refusing an unexpected input rather than converting it keeps this off the
    /// list of things that can change a frame's shape underneath the letterbox.
    void apply(cv::Mat& bgr);

    const camera::ImageEnhancement& config() const { return cfg_; }
    EnhanceScope scope() const { return scope_; }

    /// True when this camera has no usable Area polygon, so the whole frame is
    /// the detection region and therefore the enhancement region.
    bool whole_frame() const { return areas_.empty(); }

    /// The region the last apply() computed over, and the mask it copied back
    /// through (empty mask == unmasked whole-region copy). Test observability
    /// only; no production behaviour reads them.
    const cv::Rect& region() const { return region_; }
    const cv::Mat& mask() const { return mask_; }

    RoiEnhancer(const RoiEnhancer&) = delete;
    RoiEnhancer& operator=(const RoiEnhancer&) = delete;

private:
    RoiEnhancer(const camera::ImageEnhancement& cfg,
                std::vector<camera::CameraArea> areas, EnhanceScope scope);

    /// Rasterise the mask and resolve the working region for a frame of this
    /// size. Cached: a camera's frame size is stable, so this runs once per
    /// stream, not once per frame.
    void ensure_geometry(int w, int h);

    camera::ImageEnhancement cfg_;
    EnhanceScope scope_;
    std::vector<camera::CameraArea> areas_;   // empty == whole frame

    cv::Mat tone_lut_;           // 1x256 CV_8U, or empty when the tone is neutral
    cv::Mat chroma_lut_;         // 1x256 CV_8U, or empty when saturation is neutral
    cv::Ptr<cv::CLAHE> clahe_;   // null at Off; owned by THIS enhancer, never shared

    cv::Mat mask_;               // CV_8UC1 union mask, or empty for whole-frame
    cv::Rect region_;            // where apply() computes
    int geom_w_ = 0;             // frame size the cache was built for
    int geom_h_ = 0;

    // Scratch, reused across frames. Safe because one enhancer is touched by one
    // thread (see the header note) — this is what keeps the per-frame allocation
    // count flat instead of growing with the frame rate.
    cv::Mat lab_;
    cv::Mat enhanced_;
    cv::Mat l_out_;
    std::vector<cv::Mat> planes_;
};

/// The WIZARD PREVIEW entry point — and deliberately nothing more than a wrapper
/// around the very same RoiEnhancer the inference worker runs.
///
/// This exists so the operator cannot possibly be shown one transformation while
/// the model receives another. There is no second tone curve, no second strength
/// table, no second mask rasteriser and no second colour path here: change
/// `build_tone_lut`, `clahe_params` or `build_area_mask` and BOTH the preview and
/// the runtime move together, because there is only one of each to change.
///
/// `oriented` is the wizard's snapshot AFTER apply_orientation, matching the
/// runtime's branch point exactly (the inference copy is taken from the oriented
/// frame). `areas` is the wizard's CURRENT WORKING geometry, unsaved edits and
/// all, and `cfg` its CURRENT WORKING configuration — the page owns both, so it
/// is the page that calls this.
///
/// Returns `oriented` unchanged when the configuration has no effect or the image
/// is unusable. Callers must always pass the ORIGINAL snapshot: feeding a
/// previously enhanced image back in would compound the transform and show
/// something the runtime never produces.
QImage enhance_preview(const QImage& oriented, const camera::ImageEnhancement& cfg,
                       const std::vector<camera::CameraArea>& areas,
                       EnhanceScope scope = EnhanceScope::MaskBounds);

} // namespace denso::ui
