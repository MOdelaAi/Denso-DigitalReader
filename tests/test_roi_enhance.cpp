// The Image Enhancement AUTHORITY: the domain bundle, the tone and chroma
// curves, the strength table, the union mask, and the preview wrapper that
// shares all of them with the runtime.
//
// Everything here is synthetic — generated images and hand-built polygons. No
// camera, no database, no inference backend.
//
// The properties these cases exist to pin down, in order of how expensive they
// would be to discover in the field:
//
//   * DISABLED changes nothing, and allocates nothing to do it. So does an
//     enabled-but-neutral configuration: neutral is not "a transform that
//     happens to be the identity", it is no transform at all, because the Lab
//     round trip is not bit-exact and would perturb the model's input for free.
//   * Every control's neutral value is a no-op ON ITS OWN, and every control
//     measurably moves a suitable fixture.
//   * Pixels outside the union of the areas are byte-identical afterwards.
//   * Overlapping areas union — an overlapped pixel is not enhanced twice, and
//     (the trap) the intersection is not punched out as an even-odd hole.
//   * Shape, type and channel count survive, because letterbox() and
//     blobFromImage() downstream are a model input contract.
#include <catch2/catch_test_macros.hpp>

#include "camera/camera.h"
#include "camera/roi_enhance.h"
#include "camera/roi_enhancement.h"

#include <QImage>

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

#include <cmath>
#include <string>
#include <vector>

using denso::camera::CameraArea;
using denso::camera::clamp_enhancement;
using denso::camera::has_effect;
using denso::camera::ImageEnhancement;
using denso::camera::is_neutral;
using denso::camera::neutral_enhancement;
using denso::camera::parse_enhancement;
using denso::camera::parse_roi_enhancement;
using denso::camera::Point;
using denso::camera::RoiEnhancement;
using denso::camera::roi_enhancement_label;
using denso::camera::to_int;
using denso::ui::build_area_mask;
using denso::ui::build_chroma_lut;
using denso::ui::build_tone_lut;
using denso::ui::clahe_params;
using denso::ui::effective_tiles;
using denso::ui::EnhanceScope;
using denso::ui::enhance_preview;
using denso::ui::RoiEnhancer;

namespace {

constexpr int kW = 320;
constexpr int kH = 240;

CameraArea rect_area(float x1, float y1, float x2, float y2) {
    CameraArea a;
    a.points = {Point{x1, y1}, Point{x2, y1}, Point{x2, y2}, Point{x1, y2}};
    return a;
}

/// An enabled bundle with exactly one control moved off neutral.
ImageEnhancement only_local(RoiEnhancement l) {
    ImageEnhancement e;
    e.enabled = true;
    e.local_contrast = l;
    return e;
}
ImageEnhancement only_brightness(int v) {
    ImageEnhancement e;
    e.enabled = true;
    e.brightness = v;
    return e;
}
ImageEnhancement only_contrast(int v) {
    ImageEnhancement e;
    e.enabled = true;
    e.contrast = v;
    return e;
}
ImageEnhancement only_gamma(int v) {
    ImageEnhancement e;
    e.enabled = true;
    e.gamma = v;
    return e;
}
ImageEnhancement only_saturation(int v) {
    ImageEnhancement e;
    e.enabled = true;
    e.saturation = v;
    return e;
}

/// A deliberately LOW-CONTRAST frame: a faint gradient plus a barely-visible
/// darker block, all squeezed into a narrow band around mid-grey. This is the
/// situation the feature exists for — a washed-out meter face.
cv::Mat low_contrast_bgr(int w = kW, int h = kH) {
    cv::Mat m(h, w, CV_8UC3);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            int v = 120 + (x * 12) / w + (y * 6) / h;   // ~120..138
            if (x > w / 4 && x < w / 2 && y > h / 4 && y < h / 2) {
                v -= 6;   // a faint "digit"
            }
            m.at<cv::Vec3b>(y, x) = cv::Vec3b(static_cast<uchar>(v),
                                              static_cast<uchar>(v),
                                              static_cast<uchar>(v));
        }
    }
    return m;
}

/// A tinted frame with texture, for the colour cases.
cv::Mat tinted_bgr(int w = kW, int h = kH) {
    cv::Mat m(h, w, CV_8UC3, cv::Scalar(40, 90, 160));   // BGR, orange-ish
    cv::rectangle(m, cv::Rect(w / 4, h / 4, w / 4, h / 4),
                  cv::Scalar(55, 105, 175), cv::FILLED);
    return m;
}

cv::Mat saturated_bgr(int w = kW, int h = kH) {
    cv::Mat m(h, w, CV_8UC3, cv::Scalar(0, 0, 0));
    m(cv::Rect(0, 0, w / 2, h)).setTo(cv::Scalar(255, 255, 255));
    return m;
}

int diff_count(const cv::Mat& a, const cv::Mat& b) {
    REQUIRE(a.size() == b.size());
    REQUIRE(a.type() == b.type());
    cv::Mat d;
    cv::absdiff(a, b, d);
    cv::Mat flat;
    cv::cvtColor(d, flat, cv::COLOR_BGR2GRAY);
    return cv::countNonZero(flat);
}

