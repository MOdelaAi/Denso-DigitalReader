#include "camera/roi_enhance.h"

#include "camera/frame_convert.h"   // qimage_to_mat, mat_to_qimage

#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cmath>
#include <utility>

namespace denso::ui {

namespace {

/// Saturating round to a byte. Every curve below ends here, which is what makes
/// "no control can produce an invalid pixel value" structural rather than a range
/// argument about each formula.
uchar to_byte(double v) {
    return static_cast<uchar>(std::clamp(std::lround(v), 0L, 255L));
}

/// Lab's a/b planes are stored 0..255 with 128 meaning "no chroma".
constexpr double kChromaCentre = 128.0;
/// Contrast pivots about mid-grey so raising it does not also brighten.
constexpr double kTonePivot = 128.0;

}  // namespace

// ─── The local-contrast table ───────────────────────────────────────────────
//
// INITIAL values, chosen to be conservative and to leave room in BOTH directions
// after on-site calibration. They are not qualified for any particular meter.
//
// Only the clip limit varies. OpenCV's CLAHE has exactly two knobs, and moving
// both at once would make an operator's "Medium looked better than High" report
// uninterpretable — there would be no way to tell which knob did it. The tile
// grid is therefore fixed at the 8 x 8 default (reduced only for a small region,
// see effective_tiles) so the four levels lie on one axis.
//
// Why these numbers: OpenCV's default clipLimit is 40, which is not a contrast
// setting so much as "no clipping at all" — on a meter face it turns sensor grain
// into strokes the detector reads as digits. The useful band for document and
// instrument imagery is roughly 1..5, so the levels sample it evenly and stop
// well short of the point where noise dominates. Low is deliberately close to a
// no-op: it must be possible for an operator to conclude that a small nudge is
// enough.
ClaheParams clahe_params(camera::RoiEnhancement level) {
    switch (level) {
        case camera::RoiEnhancement::Low:
            return ClaheParams{1.5, 8};
        case camera::RoiEnhancement::Medium:
            return ClaheParams{2.5, 8};
        case camera::RoiEnhancement::High:
            return ClaheParams{4.0, 8};
        case camera::RoiEnhancement::Off:
            break;
    }
    // Off is not a configuration — no CLAHE pass runs at that level, so this
    // value exists only to make the function total.
    return ClaheParams{0.0, 0};
}

cv::Size effective_tiles(const ClaheParams& params, int w, int h) {
    if (params.tile_grid <= 0 || w <= 0 || h <= 0) {
        return cv::Size(1, 1);
    }
    // Integer division IS the rule: w / kMinClaheTilePx is the largest number of
    // tiles whose edge is still at least kMinClaheTilePx. Never more than the
    // nominal grid, never fewer than one.
    const int nx = std::clamp(w / kMinClaheTilePx, 1, params.tile_grid);
    const int ny = std::clamp(h / kMinClaheTilePx, 1, params.tile_grid);
    return cv::Size(nx, ny);
}

// ─── The tone curve ─────────────────────────────────────────────────────────
//
// Brightness, contrast and gamma are all per-pixel monotone functions of
// luminance, so they compose into ONE table computed once. Composing them
// per-pixel instead would be three passes over the region for the same answer.
cv::Mat build_tone_lut(const camera::ImageEnhancement& cfg) {
    const bool neutral = cfg.brightness == camera::kNeutralBrightness &&
                         cfg.contrast == camera::kNeutralContrast &&
                         cfg.gamma == camera::kNeutralGamma;
    if (neutral) {
        return cv::Mat();   // identity: the caller skips the pass entirely
    }

    const double offset = cfg.brightness * kBrightnessScale;
    const double gain = 1.0 + (cfg.contrast / 100.0) * kContrastScale;
    // Stored in hundredths, so 100 == 1.00. The EXPONENT is the reciprocal, which
    // is the conventional direction: gamma above 1.00 lifts the midtones.
    const double gamma = cfg.gamma / 100.0;
    const double exponent = 1.0 / gamma;
    const bool gamma_neutral = cfg.gamma == camera::kNeutralGamma;

    cv::Mat lut(1, 256, CV_8U);
    uchar* out = lut.ptr<uchar>();
    for (int v = 0; v < 256; ++v) {
        double x = static_cast<double>(v);
        x += offset;                                   // 1. brightness
        x = (x - kTonePivot) * gain + kTonePivot;      // 2. contrast, about mid-grey
        if (!gamma_neutral) {                          // 3. gamma
            // Clamped BEFORE the power: std::pow of a negative base with a
            // fractional exponent is NaN, and a contrast cut can push x below 0.
            const double norm = std::clamp(x, 0.0, 255.0) / 255.0;
            x = 255.0 * std::pow(norm, exponent);
        }
        out[v] = to_byte(x);
    }
    return lut;
}

cv::Mat build_chroma_lut(const camera::ImageEnhancement& cfg) {
    if (cfg.saturation == camera::kNeutralSaturation) {
        return cv::Mat();   // identity: the caller skips the pass entirely
    }
    // -100 -> 0.0 (fully desaturated), 0 -> 1.0, +100 -> 2.0.
    const double scale = 1.0 + cfg.saturation * kSaturationScale;
    cv::Mat lut(1, 256, CV_8U);
    uchar* out = lut.ptr<uchar>();
    for (int v = 0; v < 256; ++v) {
        out[v] = to_byte((v - kChromaCentre) * scale + kChromaCentre);
    }
    return lut;
}

cv::Mat build_area_mask(const std::vector<camera::CameraArea>& areas, int w, int h) {
    if (w <= 0 || h <= 0) {
        return cv::Mat();
    }

    std::vector<std::vector<cv::Point>> polys;
    polys.reserve(areas.size());
    for (const camera::CameraArea& a : areas) {
        if (a.points.size() < 3) {
            continue;  // not a polygon — the same bar the canvas and repo apply
        }
        std::vector<cv::Point> poly;
        poly.reserve(a.points.size());
        for (const camera::Point& p : a.points) {
            // Normalized [0,1] -> pixels, clamped INSIDE the frame. A stored
            // polygon can sit exactly on 1.0 (a drag clamps to the border), and
            // w is not a valid column index.
            const int px = std::clamp(
                static_cast<int>(std::lround(static_cast<double>(p.x) * w)), 0, w - 1);
            const int py = std::clamp(
                static_cast<int>(std::lround(static_cast<double>(p.y) * h)), 0, h - 1);
            poly.emplace_back(px, py);
        }
        polys.push_back(std::move(poly));
    }
    if (polys.empty()) {
        return cv::Mat();  // no usable polygon == no confinement == whole frame
    }

    cv::Mat mask(h, w, CV_8UC1, cv::Scalar(0));
    // ONE fillPoly CALL PER POLYGON, never one call with every contour.
    //
    // cv::fillPoly given several contours at once fills them with the EVEN-ODD
    // rule, so the intersection of two overlapping Areas would come out as a
    // HOLE — the exact opposite of the union this mask must be. Areas are allowed
    // to overlap, so that is not a hypothetical. Filling one at a time makes
    // overlap a plain re-write of 255: a pixel is in the mask or it is not, and
    // it can never be "in twice".
    //
    // This is the same even-odd trap camera::point_in_polygon documents on the
    // detection side; it bites here for the same reason.
    for (const std::vector<cv::Point>& poly : polys) {
        cv::fillPoly(mask, std::vector<std::vector<cv::Point>>{poly}, cv::Scalar(255),
                     cv::LINE_8);
    }
    return mask;
}

std::unique_ptr<RoiEnhancer> RoiEnhancer::make(const camera::ImageEnhancement& cfg,
                                               std::vector<camera::CameraArea> areas,
                                               EnhanceScope scope) {
    if (!camera::has_effect(cfg)) {
        // Disabled, or enabled with every control neutral. Either way there is
        // nothing to do, and doing nothing means building NOTHING: no lookup
        // table, no CLAHE, no mask — and, importantly, no Lab round trip, which
        // is not bit-exact and would otherwise perturb the model's input for no
        // reason. Every caller reads null as "the unenhanced pipeline".
        return nullptr;
    }
    // make_unique cannot reach the private constructor; the raw new is contained
    // in this one line and adopted immediately.
    return std::unique_ptr<RoiEnhancer>(
        new RoiEnhancer(cfg, std::move(areas), scope));
}

RoiEnhancer::RoiEnhancer(const camera::ImageEnhancement& cfg,
                         std::vector<camera::CameraArea> areas, EnhanceScope scope)
    : cfg_(camera::clamp_enhancement(cfg)), scope_(scope), areas_(std::move(areas)) {
    // Both curves are computed ONCE, here — they depend only on the configuration.
    // An empty Mat means "identity", and the pass is skipped rather than run with
    // a table that would change nothing.
    tone_lut_ = build_tone_lut(cfg_);
    chroma_lut_ = build_chroma_lut(cfg_);
    if (cfg_.local_contrast != camera::RoiEnhancement::Off) {
        const ClaheParams p = clahe_params(cfg_.local_contrast);
        // One CLAHE per enhancer, per camera. The tile grid is set in
        // ensure_geometry() once the working region is known.
        clahe_ = cv::createCLAHE(p.clip_limit, cv::Size(p.tile_grid, p.tile_grid));
    }
}

void RoiEnhancer::ensure_geometry(int w, int h) {
    if (w == geom_w_ && h == geom_h_) {
        return;  // a camera's frame size is stable — this runs once per stream
    }
    geom_w_ = w;
    geom_h_ = h;
    mask_ = build_area_mask(areas_, w, h);

    if (mask_.empty() || scope_ == EnhanceScope::WholeFrame) {
        // No usable Area means the detection region IS the whole frame, so the
        // enhancement region is too — the two regions are the same region by
        // construction rather than by a rule stated twice.
        region_ = cv::Rect(0, 0, w, h);
    } else {
        // boundingRect over a single-channel 8-bit image is the bounding box of
        // its non-zero pixels.
        region_ = cv::boundingRect(mask_);
    }
    if (region_.width <= 0 || region_.height <= 0) {
        region_ = cv::Rect();  // an all-zero mask: nothing to do, and apply() bails
    }
    if (clahe_) {
        clahe_->setTilesGridSize(
            effective_tiles(clahe_params(cfg_.local_contrast), region_.width,
                            region_.height));
    }
}

void RoiEnhancer::apply(cv::Mat& bgr) {
    // Refuse anything unexpected rather than converting it. Changing a frame's
    // shape or depth here would land downstream in letterbox() and blobFromImage(),
    // i.e. in the model's input contract.
    if (bgr.empty() || bgr.type() != CV_8UC3) {
        return;
    }
    ensure_geometry(bgr.cols, bgr.rows);
    if (region_.width <= 0 || region_.height <= 0) {
        return;
    }

    // A VIEW into `bgr`, not a copy — everything written through `roi` lands in
    // the caller's Mat in place.
    cv::Mat roi = bgr(region_);

    // ONE conversion for the whole pass. L carries the tone work, a/b carry the
    // colour work, and neither disturbs the other.
    cv::cvtColor(roi, lab_, cv::COLOR_BGR2Lab);
    cv::split(lab_, planes_);

    // 1-3. brightness -> contrast -> gamma, composed into one table.
    if (!tone_lut_.empty()) {
        cv::LUT(planes_[0], tone_lut_, l_out_);
        // copyTo, NOT `planes_[0] = l_out_`. Assigning would make planes_[0] share
        // l_out_'s buffer; the next frame's cv::split() would then write its L
        // plane straight into l_out_ (create() matches on size/type and returns
        // early, it does not un-share), and the LUT/CLAHE below would be reading
        // and writing one buffer. That is a silent, frame-rate-dependent
        // corruption.
        l_out_.copyTo(planes_[0]);
    }
    // 4. local contrast, on the tone-mapped luminance.
    if (clahe_) {
        clahe_->apply(planes_[0], l_out_);
        l_out_.copyTo(planes_[0]);
    }
    // 5. saturation, on the chroma planes only — the same affine map for both, so
    //    one table serves them.
    if (!chroma_lut_.empty()) {
        cv::LUT(planes_[1], chroma_lut_, l_out_);
        l_out_.copyTo(planes_[1]);
        cv::LUT(planes_[2], chroma_lut_, l_out_);
        l_out_.copyTo(planes_[2]);
    }

    cv::merge(planes_, lab_);
    cv::cvtColor(lab_, enhanced_, cv::COLOR_Lab2BGR);

    if (mask_.empty()) {
        enhanced_.copyTo(roi);  // whole frame: every pixel is in the region
    } else {
        // THE guarantee: pixels where the mask is 0 are not written at all, so
        // everything outside the union of the Areas stays byte-identical to what
        // the caller handed in.
        enhanced_.copyTo(roi, mask_(region_));
    }
}

QImage enhance_preview(const QImage& oriented, const camera::ImageEnhancement& cfg,
                       const std::vector<camera::CameraArea>& areas,
                       EnhanceScope scope) {
    // Built per call rather than cached: a preview render happens when the
    // operator moves a control, not per frame, and rebuilding is what guarantees
    // the curves and the mask match the settings and polygons as they are RIGHT
    // NOW — including a slider dragged a moment ago and never saved.
    std::unique_ptr<RoiEnhancer> enhancer = RoiEnhancer::make(cfg, areas, scope);
    if (!enhancer || oriented.isNull()) {
        return oriented;  // no effect (or nothing to show) is the original, untouched
    }
    cv::Mat bgr = qimage_to_mat(oriented);
    if (bgr.empty()) {
        return oriented;
    }
    enhancer->apply(bgr);
    const QImage out = mat_to_qimage(bgr);
    return out.isNull() ? oriented : out;
}

} // namespace denso::ui
