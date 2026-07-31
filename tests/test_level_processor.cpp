// Phase B — the Ball Leveler RUNTIME: the level overlay burned into the frame and
// the BallLevelProcessor that produces it.
//
// Everything here is synthetic: a stub InferenceEngine and generated frames. No
// camera is contacted, no engine file is deserialized, no database is opened.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "camera/level_overlay.h"
#include "camera/level_processor.h"
#include "detection/inference_engine.h"
#include "level/calibration.h"
#include "level/measure.h"
#include "level/runtime.h"

#include <QImage>

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

#include <atomic>
#include <cmath>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

using denso::level::BallBox;
using denso::level::LevelCalibration;
using denso::level::LevelRuntimeEntry;
using denso::level::LevelState;
using denso::ui::BallLevelProcessor;
using denso::ui::LevelZoneDraw;
using denso::ui::ZoneDisplayState;
using denso::ui::level_zone_text;
using denso::ui::Detection;
using denso::ui::draw_level_overlay;
using denso::ui::InferenceEngine;
using denso::ui::LevelStateProcessor;

namespace {

constexpr int kW = 640;
constexpr int kH = 480;

// The calibration every case measures against: rectangle x[0.3,0.7) y[0.1,0.9),
// 100% line at y=0.2, 0% line at y=0.8, confidence floor 0.5.
LevelCalibration calib() {
    LevelCalibration c;
    c.rect_x = 0.3;
    c.rect_y = 0.1;
    c.rect_w = 0.4;
    c.rect_h = 0.8;
    c.y_100 = 0.2;
    c.y_0 = 0.8;
    c.conf = 0.5;
    c.hold_ms = 2000;
    return c;
}

// A detection in PIXELS, from normalized corners — the processor divides by the
// frame size, so the test states what the backend would actually return.
Detection det(double nx1, double ny1, double nx2, double ny2, float conf,
              int class_id = 0) {
    Detection d;
    // lround, NOT a truncating cast: (0.6 - 0.4) * 480 is 95.999999999999986 in
    // binary64, which truncates to 95 and silently shifts the box centre half a
    // pixel — enough to move the asserted percentage by 0.17.
    d.box = cv::Rect(static_cast<int>(std::lround(nx1 * kW)),
                     static_cast<int>(std::lround(ny1 * kH)),
                     static_cast<int>(std::lround((nx2 - nx1) * kW)),
                     static_cast<int>(std::lround((ny2 - ny1) * kH)));
    d.class_id = class_id;
    d.conf = conf;
    return d;
}

/// Deterministic stand-in for the backend. Returns a fixed detection list, or
/// throws, so the worker's firewall and escalation are drivable without a GPU.
class StubEngine : public InferenceEngine {
public:
    explicit StubEngine(std::vector<Detection> out) : out_(std::move(out)) {}

    void set_throwing(bool t) {
        std::lock_guard<std::mutex> lk(m_);
        throwing_ = t;
    }

    std::vector<Detection> infer(const cv::Mat&) override {
        std::lock_guard<std::mutex> lk(m_);
        ++calls_;
        if (throwing_) throw std::runtime_error("stub inference failure");
        return out_;
    }
    const std::vector<std::string>& class_names() const override { return names_; }