bool unchanged_outside(const cv::Mat& before, const cv::Mat& after,
                       const cv::Mat& mask) {
    for (int y = 0; y < before.rows; ++y) {
        for (int x = 0; x < before.cols; ++x) {
            if (mask.at<uchar>(y, x) != 0) {
                continue;
            }
            if (before.at<cv::Vec3b>(y, x) != after.at<cv::Vec3b>(y, x)) {
                return false;
            }
        }
    }
    return true;
}

/// Run one configuration over a whole-frame fixture and hand back the result.
cv::Mat run(const ImageEnhancement& cfg, const cv::Mat& src) {
    cv::Mat out = src.clone();
    auto e = RoiEnhancer::make(cfg, {});
    if (e) {
        e->apply(out);
    }
    return out;
}

/// Mean Rec.601 luma of the RGB values — "how bright does this look".
double mean_luma(const cv::Mat& bgr) {
    cv::Mat gray;
    cv::cvtColor(bgr, gray, cv::COLOR_BGR2GRAY);
    return cv::mean(gray)[0];
}

/// Mean Lab L — the channel the tone controls actually operate on.
///
/// Deliberately distinct from mean_luma above, and the distinction is the point
/// for the saturation case: Lab L and Rec.601 luma are different functions, so
/// changing chroma at constant L legitimately moves the luma of the reconstructed
/// RGB (and out-of-gamut colours clip on the way back, moving it further).
/// Asserting "saturation does not change brightness" against luma would be
/// asserting something untrue about colour spaces; L is the quantity the
/// implementation promises to leave alone.
double mean_lab_L(const cv::Mat& bgr) {
    cv::Mat lab;
    cv::cvtColor(bgr, lab, cv::COLOR_BGR2Lab);
    std::vector<cv::Mat> p;
    cv::split(lab, p);
    return cv::mean(p[0])[0];
}

/// Mean distance of the Lab chroma planes from neutral — "how colourful".
double mean_chroma(const cv::Mat& bgr) {
    cv::Mat lab;
    cv::cvtColor(bgr, lab, cv::COLOR_BGR2Lab);
    std::vector<cv::Mat> p;
    cv::split(lab, p);
    cv::Mat da;
    cv::Mat db;
    cv::absdiff(p[1], 128.0, da);
    cv::absdiff(p[2], 128.0, db);
    return cv::mean(da)[0] + cv::mean(db)[0];
}

}  // namespace

// ─── 1. The domain bundle ────────────────────────────────────────────────────

TEST_CASE("the persisted encodings are a stable, closed set", "[roi_enhance]") {
    // The integers ARE the file format (schema v18's columns). If any of these
    // moved, every appliance in the field would come back tuned differently after
    // an upgrade — silently.
    CHECK(to_int(RoiEnhancement::Off) == 0);
    CHECK(to_int(RoiEnhancement::Low) == 1);
    CHECK(to_int(RoiEnhancement::Medium) == 2);
    CHECK(to_int(RoiEnhancement::High) == 3);

    // Gamma is fixed-point hundredths, so the neutral value is exactly 100 and
    // there is no float anywhere on the struct.
    CHECK(denso::camera::kNeutralGamma == 100);
    CHECK(denso::camera::kNeutralBrightness == 0);
    CHECK(denso::camera::kNeutralContrast == 0);
    CHECK(denso::camera::kNeutralSaturation == 0);
}

TEST_CASE("a default bundle is disabled and neutral", "[roi_enhance]") {
    const ImageEnhancement d;
    CHECK_FALSE(d.enabled);
    CHECK(d.local_contrast == RoiEnhancement::Off);
    CHECK(d.brightness == 0);
    CHECK(d.contrast == 0);
    CHECK(d.gamma == 100);
    CHECK(d.saturation == 0);
    CHECK(is_neutral(d));
    CHECK_FALSE(has_effect(d));
    CHECK(neutral_enhancement() == d);
}

TEST_CASE("has_effect is the ONE gate: enabled AND not neutral", "[roi_enhance]") {
    ImageEnhancement e;
    CHECK_FALSE(has_effect(e));               // disabled + neutral

    e.enabled = true;
    CHECK_FALSE(has_effect(e));               // enabled but nothing moved
    CHECK(is_neutral(e));

    // Each control alone is enough to make it effective…
    CHECK(has_effect(only_local(RoiEnhancement::Low)));
    CHECK(has_effect(only_brightness(1)));
    CHECK(has_effect(only_contrast(-1)));
    CHECK(has_effect(only_gamma(101)));
    CHECK(has_effect(only_saturation(-1)));

    // …but NONE of them matters while the master switch is off. That is the whole
    // point of holding the switch separately: turning it off must preserve the
    // tuning, not erase it.
    ImageEnhancement tuned = only_brightness(40);
    tuned.local_contrast = RoiEnhancement::High;
    tuned.gamma = 180;
    tuned.saturation = -30;
    tuned.enabled = false;
    CHECK_FALSE(has_effect(tuned));
    CHECK_FALSE(is_neutral(tuned));           // the values are still there
    CHECK(tuned.brightness == 40);
    CHECK(tuned.gamma == 180);
}

