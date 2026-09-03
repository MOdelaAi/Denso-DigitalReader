// Where the Digital ROI enhancement sits in the running pipeline.
//
// The inspection that preceded this feature established the one structural fact
// everything here depends on: the display Mat and the inference Mat are taken
// from the same oriented frame, and inference gets its own copy BEFORE any
// overlay is drawn. The enhancement is applied to that copy, on the inference
// worker — so these cases assert the two halves separately:
//
//   * the engine receives ENHANCED pixels inside the areas (and only there);
//   * the image handed back for display is the RAW frame, untouched.
//
// The failure that would matter most in the field is the second one being wrong
// in the quiet direction: an operator's wall silently showing a processed
// picture, so that what they see is no longer evidence about what the camera
// sends. There is also a case for the Ball Leveler, which must not consume this
// setting at all.
//
// Everything is synthetic: a stub engine that records what it was handed, and
// generated frames. No camera, no database, no GPU.
#include <catch2/catch_test_macros.hpp>

#include "camera/camera.h"
#include "camera/frame_processor.h"
#include "camera/level_processor.h"
#include "camera/roi_enhance.h"
#include "camera/roi_enhancement.h"
#include "detection/inference_engine.h"
#include "level/calibration.h"

#include <QImage>

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using denso::camera::CameraArea;
using denso::camera::Point;
using denso::camera::ImageEnhancement;
using denso::camera::RoiEnhancement;
using denso::ui::BallLevelProcessor;
using denso::ui::build_area_mask;
using denso::ui::Detection;
using denso::ui::DetectionProcessor;
using denso::ui::InferenceEngine;

namespace {

constexpr int kW = 256;
constexpr int kH = 192;

/// A stub backend that CLONES and keeps the Mat it was handed. That recorded Mat
/// is the whole point: it is literally the model's input, so asserting on it is
/// asserting on what the detector sees.
class CapturingEngine : public InferenceEngine {
public:
    std::vector<Detection> infer(const cv::Mat& bgr) override {
        std::lock_guard<std::mutex> lk(m_);
        last_ = bgr.clone();
        ++calls_;
        return {};   // no detections: nothing is drawn on the display path
    }
    const std::vector<std::string>& class_names() const override { return names_; }