    int calls() const {
        std::lock_guard<std::mutex> lk(m_);
        return calls_;
    }

private:
    mutable std::mutex m_;
    std::vector<Detection> out_;
    std::vector<std::string> names_{"Small"};
    bool throwing_ = false;
    int calls_ = 0;
};

/// One calibration as the single-zone set the multi-zone processor takes. These
/// cases are about the measurement CORE and its interruption rules, not about
/// zone fan-out, so they state one zone once here.
std::vector<denso::level::LevelZone> one_zone(const denso::level::LevelCalibration& c,
                                              int zone_no = 1) {
    return {denso::level::LevelZone{zone_no, c}};
}

/// The camera picture with zone 1's percentage folded in — the single-zone shape
/// these cases were written against, expressed over the multi-zone API.
///
/// Healthy is DOWNGRADED to Acquiring when the camera is measuring but this zone
/// selected no ball: at camera level "measuring" is true, and it is the ZONE that
/// has no value. Collapsing that into the camera state is exactly what the
/// multi-zone design stopped doing, so the shim states it rather than hiding it.
denso::level::LevelRuntimeEntry snap1(const BallLevelProcessor& p) {
    denso::level::LevelRuntimeEntry e = p.camera_snapshot();
    e.percent.reset();
    if (e.state == denso::level::LevelState::Healthy) {
        const auto zones = p.snapshot();
        if (!zones.empty() && zones.front().percent) {
            e.percent = zones.front().percent;
        } else {
            e.state = denso::level::LevelState::Acquiring;
        }
    }
    return e;
}

QImage grey_frame() {
    QImage img(kW, kH, QImage::Format_RGB888);
    img.fill(QColor(40, 40, 40));
    return img;
}

/// Drive the processor until `pred` holds on its snapshot, or give up. The worker
/// is asynchronous by design, so a test must pump frames rather than sleep once.
template <typename Pred>
bool pump_until(BallLevelProcessor& p, Pred pred, int max_frames = 200) {
    const QImage frame = grey_frame();
    for (int i = 0; i < max_frames; ++i) {
        p.process(frame);
        if (pred(snap1(p))) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return pred(snap1(p));
}

/// Holds the worker inside infer() until released, so a test can raise an
/// interruption while a frame is genuinely in flight, and can distinguish a
/// frame that was IN FLIGHT from one that was merely QUEUED.
class BlockingEngine : public InferenceEngine {
public:
    explicit BlockingEngine(std::vector<Detection> out) : out_(std::move(out)) {}
    std::vector<Detection> infer(const cv::Mat&) override {
        {
            std::unique_lock<std::mutex> lk(m_);
            ++entered_;
            cv_.notify_all();
            cv_.wait(lk, [this] { return released_; });
        }
        {
            std::lock_guard<std::mutex> lk(m_);
            ++returned_;
        }
        cv_.notify_all();
        return out_;
    }
    const std::vector<std::string>& class_names() const override { return names_; }
    void wait_until_entered(int n) {
        std::unique_lock<std::mutex> lk(m_);
        cv_.wait(lk, [this, n] { return entered_ >= n; });
    }
    void wait_until_returned(int n) {
        std::unique_lock<std::mutex> lk(m_);
        cv_.wait(lk, [this, n] { return returned_ >= n; });
    }
    void release() {
        {
            std::lock_guard<std::mutex> lk(m_);
            released_ = true;
        }
        cv_.notify_all();
    }
    int entered() const {
        std::lock_guard<std::mutex> lk(m_);
        return entered_;
    }

private:
    mutable std::mutex m_;
    std::condition_variable cv_;
    std::vector<Detection> out_;
    std::vector<std::string> names_{"Small"};
    int entered_ = 0;
    int returned_ = 0;
    bool released_ = false;
};

cv::Mat black(int w = kW, int h = kH) { return cv::Mat::zeros(h, w, CV_8UC3); }

long nonzero_pixels(const cv::Mat& m) {
    cv::Mat grey;
    cv::cvtColor(m, grey, cv::COLOR_BGR2GRAY);
    return cv::countNonZero(grey);
}

}  // namespace

// ── The runtime entry's central invariant ───────────────────────────────────

TEST_CASE("only a Healthy level entry can carry a percentage", "[level][runtime]") {
    CHECK(LevelRuntimeEntry::healthy(1, 42.0, 100).percent.has_value());

    // Every other constructor leaves it disengaged. This is what makes "never
    // show a stale value as a current live measurement" a property of the type
    // rather than a rule each draw site must remember.
    CHECK_FALSE(LevelRuntimeEntry::acquiring(1, 100).percent.has_value());
    CHECK_FALSE(LevelRuntimeEntry::unavailable(1, "camera_offline", 100).percent.has_value());
    CHECK_FALSE(LevelRuntimeEntry::unconfigured(1, 100).percent.has_value());
    CHECK_FALSE(LevelRuntimeEntry::calibration_invalid(1, 100).percent.has_value());

    CHECK(LevelRuntimeEntry::unavailable(1, "paused", 100).reason == "paused");
}

// ── Overlay text ────────────────────────────────────────────────────────────

TEST_CASE("level overlay renders the required captions", "[level][overlay]") {
    // The caption is now per ZONE and speaks the SHARED zone state vocabulary
    // (ui::ZoneDisplayState), because Ball zones render through the same
    // annotation boundary the digit zone panel uses.
    const auto row = [](ZoneDisplayState st, std::optional<double> pct) {
        LevelZoneDraw z;
        z.zone_no = 1;
        z.calib = calib();
        z.state = st;
        z.percent = pct;
        return z;
    };
    SECTION("Healthy shows the number, at one decimal") {
        // One decimal on the FRAME; the backend receives the quantized integer.
        // The two differ by design (amendment §10.3).
        CHECK(level_zone_text(row(ZoneDisplayState::Healthy, 67.4)) ==
              "ZONE 1   LEVEL 67.4%");
    }
    SECTION("no state but Healthy can show a number") {
        // The percentage is absent for every other state BY CONSTRUCTION - the
        // producer only engages it for Healthy - so these render their state
        // word rather than a fabricated reading.
        CHECK(level_zone_text(row(ZoneDisplayState::Acquiring, std::nullopt)) ==
              "ZONE 1   ACQUIRING");
        CHECK(level_zone_text(row(ZoneDisplayState::Inhibited, std::nullopt)) ==
              "ZONE 1   UNAVAILABLE");
        CHECK(level_zone_text(row(ZoneDisplayState::Paused, std::nullopt)) ==
              "ZONE 1   PAUSED");
        CHECK(level_zone_text(row(ZoneDisplayState::Conflict, std::nullopt)) ==
              "ZONE 1   CONFLICT");
    }
    SECTION("the zone number is part of the caption") {
        LevelZoneDraw z = row(ZoneDisplayState::Healthy, 12.0);
        z.zone_no = 4;
        CHECK(level_zone_text(z) == "ZONE 4   LEVEL 12.0%");
    }
}

TEST_CASE("level overlay draws every configured zone", "[level][overlay]") {
    LevelZoneDraw z1;
    z1.zone_no = 1;
    z1.calib = calib();
    z1.state = ZoneDisplayState::Healthy;
    z1.percent = 50.0;
    BallBox b;
    b.x1 = 0.4; b.y1 = 0.4; b.x2 = 0.6; b.y2 = 0.6; b.conf = 0.9;
    z1.ball = b;

    cv::Mat one = black();
    draw_level_overlay(one, {z1});

    // A SECOND zone, elsewhere on the frame, must add its own geometry rather
    // than replace the first - one zone's annotation never erases a sibling's.
    LevelZoneDraw z2 = z1;
    z2.zone_no = 2;
    z2.calib.rect_x = 0.05;
    z2.calib.rect_w = 0.2;
    z2.ball.reset();
    z2.percent.reset();
    z2.state = ZoneDisplayState::Acquiring;

    cv::Mat two = black();
    draw_level_overlay(two, {z1, z2});

    CHECK(nonzero_pixels(one) > 0);
    CHECK(nonzero_pixels(two) > nonzero_pixels(one));

    SECTION("a zone that is not measuring is still outlined") {
        // Its rectangle and reference lines remain, so the operator can see
        // WHERE a zone reporting nothing was configured.
        cv::Mat silent = black();
        draw_level_overlay(silent, {z2});
        CHECK(nonzero_pixels(silent) > 0);
    }

    SECTION("an empty zone set leaves the frame byte-identical") {
        cv::Mat untouched = black();
        const int before = nonzero_pixels(untouched);
        draw_level_overlay(untouched, {});
        CHECK(nonzero_pixels(untouched) == before);
    }

    SECTION("an empty Mat is a no-op rather than a crash") {
        cv::Mat empty;
        draw_level_overlay(empty, {z1});
        CHECK(empty.empty());
    }
}

// ── The processor ───────────────────────────────────────────────────────────

TEST_CASE("a ball inside the rectangle produces a level from its bbox CENTRE",
          "[level][processor]") {
    // Box spans y 0.4..0.6, so its centre is 0.5 — exactly halfway between the
    // 100% line (0.2) and the 0% line (0.8). Using the bbox TOP would give
    // 66.7%, so this single number distinguishes the two.
    StubEngine engine({det(0.4, 0.4, 0.6, 0.6, 0.9f)});
    BallLevelProcessor p(0, 0.0, 0.0, &engine, 0, one_zone(calib()), 7);

    REQUIRE(pump_until(p, [](const LevelRuntimeEntry& e) {
        return e.state == LevelState::Healthy;
    }));
    const LevelRuntimeEntry e = snap1(p);
    REQUIRE(e.percent.has_value());
    CHECK_THAT(*e.percent, Catch::Matchers::WithinAbs(50.0, 0.001));
    CHECK(e.camera_id == 7);
}

// The load-bearing economic claim of the multi-zone amendment (§10.1): a camera
// binds ONE model and runs ONE inference per frame, whatever its zone count. The
// one detection set is then evaluated independently per zone. A per-zone
// inference would quadruple GPU work on a four-tank camera for no new
// information, and on an Orin Nano running four cameras that is the difference
// between keeping frame rate and not.
TEST_CASE("four zones share one inference execution per frame",
          "[level][processor][multizone]") {
    // Four balls at four heights. Each zone's rectangle selects exactly one of
    // them, so a correct fan-out yields four DIFFERENT percentages from the one
    // detection set — which is what distinguishes real per-zone evaluation from
    // one measurement copied four times.
    // Each ball sits at a DIFFERENT height within its own zone, so the four
    // percentages must come out distinct. Equal heights would let a broken
    // fan-out that measures zone 1 four times pass unnoticed.
    StubEngine engine({det(0.33, 0.15, 0.39, 0.21, 0.9f),    // zone 1: ~92%
                       det(0.53, 0.33, 0.59, 0.39, 0.9f),    // zone 2: ~69%
                       det(0.33, 0.58, 0.39, 0.64, 0.9f),    // zone 3: ~38%
                       det(0.53, 0.76, 0.59, 0.82, 0.9f)});  // zone 4: ~15%

    // Four side-by-side/stacked rectangles, each containing exactly one ball.
    const auto zone_at = [](double x, double y) {
        LevelCalibration c;
        c.rect_x = x;
        c.rect_y = y;
        c.rect_w = 0.16;
        c.rect_h = 0.15;
        c.y_100 = y + 0.01;
        c.y_0 = y + 0.14;
        c.conf = 0.5;
        c.hold_ms = 2000;
        return c;
    };
    std::vector<denso::level::LevelZone> zones{
        {1, zone_at(0.28, 0.16)},
        {2, zone_at(0.48, 0.31)},
        {3, zone_at(0.28, 0.52)},
        {4, zone_at(0.48, 0.67)}};

    BallLevelProcessor p(0, 0.0, 0.0, &engine, 0, zones, 11);

    // Pump by hand rather than through pump_until, because the FRAME COUNT is
    // half the assertion: the claim is inference-per-frame, so the denominator
    // has to be counted, not assumed.
    const QImage frame = grey_frame();
    int frames_submitted = 0;
    const auto all_measured = [&p] {
        const std::vector<denso::ui::LevelZoneResult> s = p.snapshot();
        if (s.size() != 4) return false;
        for (const denso::ui::LevelZoneResult& z : s)
            if (!z.percent.has_value()) return false;
        return true;
    };
    for (int i = 0; i < 200 && !all_measured(); ++i) {
        p.process(frame);
        ++frames_submitted;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    REQUIRE(all_measured());

    const std::vector<denso::ui::LevelZoneResult> s = p.snapshot();
    REQUIRE(s.size() == 4);
    CHECK(s[0].zone_no == 1);
    CHECK(s[3].zone_no == 4);
    // Four distinct measurements, each from its OWN zone's ball and reference
    // lines — not one value fanned out.
    std::set<int> distinct;
    for (const denso::ui::LevelZoneResult& z : s) {
        REQUIRE(z.percent.has_value());
        distinct.insert(static_cast<int>(*z.percent));
    }
    CHECK(distinct.size() == 4);

    // Inference ran at most once per submitted frame. The worker keeps a single
    // latest-frame slot, so calls can only be FEWER than frames (coalescing),
    // never more — four zones must not multiply it. A per-zone inference would
    // put this at roughly 4x the frame count.
    CHECK(engine.calls() <= frames_submitted);
    CHECK(engine.calls() >= 1);
}

TEST_CASE("a detection outside the measurement rectangle is ignored",
          "[level][processor]") {
    // Centre at x = 0.85, outside the rectangle's x band [0.3, 0.7).
    StubEngine engine({det(0.8, 0.4, 0.9, 0.6, 0.99f)});
    BallLevelProcessor p(0, 0.0, 0.0, &engine, 0, one_zone(calib()), 1);

    // Let the worker actually run — otherwise "still Acquiring" would prove
    // nothing but that inference had not happened yet.
    REQUIRE(pump_until(p, [&engine](const LevelRuntimeEntry&) {
        return engine.calls() >= 3;
    }));
    CHECK(snap1(p).state == LevelState::Acquiring);
    CHECK_FALSE(snap1(p).percent.has_value());
}

TEST_CASE("the calibration's confidence threshold is enforced",
          "[level][processor]") {
    // Perfectly placed, but below the calibration's 0.5 floor.
    StubEngine engine({det(0.4, 0.4, 0.6, 0.6, 0.20f)});
    BallLevelProcessor p(0, 0.0, 0.0, &engine, 0, one_zone(calib()), 1);

    REQUIRE(pump_until(p, [&engine](const LevelRuntimeEntry&) {
        return engine.calls() >= 3;
    }));
    CHECK(snap1(p).state == LevelState::Acquiring);
    CHECK_FALSE(snap1(p).percent.has_value());
}

TEST_CASE("a detection of another class is discarded", "[level][processor]") {
    // Well-placed and confident, but class 3 while the binding names class 0.
    StubEngine engine({det(0.4, 0.4, 0.6, 0.6, 0.99f, /*class_id*/ 3)});
    BallLevelProcessor p(0, 0.0, 0.0, &engine, /*class_id*/ 0, one_zone(calib()), 1);

    REQUIRE(pump_until(p, [&engine](const LevelRuntimeEntry&) {
        return engine.calls() >= 3;
    }));
    CHECK(snap1(p).state == LevelState::Acquiring);
}

TEST_CASE("the highest-confidence valid ball wins", "[level][processor]") {
    // Two balls inside the rectangle: the lower-confidence one sits at the 100%
    // line, the higher-confidence one halfway. Picking by confidence gives 50%.
    StubEngine engine({det(0.4, 0.15, 0.6, 0.25, 0.60f),
                       det(0.4, 0.40, 0.6, 0.60, 0.95f)});
    BallLevelProcessor p(0, 0.0, 0.0, &engine, 0, one_zone(calib()), 1);

    REQUIRE(pump_until(p, [](const LevelRuntimeEntry& e) {
        return e.state == LevelState::Healthy;
    }));
    CHECK_THAT(*snap1(p).percent, Catch::Matchers::WithinAbs(50.0, 0.001));
}

TEST_CASE("an unavailable camera clears the live value", "[level][processor]") {
    StubEngine engine({det(0.4, 0.4, 0.6, 0.6, 0.9f)});
    BallLevelProcessor p(0, 0.0, 0.0, &engine, 0, one_zone(calib()), 1);

    REQUIRE(pump_until(p, [](const LevelRuntimeEntry& e) {
        return e.state == LevelState::Healthy;
    }));
    REQUIRE(snap1(p).percent.has_value());

    // The camera drops. The measurement is still fresh in the processor, which
    // is precisely the situation in which a stale number could leak out.
    p.set_unavailable(std::string(denso::level::kReasonCameraOffline));
    const LevelRuntimeEntry off = snap1(p);
    CHECK(off.state == LevelState::Unavailable);
    CHECK(off.reason == "camera_offline");
    CHECK_FALSE(off.percent.has_value());

    // ...and it recovers when the camera comes back.
    p.set_unavailable(std::nullopt);
    REQUIRE(pump_until(p, [](const LevelRuntimeEntry& e) {
        return e.state == LevelState::Healthy;
    }));
}

// ── The two causes are INDEPENDENT (Phase B review, blocking finding) ───────
//
// These were one `unavailable_` slot, and the collapse was a real defect: a
// camera coming back online wrote nullopt over a live inference_error, and
// because escalation was tested with `==` the worker could never re-raise it.
// The processor then republished its last good percentage as a live HEALTHY
// reading, for as long as the appliance ran. On a levelling appliance that is
// the worst possible failure: a confident, wrong number.

TEST_CASE("a camera flap cannot resurrect a dead model's last reading",
          "[level][processor]") {
    StubEngine engine({det(0.4, 0.4, 0.6, 0.6, 0.9f)});
    BallLevelProcessor p(0, 0.0, 0.0, &engine, 0, one_zone(calib()), 1);

    // 1. A genuine live reading.
    REQUIRE(pump_until(p, [](const LevelRuntimeEntry& e) {
        return e.state == LevelState::Healthy;
    }));
    REQUIRE(snap1(p).percent.has_value());

    // 2. Inference dies and stays dead, WELL past the escalation threshold — the
    //    `==` test that used to guard it fires only on the exact 10th failure.
    engine.set_throwing(true);
    REQUIRE(pump_until(p, [](const LevelRuntimeEntry& e) {
        return e.state == LevelState::Unavailable &&
               e.reason == denso::level::kReasonInferenceError;
    }, 400));

    // 3. The camera flaps: offline, then back. This is an ordinary event on this
    //    appliance — the whole reconnect loop in CameraStream exists for it.
    p.set_unavailable(std::string(denso::level::kReasonCameraOffline));
    CHECK(snap1(p).reason == "camera_offline");
    p.set_unavailable(std::nullopt);   // Connecting/Live clears the CAMERA cause

    // 4. The model is still dead, so the camera cause clearing must NOT hand the
    //    picture back. It must certainly never carry the old percentage.
    const LevelRuntimeEntry after = snap1(p);
    CHECK(after.state == LevelState::Unavailable);
    CHECK(after.reason == denso::level::kReasonInferenceError);
    CHECK_FALSE(after.percent.has_value());

    // ...and it stays that way under continued traffic: the inference cause is
    // cleared by a genuine success and by nothing else.
    REQUIRE_FALSE(pump_until(p, [](const LevelRuntimeEntry& e) {
        return e.state == LevelState::Healthy;
    }, 60));

    // 5. A real recovery does clear it — and the number that returns is measured
    //    AFTER the interruption, never the one from before it.
    engine.set_throwing(false);
    REQUIRE(pump_until(p, [](const LevelRuntimeEntry& e) {
        return e.state == LevelState::Healthy;
    }, 400));
    CHECK(snap1(p).percent.has_value());
}

TEST_CASE("an inference recovery does not clear a camera-offline cause",
          "[level][processor]") {
    // The mirror of the case above: neither cause may retire the other, so the
    // fix cannot have been a one-directional patch.
    StubEngine engine({det(0.4, 0.4, 0.6, 0.6, 0.9f)});
    engine.set_throwing(true);
    BallLevelProcessor p(0, 0.0, 0.0, &engine, 0, one_zone(calib()), 1);

    REQUIRE(pump_until(p, [](const LevelRuntimeEntry& e) {
        return e.state == LevelState::Unavailable &&
               e.reason == denso::level::kReasonInferenceError;
    }, 400));

    p.set_unavailable(std::string(denso::level::kReasonCameraOffline));
    engine.set_throwing(false);   // the model recovers while the camera is down

    // The camera is still offline, so the camera cause must survive the
    // inference recovery and keep the picture unavailable.
    REQUIRE_FALSE(pump_until(p, [](const LevelRuntimeEntry& e) {
        return e.state == LevelState::Healthy;
    }, 60));
    const LevelRuntimeEntry e = snap1(p);
    CHECK(e.state == LevelState::Unavailable);
    CHECK(e.reason == "camera_offline");
    CHECK_FALSE(e.percent.has_value());
}

TEST_CASE("a measurement does not survive an availability interruption",
          "[level][processor]") {
    // Cause separation alone would still allow a pre-interruption number to be
    // republished the instant the cause cleared. The measurement is dropped when
    // a cause ENGAGES, so the first picture after a recovery is Acquiring: what
    // the operator sees always comes from a frame taken after the interruption.
    StubEngine engine({det(0.4, 0.4, 0.6, 0.6, 0.9f)});
    BallLevelProcessor p(0, 0.0, 0.0, &engine, 0, one_zone(calib()), 1);

    REQUIRE(pump_until(p, [](const LevelRuntimeEntry& e) {
        return e.state == LevelState::Healthy;
    }));

    p.set_unavailable(std::string(denso::level::kReasonPaused));
    p.set_unavailable(std::nullopt);

    // Read the state WITHOUT pumping a frame: no new measurement can have landed.
    const LevelRuntimeEntry e = snap1(p);
    CHECK(e.state == LevelState::Acquiring);
    CHECK_FALSE(e.percent.has_value());
}

TEST_CASE("a result computed before an interruption is never published",
          "[level][processor]") {
    // Invalidating on ENGAGE closes the window from one side only: a frame
    // already inside infer() when the camera dropped completes afterwards and
    // would write itself in as the current measurement, becoming "live" the
    // moment the cause cleared. Each submission carries the availability epoch it
    // was taken in, and a result whose epoch has moved on is discarded.
    //
    // Exactly ONE frame is ever submitted, and the stub is held inside infer()
    // until the interruption has been raised and cleared. So the only result the
    // worker can possibly publish is the stale one — no legitimate later
    // measurement can be mistaken for it.
    BlockingEngine engine({det(0.4, 0.4, 0.6, 0.6, 0.9f)});
    BallLevelProcessor p(0, 0.0, 0.0, &engine, 0, one_zone(calib()), 1);

    // ONE frame, and the worker is genuinely inside infer() before we go on.
    p.process(grey_frame());
    engine.wait_until_entered(1);

    // The camera drops and returns while that frame is still being inferred.
    p.set_unavailable(std::string(denso::level::kReasonCameraOffline));
    p.set_unavailable(std::nullopt);

    // Let the stale frame finish, and give the worker room to reach its publish
    // block. Nothing else is ever submitted, so whatever it does there is about
    // this one pre-interruption result.
    engine.release();
    engine.wait_until_returned(1);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    const LevelRuntimeEntry after = snap1(p);
    CHECK(after.state == LevelState::Acquiring);
    CHECK_FALSE(after.percent.has_value());
    CHECK(engine.entered() == 1);   // no second frame could have produced this
}

TEST_CASE("a frame QUEUED before an interruption is never published",
          "[level][processor]") {
    // The sibling of the in-flight case, and the subtler half. Stamping the epoch
    // when the worker DEQUEUES a frame would re-date a frame that had been
    // waiting in the slot since before the interruption, and publish that
    // pre-interruption picture as a post-recovery measurement. The stamp
    // therefore travels WITH the frame, applied at submission.
    BlockingEngine engine({det(0.4, 0.4, 0.6, 0.6, 0.9f)});
    BallLevelProcessor p(0, 0.0, 0.0, &engine, 0, one_zone(calib()), 1);

    // Frame 1 goes in and the worker blocks inside infer() on it.
    p.process(grey_frame());
    engine.wait_until_entered(1);

    // Frame 2 is submitted while the worker is busy, so it SITS in the slot,
    // stamped with the epoch current at submission.
    p.process(grey_frame());

    // Only now does the camera drop and return.
    p.set_unavailable(std::string(denso::level::kReasonCameraOffline));
    p.set_unavailable(std::nullopt);

    // Let both frames complete. Neither belongs to the world after the
    // interruption, so neither may become the live reading — and no third frame
    // is ever submitted, so nothing else can account for the state.
    engine.release();
    engine.wait_until_returned(2);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    const LevelRuntimeEntry after = snap1(p);
    CHECK(after.state == LevelState::Acquiring);
    CHECK_FALSE(after.percent.has_value());
    CHECK(engine.entered() == 2);
}

TEST_CASE("the overlay lands on the FINAL displayed image", "[level][processor]") {
    StubEngine engine({det(0.4, 0.4, 0.6, 0.6, 0.9f)});
    BallLevelProcessor p(0, 0.0, 0.0, &engine, 0, one_zone(calib()), 1);

    REQUIRE(pump_until(p, [](const LevelRuntimeEntry& e) {
        return e.state == LevelState::Healthy;
    }));

    const QImage input = grey_frame();
    const QImage shown = p.process(input);
    REQUIRE_FALSE(shown.isNull());
    REQUIRE(shown.size() == input.size());

    // The returned image — the one the tile paints — must differ from the input.
    // Drawing on a worker Mat that never reaches display would leave it identical.
    int changed = 0;
    for (int y = 0; y < shown.height(); ++y) {
        for (int x = 0; x < shown.width(); ++x) {
            if (shown.pixel(x, y) != input.pixel(x, y)) ++changed;
        }
    }
    CHECK(changed > 0);
}

TEST_CASE("a throwing engine does not kill the worker and escalates once",
          "[level][processor]") {
    StubEngine engine({det(0.4, 0.4, 0.6, 0.6, 0.9f)});
    engine.set_throwing(true);

    std::atomic<int> raised{0};
    std::atomic<int> cleared{0};
    {
        BallLevelProcessor p(
            0, 0.0, 0.0, &engine, 0, one_zone(calib()), 5, /*zone_sink=*/nullptr,
            [&raised, &cleared](int64_t, bool failed) {
                if (failed) ++raised; else ++cleared;
            });

        // Survives the throws (a std::terminate would take the test with it) and
        // publishes NOTHING — a failed frame must not move the value.
        REQUIRE(pump_until(p, [&raised](const LevelRuntimeEntry&) {
            return raised.load() >= 1;
        }, 400));
        // The escalation and the PUBLISHED picture are now the same fact. This
        // used to expect Acquiring: the failure travelled out through the
        // callback while the processor itself still reported "configured, no
        // measurement yet". That gap is what let a camera-status change wipe the
        // failure, so the state is now raised in the processor, under its own
        // mutex, by the worker that observed it.
        const LevelRuntimeEntry escalated = snap1(p);
        CHECK(escalated.state == LevelState::Unavailable);
        CHECK(escalated.reason == denso::level::kReasonInferenceError);
        CHECK_FALSE(escalated.percent.has_value());
        CHECK(raised.load() == 1);   // escalated exactly once, not per frame

        engine.set_throwing(false);
        REQUIRE(pump_until(p, [](const LevelRuntimeEntry& e) {
            return e.state == LevelState::Healthy;
        }, 400));
        CHECK(cleared.load() >= 1);  // recovery is announced
    }
    // Destructor returned, so the worker was stopped and joined.
    SUCCEED();
}

TEST_CASE("construction is counted only for a MEASURING processor",
          "[level][processor]") {
    const uint64_t before = BallLevelProcessor::constructed_count();

    // A state-only processor builds no pipeline, holds no engine, starts no
    // thread — and must not register as a measuring camera.
    {
        LevelStateProcessor s(0, 0.0, 0.0, 1, LevelState::Unconfigured);
        const QImage out = s.process(grey_frame());
        CHECK_FALSE(out.isNull());
    }
    CHECK(BallLevelProcessor::constructed_count() == before);

    {
        StubEngine engine({});
        BallLevelProcessor p(0, 0.0, 0.0, &engine, 0, one_zone(calib()), 1);
    }
    CHECK(BallLevelProcessor::constructed_count() == before + 1);
}

TEST_CASE("repeated construction and teardown joins cleanly", "[level][processor]") {
    // The destructor is the only thing standing between a mode switch and a
    // detached worker touching freed members, so exercise it in a loop.
    for (int i = 0; i < 8; ++i) {
        StubEngine engine({det(0.4, 0.4, 0.6, 0.6, 0.9f)});
        BallLevelProcessor p(0, 0.0, 0.0, &engine, 0, one_zone(calib()), i);
        p.process(grey_frame());
    }
    SUCCEED();
}