TEST_CASE("parsing is fail-safe in the direction that matters", "[roi_enhance]") {
    // `enabled` is "exactly 1", never "non-zero-ish": nothing a corrupt database
    // can say may switch processing ON for a camera whose operator never did.
    CHECK_FALSE(parse_enhancement(0, 0, 0, 0, 100, 0).enabled);
    CHECK(parse_enhancement(1, 0, 0, 0, 100, 0).enabled);
    CHECK_FALSE(parse_enhancement(2, 0, 0, 0, 100, 0).enabled);
    CHECK_FALSE(parse_enhancement(-1, 0, 0, 0, 100, 0).enabled);

    // An unknown local-contrast level is an ENUMERATION, so it resolves to Off
    // rather than to the nearest level this build happens to have.
    CHECK(parse_enhancement(1, 9, 0, 0, 100, 0).local_contrast == RoiEnhancement::Off);
    CHECK(parse_enhancement(1, -3, 0, 0, 100, 0).local_contrast == RoiEnhancement::Off);
    CHECK(parse_roi_enhancement(4) == RoiEnhancement::Off);

    // The numeric controls are CONTINUOUS, so the nearest legal value IS the
    // nearest intent and clamping is right.
    const ImageEnhancement wild = parse_enhancement(1, 2, 9999, -9999, 9999, -9999);
    CHECK(wild.brightness == denso::camera::kMaxBrightness);
    CHECK(wild.contrast == denso::camera::kMinContrast);
    CHECK(wild.gamma == denso::camera::kMaxGamma);
    CHECK(wild.saturation == denso::camera::kMinSaturation);
}

TEST_CASE("clamping leaves legal values alone", "[roi_enhance]") {
    ImageEnhancement e = only_brightness(37);
    e.contrast = -12;
    e.gamma = 175;
    e.saturation = 60;
    CHECK(clamp_enhancement(e) == e);
}

TEST_CASE("labels are defined for every level", "[roi_enhance]") {
    CHECK(std::string(roi_enhancement_label(RoiEnhancement::Off)) == "Off");
    CHECK(std::string(roi_enhancement_label(RoiEnhancement::Low)) == "Low");
    CHECK(std::string(roi_enhancement_label(RoiEnhancement::Medium)) == "Medium");
    CHECK(std::string(roi_enhancement_label(RoiEnhancement::High)) == "High");
}

// ─── 2. The strength table ───────────────────────────────────────────────────

TEST_CASE("strength mapping is deterministic and monotone", "[roi_enhance]") {
    const auto low = clahe_params(RoiEnhancement::Low);
    const auto med = clahe_params(RoiEnhancement::Medium);
    const auto high = clahe_params(RoiEnhancement::High);

    CHECK(clahe_params(RoiEnhancement::Medium) == med);   // deterministic

    // Monotone in the ONE varying axis. An operator comparing Low/Medium/High
    // must be comparing a single quantity.
    CHECK(low.clip_limit < med.clip_limit);
    CHECK(med.clip_limit < high.clip_limit);
    CHECK(low.tile_grid == med.tile_grid);
    CHECK(med.tile_grid == high.tile_grid);

    // Well inside the sane band: OpenCV's default of 40 is effectively "no
    // clipping", which amplifies sensor noise into strokes.
    CHECK(high.clip_limit <= 8.0);
    CHECK(low.clip_limit >= 1.0);
}

TEST_CASE("the tile grid shrinks for a small region but never below one",
          "[roi_enhance]") {
    const auto p = clahe_params(RoiEnhancement::Medium);
    CHECK(effective_tiles(p, 1920, 1080) == cv::Size(p.tile_grid, p.tile_grid));

    const cv::Size small = effective_tiles(p, 120, 40);
    CHECK(small.width < p.tile_grid);
    CHECK(small.height < p.tile_grid);
    CHECK(small.width >= 1);
    CHECK(small.height >= 1);

    CHECK(effective_tiles(p, 1, 1) == cv::Size(1, 1));
    CHECK(effective_tiles(p, 0, 0) == cv::Size(1, 1));
    CHECK(effective_tiles(p, -5, -5) == cv::Size(1, 1));
}

// ─── 3. The curves ───────────────────────────────────────────────────────────

TEST_CASE("a neutral tone curve is no curve at all", "[roi_enhance]") {
    // Empty means "skip the pass", which is stronger than an identity table: it
    // is one fewer full-region LUT per frame.
    CHECK(build_tone_lut(neutral_enhancement()).empty());
    CHECK(build_tone_lut(only_local(RoiEnhancement::High)).empty());   // not a tone control
    CHECK(build_tone_lut(only_saturation(50)).empty());                // nor is this
}

TEST_CASE("each tone control builds a curve and stays in range", "[roi_enhance]") {
    for (const ImageEnhancement& cfg :
         {only_brightness(40), only_brightness(-40), only_contrast(50),
          only_contrast(-50), only_gamma(200), only_gamma(60)}) {
        const cv::Mat lut = build_tone_lut(cfg);
        REQUIRE_FALSE(lut.empty());
        CHECK(lut.type() == CV_8U);
        CHECK(lut.total() == 256);
        // Every entry is a byte by construction (the curve saturates), so the
        // only thing left to prove is that it MOVED.
        bool moved = false;
        for (int v = 0; v < 256; ++v) {
            if (lut.at<uchar>(0, v) != static_cast<uchar>(v)) {
                moved = true;
                break;
            }
        }
        CHECK(moved);
    }
}