    int calls() const {
        std::lock_guard<std::mutex> lk(m_);
        return calls_;
    }
    cv::Mat last() const {
        std::lock_guard<std::mutex> lk(m_);
        return last_.clone();
    }

private:
    mutable std::mutex m_;
    cv::Mat last_;
    int calls_ = 0;
    std::vector<std::string> names_;
};

/// An enabled bundle at a given local-contrast level, everything else neutral.
ImageEnhancement enabled_at(RoiEnhancement level) {
    ImageEnhancement e;
    e.enabled = true;
    e.local_contrast = level;
    return e;
}

/// An enabled bundle exercising every control at once — what a field-calibrated
/// camera actually looks like.
ImageEnhancement fully_tuned() {
    ImageEnhancement e;
    e.enabled = true;
    e.local_contrast = RoiEnhancement::Medium;
    e.brightness = 30;
    e.contrast = 25;
    e.gamma = 160;
    e.saturation = -35;
    return e;
}

CameraArea rect_area(float x1, float y1, float x2, float y2) {
    CameraArea a;
    a.points = {Point{x1, y1}, Point{x2, y1}, Point{x2, y2}, Point{x1, y2}};
    return a;
}

/// A low-contrast frame as a QImage — what a washed-out meter looks like coming
/// off the capture thread.
QImage faded_frame() {
    QImage img(kW, kH, QImage::Format_RGB888);
    for (int y = 0; y < kH; ++y) {
        for (int x = 0; x < kW; ++x) {
            int v = 118 + (x * 14) / kW + (y * 5) / kH;
            if (x > kW / 4 && x < kW / 2 && y > kH / 4 && y < kH / 2) {
                v -= 5;
            }
            img.setPixel(x, y, qRgb(v, v, v));
        }
    }
    return img;
}

/// The frame as the processors see it after conversion: BGR CV_8UC3.
cv::Mat as_bgr(const QImage& img) {
    cv::Mat out(img.height(), img.width(), CV_8UC3);
    for (int y = 0; y < img.height(); ++y) {
        for (int x = 0; x < img.width(); ++x) {
            const QRgb p = img.pixel(x, y);
            out.at<cv::Vec3b>(y, x) = cv::Vec3b(static_cast<uchar>(qBlue(p)),
                                                static_cast<uchar>(qGreen(p)),
                                                static_cast<uchar>(qRed(p)));
        }
    }
    return out;
}

bool images_identical(const QImage& a, const QImage& b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (int y = 0; y < a.height(); ++y) {
        for (int x = 0; x < a.width(); ++x) {
            if (a.pixel(x, y) != b.pixel(x, y)) {
                return false;
            }
        }
    }
    return true;
}

int diff_count(const cv::Mat& a, const cv::Mat& b) {
    if (a.size() != b.size() || a.type() != b.type()) {
        return -1;
    }
    cv::Mat d;
    cv::absdiff(a, b, d);
    cv::Mat gray;
    cv::cvtColor(d, gray, cv::COLOR_BGR2GRAY);
    return cv::countNonZero(gray);
}

bool unchanged_outside(const cv::Mat& before, const cv::Mat& after,
                       const cv::Mat& mask) {
    for (int y = 0; y < before.rows; ++y) {
        for (int x = 0; x < before.cols; ++x) {
            if (mask.at<uchar>(y, x) == 0 &&
                before.at<cv::Vec3b>(y, x) != after.at<cv::Vec3b>(y, x)) {
                return false;
            }
        }
    }
    return true;
}

/// Feed frames until the worker has run at least `want` inferences, or give up.
/// The inference worker is asynchronous by design (that is what keeps video
/// smooth), so a test has to pump rather than assume.
template <typename Proc>
QImage pump(Proc& proc, const CapturingEngine& engine, const QImage& frame,
            int want = 1) {
    QImage shown;
    for (int i = 0; i < 200 && engine.calls() < want; ++i) {
        shown = proc.process(frame);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    // One more, so the returned display image is from a settled pipeline.
    shown = proc.process(frame);
    return shown;
}

}  // namespace

// ─── 1. Off changes nothing ──────────────────────────────────────────────────

TEST_CASE("disabled: the model receives exactly the oriented frame",
          "[roi_enhance][pipeline]") {
    const QImage frame = faded_frame();
    const cv::Mat expected = as_bgr(frame);

    CapturingEngine engine;
    std::vector<DetectionProcessor::ModelRun> runs;
    runs.push_back({&engine, {}, {}});
    DetectionProcessor proc(0, 0.0, 0.0, std::move(runs),
                            {rect_area(0.25f, 0.25f, 0.75f, 0.75f)},
                            /*camera_id=*/1, nullptr, nullptr, {}, {},
                            ImageEnhancement{});

    const QImage shown = pump(proc, engine, frame);
    REQUIRE(engine.calls() >= 1);

    // Byte-for-byte the pipeline as it was before this feature existed.
    CHECK(diff_count(engine.last(), expected) == 0);
    CHECK(images_identical(shown, frame));
}

// ─── 2. An enabled level reaches inference, and only inference ───────────────

TEST_CASE("the model receives enhanced pixels inside the areas — and the "
          "display does not", "[roi_enhance][pipeline]") {
    const QImage frame = faded_frame();
    const cv::Mat original = as_bgr(frame);
    const CameraArea area = rect_area(0.25f, 0.25f, 0.75f, 0.75f);
    const cv::Mat mask = build_area_mask({area}, kW, kH);
    REQUIRE_FALSE(mask.empty());

    CapturingEngine engine;
    std::vector<DetectionProcessor::ModelRun> runs;
    runs.push_back({&engine, {}, {}});
    DetectionProcessor proc(0, 0.0, 0.0, std::move(runs), {area},
                            /*camera_id=*/1, nullptr, nullptr, {}, {},
                            fully_tuned());

    const QImage shown = pump(proc, engine, frame);
    REQUIRE(engine.calls() >= 1);
    const cv::Mat seen = engine.last();

    // The model's input contract is intact…
    REQUIRE(seen.size() == original.size());
    REQUIRE(seen.type() == CV_8UC3);
    // …something inside the area moved…
    CHECK(diff_count(seen, original) > 0);
    // …and nothing outside it did.
    CHECK(unchanged_outside(original, seen, mask));

    // THE display guarantee: the operator's wall still shows the picture the
    // camera actually sent. If this ever fails, the dashboard has stopped being
    // evidence about the camera.
    CHECK(images_identical(shown, frame));
}

TEST_CASE("with no areas the whole inference frame is enhanced",
          "[roi_enhance][pipeline]") {
    // Matching the detection semantics: no areas already means "detect on the
    // whole frame", so the enhanced region follows it there.
    const QImage frame = faded_frame();
    const cv::Mat original = as_bgr(frame);

    CapturingEngine engine;
    std::vector<DetectionProcessor::ModelRun> runs;
    runs.push_back({&engine, {}, {}});
    DetectionProcessor proc(0, 0.0, 0.0, std::move(runs), /*areas=*/{},
                            /*camera_id=*/1, nullptr, nullptr, {}, {},
                            enabled_at(RoiEnhancement::Medium));

    const QImage shown = pump(proc, engine, frame);
    REQUIRE(engine.calls() >= 1);
    CHECK(diff_count(engine.last(), original) > 0);
    CHECK(images_identical(shown, frame));   // display still raw
}

// ─── 3. Cameras are independent ──────────────────────────────────────────────

TEST_CASE("two cameras hold different levels at the same time",
          "[roi_enhance][pipeline]") {
    // Each camera owns its own processor, its own enhancer and its own CLAHE.
    // Nothing is shared, so one camera's level cannot reach another's frames.
    const QImage frame = faded_frame();
    const cv::Mat original = as_bgr(frame);
    const CameraArea area = rect_area(0.2f, 0.2f, 0.8f, 0.8f);

    CapturingEngine off_engine;
    CapturingEngine high_engine;

    std::vector<DetectionProcessor::ModelRun> off_runs;
    off_runs.push_back({&off_engine, {}, {}});
    DetectionProcessor off(0, 0.0, 0.0, std::move(off_runs), {area}, 1, nullptr,
                           nullptr, {}, {}, ImageEnhancement{});

    std::vector<DetectionProcessor::ModelRun> high_runs;
    high_runs.push_back({&high_engine, {}, {}});
    DetectionProcessor high(0, 0.0, 0.0, std::move(high_runs), {area}, 2, nullptr,
                            nullptr, {}, {}, fully_tuned());

    pump(off, off_engine, frame);
    pump(high, high_engine, frame);
    REQUIRE(off_engine.calls() >= 1);
    REQUIRE(high_engine.calls() >= 1);

    CHECK(diff_count(off_engine.last(), original) == 0);   // camera 1: untouched
    CHECK(diff_count(high_engine.last(), original) > 0);   // camera 2: enhanced
}

TEST_CASE("the master switch alone decides what the model receives",
          "[roi_enhance][pipeline]") {
    // Same fully tuned bundle, twice, differing ONLY in `enabled`. The disabled
    // one must hand the model the untouched frame — proof that "off" is honoured
    // at the processor and not merely in the UI — while the tuning it carries is
    // still right there in the struct.
    const QImage frame = faded_frame();
    const cv::Mat original = as_bgr(frame);
    const CameraArea area = rect_area(0.25f, 0.25f, 0.75f, 0.75f);

    ImageEnhancement off = fully_tuned();
    off.enabled = false;
    REQUIRE(off.brightness == fully_tuned().brightness);   // nothing was erased

    CapturingEngine off_engine;
    std::vector<DetectionProcessor::ModelRun> off_runs;
    off_runs.push_back({&off_engine, {}, {}});
    DetectionProcessor off_proc(0, 0.0, 0.0, std::move(off_runs), {area}, 1,
                                nullptr, nullptr, {}, {}, off);
    pump(off_proc, off_engine, frame);
    REQUIRE(off_engine.calls() >= 1);
    CHECK(diff_count(off_engine.last(), original) == 0);

    CapturingEngine on_engine;
    std::vector<DetectionProcessor::ModelRun> on_runs;
    on_runs.push_back({&on_engine, {}, {}});
    DetectionProcessor on_proc(0, 0.0, 0.0, std::move(on_runs), {area}, 2, nullptr,
                               nullptr, {}, {}, fully_tuned());
    pump(on_proc, on_engine, frame);
    REQUIRE(on_engine.calls() >= 1);
    CHECK(diff_count(on_engine.last(), original) > 0);
}

TEST_CASE("an enabled but neutral bundle changes nothing",
          "[roi_enhance][pipeline]") {
    // Neutral is not "a transform that happens to be the identity" — the Lab
    // round trip is lossy, so the processor must build no enhancer at all.
    const QImage frame = faded_frame();
    const cv::Mat original = as_bgr(frame);

    ImageEnhancement enabled_neutral;
    enabled_neutral.enabled = true;

    CapturingEngine engine;
    std::vector<DetectionProcessor::ModelRun> runs;
    runs.push_back({&engine, {}, {}});
    DetectionProcessor proc(0, 0.0, 0.0, std::move(runs),
                            {rect_area(0.2f, 0.2f, 0.8f, 0.8f)}, 1, nullptr,
                            nullptr, {}, {}, enabled_neutral);
    const QImage shown = pump(proc, engine, frame);
    REQUIRE(engine.calls() >= 1);
    CHECK(diff_count(engine.last(), original) == 0);
    CHECK(images_identical(shown, frame));
}

// ─── 4. The Ball Leveler ignores it entirely ─────────────────────────────────

TEST_CASE("BallLevelProcessor never enhances its inference frame",
          "[roi_enhance][pipeline][ball]") {
    // Structurally, BallLevelProcessor's constructor has no enhancement
    // parameter, so there is no way to hand it one — this case proves the
    // consequence: whatever a camera's persisted bundle says, the Ball model
    // receives the oriented frame unmodified. A Digital-only field-calibration
    // tool must not silently change a level measurement.
    const QImage frame = faded_frame();
    const cv::Mat original = as_bgr(frame);

    denso::level::LevelZone zone;
    zone.zone_no = 1;
    zone.calibration.rect_x = 0.2;
    zone.calibration.rect_y = 0.1;
    zone.calibration.rect_w = 0.6;
    zone.calibration.rect_h = 0.8;
    zone.calibration.y_100 = 0.2;
    zone.calibration.y_0 = 0.8;
    zone.calibration.conf = 0.5;
    zone.calibration.hold_ms = 2000;

    CapturingEngine engine;
    BallLevelProcessor proc(0, 0.0, 0.0, &engine, /*class_id=*/0, {zone},
                            /*camera_id=*/1, /*zone_sink=*/nullptr, {}, {});

    const QImage shown = pump(proc, engine, frame);
    REQUIRE(engine.calls() >= 1);

    CHECK(diff_count(engine.last(), original) == 0);
    CHECK(shown.size() == frame.size());
}