TEST_CASE("the tone curve is monotone non-decreasing", "[roi_enhance]") {
    // A tone curve that folded back on itself would map two different input
    // luminances onto the same output in one place and invert them in another —
    // visible as posterised bands and a detector reading strokes that are not
    // there.
    for (const ImageEnhancement& cfg :
         {only_brightness(70), only_contrast(90), only_gamma(280), only_gamma(55)}) {
        const cv::Mat lut = build_tone_lut(cfg);
        REQUIRE_FALSE(lut.empty());
        for (int v = 1; v < 256; ++v) {
            REQUIRE(lut.at<uchar>(0, v) >= lut.at<uchar>(0, v - 1));
        }
    }
}

TEST_CASE("gamma above 1.00 lifts the midtones, below 1.00 lowers them",
          "[roi_enhance]") {
    // The direction is a convention and an operator expectation, so it is pinned.
    const cv::Mat up = build_tone_lut(only_gamma(200));
    const cv::Mat down = build_tone_lut(only_gamma(50));
    REQUIRE_FALSE(up.empty());
    REQUIRE_FALSE(down.empty());
    CHECK(up.at<uchar>(0, 128) > 128);
    CHECK(down.at<uchar>(0, 128) < 128);
    // The endpoints are fixed points of a pure gamma curve.
    CHECK(up.at<uchar>(0, 0) == 0);
    CHECK(up.at<uchar>(0, 255) == 255);
}

TEST_CASE("a neutral chroma curve is no curve at all", "[roi_enhance]") {
    CHECK(build_chroma_lut(neutral_enhancement()).empty());
    CHECK(build_chroma_lut(only_brightness(50)).empty());
    const cv::Mat lut = build_chroma_lut(only_saturation(-100));
    REQUIRE_FALSE(lut.empty());
    // Fully desaturated: every chroma value collapses onto the neutral centre.
    CHECK(lut.at<uchar>(0, 0) == 128);
    CHECK(lut.at<uchar>(0, 255) == 128);
    CHECK(lut.at<uchar>(0, 128) == 128);
}

// ─── 4. The union mask ───────────────────────────────────────────────────────

TEST_CASE("no usable polygon means an empty mask (whole frame)", "[roi_enhance]") {
    CHECK(build_area_mask({}, kW, kH).empty());
    CameraArea two;
    two.points = {Point{0.1f, 0.1f}, Point{0.5f, 0.5f}};
    CHECK(build_area_mask({two}, kW, kH).empty());
}

TEST_CASE("the mask covers the drawn polygon and nothing else", "[roi_enhance]") {
    const cv::Mat mask = build_area_mask({rect_area(0.25f, 0.25f, 0.75f, 0.75f)},
                                         kW, kH);
    REQUIRE_FALSE(mask.empty());
    CHECK(mask.type() == CV_8UC1);
    CHECK(mask.cols == kW);
    CHECK(mask.rows == kH);
    CHECK(mask.at<uchar>(kH / 2, kW / 2) == 255);
    CHECK(mask.at<uchar>(2, 2) == 0);
    CHECK(mask.at<uchar>(kH - 3, kW - 3) == 0);
}

TEST_CASE("overlapping areas union — the intersection is NOT punched out",
          "[roi_enhance]") {
    // THE trap: cv::fillPoly given several contours in ONE call fills them
    // even-odd, so the overlap of two areas would come back as a HOLE. Areas are
    // allowed to overlap, so that is not hypothetical — it would silently leave
    // the most-covered part of the scene un-enhanced.
    const cv::Mat mask = build_area_mask({rect_area(0.10f, 0.10f, 0.60f, 0.60f),
                                          rect_area(0.40f, 0.40f, 0.90f, 0.90f)},
                                         kW, kH);
    REQUIRE_FALSE(mask.empty());
    CHECK(mask.at<uchar>(kH / 2, kW / 2) == 255);
    CHECK(mask.at<uchar>(static_cast<int>(0.20 * kH),
                         static_cast<int>(0.20 * kW)) == 255);
    CHECK(mask.at<uchar>(static_cast<int>(0.80 * kH),
                         static_cast<int>(0.80 * kW)) == 255);
    CHECK(mask.at<uchar>(static_cast<int>(0.95 * kH),
                         static_cast<int>(0.05 * kW)) == 0);

    double lo = 0.0;
    double hi = 0.0;
    cv::minMaxLoc(mask, &lo, &hi);
    CHECK(lo == 0.0);
    CHECK(hi == 255.0);   // binary: no "enhanced twice" state to represent
}

TEST_CASE("a polygon on the frame border stays inside the mask", "[roi_enhance]") {
    const cv::Mat mask = build_area_mask({rect_area(0.0f, 0.0f, 1.0f, 1.0f)}, kW, kH);
    REQUIRE_FALSE(mask.empty());
    CHECK(mask.at<uchar>(0, 0) == 255);
    CHECK(mask.at<uchar>(kH - 1, kW - 1) == 255);
}

// ─── 5. Nothing to do means nothing is built ─────────────────────────────────

TEST_CASE("a bundle with no effect builds no enhancer at all", "[roi_enhance]") {
    // Not "an enhancer that does nothing" — no object. That is what makes
    // "disabled costs nothing" structural: nothing to call per frame, no LUT, no
    // CLAHE, no mask, and no Lab round trip.
    CHECK(RoiEnhancer::make(neutral_enhancement(), {}) == nullptr);

    ImageEnhancement enabled_neutral;
    enabled_neutral.enabled = true;
    CHECK(RoiEnhancer::make(enabled_neutral, {}) == nullptr);

    // Fully tuned but switched off.
    ImageEnhancement tuned = only_brightness(60);
    tuned.local_contrast = RoiEnhancement::High;
    tuned.gamma = 220;
    tuned.saturation = 45;
    tuned.enabled = false;
    CHECK(RoiEnhancer::make(tuned, {rect_area(0.2f, 0.2f, 0.8f, 0.8f)}) == nullptr);

    // …and the moment it is switched on, it builds.
    tuned.enabled = true;
    CHECK(RoiEnhancer::make(tuned, {}) != nullptr);
}

TEST_CASE("a neutral configuration returns the ORIGINAL pixels exactly",
          "[roi_enhance]") {
    // Byte-identical, not "close": the Lab round trip is lossy, so a neutral
    // configuration that still made the trip would move the model's input for
    // nothing. Proving equality here is proving the trip did not happen.
    const cv::Mat original = low_contrast_bgr();
    ImageEnhancement enabled_neutral;
    enabled_neutral.enabled = true;
    CHECK(diff_count(run(enabled_neutral, original), original) == 0);
    CHECK(diff_count(run(neutral_enhancement(), original), original) == 0);
}

TEST_CASE("each control's neutral value is a no-op on its own", "[roi_enhance]") {
    const cv::Mat original = low_contrast_bgr();
    CHECK(diff_count(run(only_brightness(0), original), original) == 0);
    CHECK(diff_count(run(only_contrast(0), original), original) == 0);
    CHECK(diff_count(run(only_gamma(100), original), original) == 0);
    CHECK(diff_count(run(only_saturation(0), original), original) == 0);
    CHECK(diff_count(run(only_local(RoiEnhancement::Off), original), original) == 0);
}

TEST_CASE("Off leaves a preview image untouched", "[roi_enhance]") {
    QImage src(64, 48, QImage::Format_RGB888);
    src.fill(QColor(90, 100, 110));
    CHECK(enhance_preview(src, neutral_enhancement(),
                          {rect_area(0.2f, 0.2f, 0.8f, 0.8f)}) == src);
}

// ─── 6. Each control does something, in the right direction ──────────────────

TEST_CASE("brightness moves luminance in the requested direction",
          "[roi_enhance]") {
    const cv::Mat original = low_contrast_bgr();
    CHECK(mean_luma(run(only_brightness(60), original)) > mean_luma(original));
    CHECK(mean_luma(run(only_brightness(-60), original)) < mean_luma(original));
}

TEST_CASE("contrast widens or narrows the luminance spread", "[roi_enhance]") {
    const cv::Mat original = low_contrast_bgr();
    const auto spread = [](const cv::Mat& m) {
        cv::Mat gray;
        cv::cvtColor(m, gray, cv::COLOR_BGR2GRAY);
        cv::Scalar mu;
        cv::Scalar sigma;
        cv::meanStdDev(gray, mu, sigma);
        return sigma[0];
    };
    CHECK(spread(run(only_contrast(90), original)) > spread(original));
    CHECK(spread(run(only_contrast(-90), original)) < spread(original));
}

TEST_CASE("gamma lifts or lowers the midtones of a real frame", "[roi_enhance]") {
    const cv::Mat original = low_contrast_bgr();
    CHECK(mean_luma(run(only_gamma(220), original)) > mean_luma(original));
    CHECK(mean_luma(run(only_gamma(60), original)) < mean_luma(original));
}

TEST_CASE("saturation changes chroma and leaves luminance where it was",
          "[roi_enhance]") {
    // The whole reason saturation is a chroma operation and not a per-channel
    // multiply: turning the colour down must not also darken the picture, because
    // the reader works on luminance.
    const cv::Mat original = tinted_bgr();
    const cv::Mat down = run(only_saturation(-100), original);
    const cv::Mat up = run(only_saturation(80), original);

    CHECK(mean_chroma(down) < mean_chroma(original));
    CHECK(mean_chroma(up) > mean_chroma(original));
    // The LUMINANCE CHANNEL is untouched — that is the actual guarantee, and it
    // holds to within the Lab round trip's rounding.
    CHECK(std::abs(mean_lab_L(down) - mean_lab_L(original)) < 1.0);
    CHECK(std::abs(mean_lab_L(up) - mean_lab_L(original)) < 1.0);
}

TEST_CASE("saturation scales each chroma plane from ITS OWN input",
          "[roi_enhance]") {
    // Pins WHICH Lab plane feeds the chroma curve — `a` from `a`, `b` from `b`.
    //
    // The coarse direction checks above cannot see a wrong-plane wiring, and that
    // is exactly why this case exists: if the curve were fed from L, then at
    // -100 every output would still collapse to the neutral 128 (the curve maps
    // ALL inputs there, whatever they are) and at +80 the output would still sit
    // further from 128 than the input did. "Desaturates" and "adds colour" would
    // both still look true while every pixel took the wrong colour.
    //
    // The only assertion that distinguishes the planes is the exact affine map,
    // per plane, per pixel: out = 128 + (in - 128) * scale, where `in` is that
    // plane's OWN value.
    //
    // Saturation -50 is chosen deliberately: desaturating moves a colour toward
    // grey, which is always inside the gamut, so the Lab -> BGR -> Lab round trip
    // cannot clip and the residual is pure rounding.
    // Four PRIMARIES, chosen because each one puts L far from both a and b (a
    // muted palette does not: a washed-out orange can sit with L within a few
    // levels of its own `a`, and the substitution would then be invisible).
    cv::Mat original(64, 64, CV_8UC3);
    const cv::Vec3b quadrants[4] = {
        cv::Vec3b(0, 0, 255),      // red      L~136 a~208 b~195
        cv::Vec3b(255, 0, 0),      // blue     L~82  a~207 b~20
        cv::Vec3b(255, 0, 255),    // magenta  L~154 a~226 b~67
        cv::Vec3b(255, 255, 0),    // cyan     L~232 a~80  b~114
    };
    for (int y = 0; y < 64; ++y) {
        for (int x = 0; x < 64; ++x) {
            original.at<cv::Vec3b>(y, x) =
                quadrants[(y < 32 ? 0 : 2) + (x < 32 ? 0 : 1)];
        }
    }

    const cv::Mat out = run(only_saturation(-50), original);
    REQUIRE(out.size() == original.size());
    REQUIRE(out.type() == CV_8UC3);

    cv::Mat lab_in;
    cv::Mat lab_out;
    cv::cvtColor(original, lab_in, cv::COLOR_BGR2Lab);
    cv::cvtColor(out, lab_out, cv::COLOR_BGR2Lab);
    std::vector<cv::Mat> pin;
    std::vector<cv::Mat> pout;
    cv::split(lab_in, pin);
    cv::split(lab_out, pout);

    // The fixture must actually SEPARATE L from the chroma planes, or this whole
    // case would pass no matter which one fed the curve. Asserted rather than
    // assumed: a fixture that stopped discriminating (someone "simplifies" the
    // colours) would otherwise silently turn this into a test of nothing.
    //
    // How much separation is enough is arithmetic, not taste. Feeding the curve
    // from L would put the output at 128 + (L - 128) * kScale instead of
    // 128 + (in - 128) * kScale, an error of |L - in| * kScale. To exceed kTol
    // that needs |L - in| > kTol / kScale, i.e. 6 levels. The bar below is
    // 24 — four times the detection threshold — so the case fails loudly rather
    // than marginally.
    constexpr int kMinPlaneSeparation = 24;
    int discriminating = 0;
    for (int y = 0; y < 64; y += 4) {
        for (int x = 0; x < 64; x += 4) {
            const int L = pin[0].at<uchar>(y, x);
            const int a = pin[1].at<uchar>(y, x);
            const int b = pin[2].at<uchar>(y, x);
            if (std::abs(L - a) > kMinPlaneSeparation &&
                std::abs(L - b) > kMinPlaneSeparation) {
                ++discriminating;
            }
        }
    }
    CHECK(discriminating == 16 * 16);   // every sampled pixel separates L from a and b

    // -50 -> scale 0.5. Each plane must follow the map from ITS OWN input.
    constexpr double kScale = 0.5;
    constexpr double kTol = 3.0;   // Lab -> BGR -> Lab rounding, no clipping
    int wrong_a = 0;
    int wrong_b = 0;
    for (int y = 0; y < 64; ++y) {
        for (int x = 0; x < 64; ++x) {
            const double want_a = 128.0 + (pin[1].at<uchar>(y, x) - 128.0) * kScale;
            const double want_b = 128.0 + (pin[2].at<uchar>(y, x) - 128.0) * kScale;
            if (std::abs(pout[1].at<uchar>(y, x) - want_a) > kTol) ++wrong_a;
            if (std::abs(pout[2].at<uchar>(y, x) - want_b) > kTol) ++wrong_b;
        }
    }
    CHECK(wrong_a == 0);
    CHECK(wrong_b == 0);

    // And L is left alone entirely — it is not a chroma plane.
    int wrong_L = 0;
    for (int y = 0; y < 64; ++y) {
        for (int x = 0; x < 64; ++x) {
            if (std::abs(pout[0].at<uchar>(y, x) - pin[0].at<uchar>(y, x)) > kTol) {
                ++wrong_L;
            }
        }
    }
    CHECK(wrong_L == 0);
}

TEST_CASE("every local-contrast level changes the image", "[roi_enhance]") {
    const cv::Mat original = low_contrast_bgr();
    const auto shift = [&](RoiEnhancement l) {
        cv::Mat d;
        cv::absdiff(run(only_local(l), original), original, d);
        return cv::sum(d)[0] + cv::sum(d)[1] + cv::sum(d)[2];
    };
    CHECK(shift(RoiEnhancement::Low) > 0.0);
    CHECK(shift(RoiEnhancement::Medium) > 0.0);
    CHECK(shift(RoiEnhancement::High) > 0.0);

    // There is deliberately NO assertion that a higher level moves more pixels.
    // It is not true in general, and this fixture demonstrates it: on a nearly
    // flat image a LOW clip limit clips more of each tile's histogram and
    // redistributes more, so Low can move further from the original than High
    // does. CLAHE's clipLimit orders how much local contrast is ALLOWED, not the
    // magnitude of the change on every scene.
    //
    // The ordering that IS guaranteed is the parameter one, asserted in the
    // strength-table case above. Anything stronger would be a claim about the
    // operator's meter that only the meter can settle.
}

TEST_CASE("the luminance controls do not recolour the image", "[roi_enhance]") {
    // Applying brightness/contrast/gamma/CLAHE to B, G and R independently pulls
    // the channels apart and shifts the hue — a new, unqualified input
    // distribution for a model trained on ordinary colour video. Working on L
    // alone must leave a strongly tinted region recognisably that colour.
    const cv::Mat original = tinted_bgr();
    for (const ImageEnhancement& cfg :
         {only_brightness(50), only_contrast(60), only_gamma(200),
          only_local(RoiEnhancement::High)}) {
        const cv::Mat out = run(cfg, original);
        for (int y = 0; y < kH; y += 11) {
            for (int x = 0; x < kW; x += 11) {
                const cv::Vec3b p = out.at<cv::Vec3b>(y, x);
                REQUIRE(p[0] < p[1]);
                REQUIRE(p[1] < p[2]);
            }
        }
        CHECK(diff_count(out, original) > 0);
    }
}

// ─── 7. Bounds and robustness ────────────────────────────────────────────────

TEST_CASE("no control can produce an invalid pixel value", "[roi_enhance]") {
    // Every extreme, on a fixture that already sits at both ends of the range.
    ImageEnhancement extreme;
    extreme.enabled = true;
    extreme.local_contrast = RoiEnhancement::High;
    extreme.brightness = denso::camera::kMaxBrightness;
    extreme.contrast = denso::camera::kMaxContrast;
    extreme.gamma = denso::camera::kMaxGamma;
    extreme.saturation = denso::camera::kMaxSaturation;

    for (const cv::Mat& fixture : {saturated_bgr(), low_contrast_bgr(), tinted_bgr()}) {
        for (int sign = 0; sign < 2; ++sign) {
            ImageEnhancement cfg = extreme;
            if (sign == 1) {
                cfg.brightness = denso::camera::kMinBrightness;
                cfg.contrast = denso::camera::kMinContrast;
                cfg.gamma = denso::camera::kMinGamma;
                cfg.saturation = denso::camera::kMinSaturation;
            }
            cv::Mat out = fixture.clone();
            auto e = RoiEnhancer::make(cfg, {});
            REQUIRE(e != nullptr);
            REQUIRE_NOTHROW(e->apply(out));
            CHECK(out.type() == CV_8UC3);
            CHECK(out.size() == fixture.size());
            CHECK(out.channels() == 3);
            // cv::Mat of CV_8UC3 cannot hold an out-of-range value, so the real
            // assertion is that nothing threw and the shape survived.
        }
    }
}

TEST_CASE("an unexpected Mat is left alone rather than converted",
          "[roi_enhance]") {
    auto e = RoiEnhancer::make(only_brightness(50), {});
    REQUIRE(e != nullptr);

    cv::Mat empty;
    REQUIRE_NOTHROW(e->apply(empty));
    CHECK(empty.empty());

    cv::Mat gray(kH, kW, CV_8UC1, cv::Scalar(128));
    const cv::Mat before = gray.clone();
    REQUIRE_NOTHROW(e->apply(gray));
    CHECK(gray.type() == CV_8UC1);
    CHECK(cv::countNonZero(gray != before) == 0);
}

// ─── 8. The ROI confinement ──────────────────────────────────────────────────

TEST_CASE("every control is confined to the union of the areas", "[roi_enhance]") {
    // A TINTED fixture, not the grey one: saturation cannot change a greyscale
    // image (there is no chroma to scale), so proving confinement for it needs a
    // frame that actually has colour. The rectangle inside gives CLAHE something
    // to work on too.
    const cv::Mat original = tinted_bgr();
    const CameraArea area = rect_area(0.25f, 0.25f, 0.75f, 0.75f);
    const cv::Mat mask = build_area_mask({area}, kW, kH);
    REQUIRE_FALSE(mask.empty());

    ImageEnhancement all;
    all.enabled = true;
    all.local_contrast = RoiEnhancement::Medium;
    all.brightness = 25;
    all.contrast = 30;
    all.gamma = 150;
    all.saturation = -40;

    for (const ImageEnhancement& cfg :
         {only_brightness(50), only_contrast(50), only_gamma(200),
          only_saturation(-80), only_local(RoiEnhancement::High), all}) {
        cv::Mat frame = original.clone();
        auto e = RoiEnhancer::make(cfg, {area});
        REQUIRE(e != nullptr);
        e->apply(frame);

        CHECK(frame.size() == original.size());
        CHECK(frame.type() == CV_8UC3);
        CHECK(diff_count(frame, original) > 0);          // something inside moved
        CHECK(unchanged_outside(original, frame, mask));  // nothing outside did
    }
}

TEST_CASE("overlapping areas are enhanced exactly once", "[roi_enhance]") {
    // Two overlapping areas must give the SAME result as one area covering their
    // union. If the implementation ever processed per area, the overlap would be
    // transformed twice and these would differ.
    const cv::Mat original = low_contrast_bgr();
    const CameraArea a = rect_area(0.10f, 0.10f, 0.60f, 0.90f);
    const CameraArea b = rect_area(0.40f, 0.10f, 0.90f, 0.90f);
    const CameraArea whole = rect_area(0.10f, 0.10f, 0.90f, 0.90f);

    ImageEnhancement cfg = only_brightness(45);
    cfg.local_contrast = RoiEnhancement::High;

    cv::Mat two = original.clone();
    RoiEnhancer::make(cfg, {a, b})->apply(two);
    cv::Mat one = original.clone();
    RoiEnhancer::make(cfg, {whole})->apply(one);
    CHECK(diff_count(two, one) == 0);
}

TEST_CASE("no areas enhances the whole frame, matching detection semantics",
          "[roi_enhance]") {
    const cv::Mat original = low_contrast_bgr();
    cv::Mat frame = original.clone();
    auto e = RoiEnhancer::make(only_brightness(50), {});
    REQUIRE(e != nullptr);
    CHECK(e->whole_frame());
    e->apply(frame);

    CHECK(e->mask().empty());
    CHECK(e->region() == cv::Rect(0, 0, kW, kH));
    CHECK(diff_count(frame, original) > 0);

    cv::Mat confined = original.clone();
    RoiEnhancer::make(only_brightness(50), {rect_area(0.4f, 0.4f, 0.6f, 0.6f)})
        ->apply(confined);
    CHECK(confined.at<cv::Vec3b>(1, 1) == original.at<cv::Vec3b>(1, 1));
}

TEST_CASE("both scopes leave the same pixels untouched", "[roi_enhance]") {
    const cv::Mat original = low_contrast_bgr();
    const CameraArea area = rect_area(0.3f, 0.3f, 0.5f, 0.5f);
    const cv::Mat mask = build_area_mask({area}, kW, kH);
    const ImageEnhancement cfg = only_local(RoiEnhancement::High);

    cv::Mat bounds = original.clone();
    RoiEnhancer::make(cfg, {area}, EnhanceScope::MaskBounds)->apply(bounds);
    cv::Mat whole = original.clone();
    RoiEnhancer::make(cfg, {area}, EnhanceScope::WholeFrame)->apply(whole);

    CHECK(unchanged_outside(original, bounds, mask));
    CHECK(unchanged_outside(original, whole, mask));

    auto def = RoiEnhancer::make(cfg, {area});
    REQUIRE(def != nullptr);
    CHECK(def->scope() == EnhanceScope::MaskBounds);
}

// ─── 9. The preview wrapper ──────────────────────────────────────────────────

TEST_CASE("preview runs the same transform as the runtime", "[roi_enhance]") {
    // The preview must not be a second implementation. Enhancing a QImage through
    // enhance_preview() and enhancing the equivalent Mat through RoiEnhancer
    // directly must land on the same pixels — that is the guarantee that the
    // operator is judging what the model will receive.
    const cv::Mat original = low_contrast_bgr(64, 48);
    const CameraArea area = rect_area(0.25f, 0.25f, 0.75f, 0.75f);

    ImageEnhancement cfg;
    cfg.enabled = true;
    cfg.local_contrast = RoiEnhancement::Medium;
    cfg.brightness = 20;
    cfg.contrast = 35;
    cfg.gamma = 160;
    cfg.saturation = -25;

    QImage img(64, 48, QImage::Format_RGB888);
    for (int y = 0; y < 48; ++y) {
        for (int x = 0; x < 64; ++x) {
            const cv::Vec3b p = original.at<cv::Vec3b>(y, x);
            img.setPixel(x, y, qRgb(p[2], p[1], p[0]));
        }
    }

    const QImage previewed = enhance_preview(img, cfg, {area});
    cv::Mat runtime = original.clone();
    RoiEnhancer::make(cfg, {area})->apply(runtime);

    REQUIRE(previewed.width() == runtime.cols);
    REQUIRE(previewed.height() == runtime.rows);
    int mismatches = 0;
    for (int y = 0; y < runtime.rows; ++y) {
        for (int x = 0; x < runtime.cols; ++x) {
            const cv::Vec3b p = runtime.at<cv::Vec3b>(y, x);
            const QRgb q = previewed.pixel(x, y);
            if (qRed(q) != p[2] || qGreen(q) != p[1] || qBlue(q) != p[0]) {
                ++mismatches;
            }
        }
    }
    CHECK(mismatches == 0);
}

TEST_CASE("re-previewing the ORIGINAL is idempotent", "[roi_enhance]") {
    // The page must always render from the stored snapshot. If it ever fed its
    // own output back in, a second render at the same settings would drift.
    QImage src(64, 48, QImage::Format_RGB888);
    for (int y = 0; y < 48; ++y) {
        for (int x = 0; x < 64; ++x) {
            const int v = 118 + (x % 9);
            src.setPixel(x, y, qRgb(v, v, v));
        }
    }
    const CameraArea area = rect_area(0.2f, 0.2f, 0.8f, 0.8f);
    const ImageEnhancement cfg = only_local(RoiEnhancement::Medium);

    const QImage once = enhance_preview(src, cfg, {area});
    const QImage again = enhance_preview(src, cfg, {area});
    CHECK(once == again);

    const QImage compounded = enhance_preview(once, cfg, {area});
    CHECK(compounded.size() == once.size());
}
