// Zone values BURNED INTO THE FRAME — the OpenCV annotation path, drawn with the
// same primitives as the detection boxes (cv::rectangle / cv::putText) so the
// displayed image already carries the values before it ever becomes a QImage.
//
// Every assertion inspects the resulting cv::Mat. Pure unit: no Qt, no camera,
// no reporter — synthetic frames only.
#include <catch2/catch_test_macros.hpp>

#include "brazing/zone_runtime.h"
#include "camera/zone_overlay.h"

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

#include <optional>
#include <string>
#include <vector>

using denso::ui::draw_zone_runtime_overlay;
using denso::ui::ZoneDisplayState;
using denso::ui::zone_overlay_scale;
using denso::ui::zone_row_text;
using denso::ui::ZoneRuntimeEntry;

namespace {

ZoneRuntimeEntry entry(int64_t cam, int zone_no, ZoneDisplayState state,
                       std::optional<int> value = std::nullopt) {
    ZoneRuntimeEntry e;
    e.camera_id = cam;
    e.zone_no = zone_no;
    e.state = state;
    e.value = value;
    return e;
}

cv::Mat black(int w = 640, int h = 480) {
    return cv::Mat::zeros(h, w, CV_8UC3);
}

int painted(const cv::Mat& m) { return cv::countNonZero(m.reshape(1)); }

// Bounding box of everything that differs from the untouched frame.
cv::Rect changed_bbox(const cv::Mat& before, const cv::Mat& after) {
    REQUIRE(before.size() == after.size());
    cv::Mat diff;
    cv::absdiff(before, after, diff);
    cv::cvtColor(diff, diff, cv::COLOR_BGR2GRAY);
    return cv::boundingRect(diff);
}

bool has_digit_after_label(const std::string& row) {
    // Skip "Z<n>" then look for any digit in the remainder.
    size_t i = 1;
    while (i < row.size() && std::isdigit(static_cast<unsigned char>(row[i]))) ++i;
    for (; i < row.size(); ++i) {
        if (std::isdigit(static_cast<unsigned char>(row[i]))) return true;
    }
    return false;
}

} // namespace

// ── Row content: which states may show a number ──────────────────────────────

TEST_CASE("Healthy row carries its accepted value and OK", "[zone_draw]") {
    const std::string row = zone_row_text(entry(1, 1, ZoneDisplayState::Healthy, 128));
    CHECK(row.find("128") != std::string::npos);
    CHECK(row.find("OK") != std::string::npos);
}

TEST_CASE("Hold row carries the last valid value and HOLD", "[zone_draw]") {
    const std::string row =
        zone_row_text(entry(1, 2, ZoneDisplayState::HoldingLastValid, 95));
    CHECK(row.find("95") != std::string::npos);
    CHECK(row.find("HOLD") != std::string::npos);
    CHECK(row.find("INHIBITED") == std::string::npos);
}

TEST_CASE("states with no trusted reading show dashes and no number",
          "[zone_draw]") {
    struct Case { ZoneDisplayState state; const char* label; };
    for (const Case c : {Case{ZoneDisplayState::Acquiring, "ACQUIRING"},
                         Case{ZoneDisplayState::Inhibited, "INHIBITED"},
                         Case{ZoneDisplayState::Paused,    "PAUSED"},
                         Case{ZoneDisplayState::Conflict,  "CONFLICT"}}) {
        const std::string row = zone_row_text(entry(1, 4, c.state));
        INFO("row: " << row);
        CHECK(row.find("--") != std::string::npos);
        CHECK(row.find(c.label) != std::string::npos);
        CHECK_FALSE(has_digit_after_label(row));
    }
}

TEST_CASE("no backend delivery vocabulary can reach a row", "[zone_draw]") {
    for (const ZoneDisplayState s :
         {ZoneDisplayState::Healthy, ZoneDisplayState::HoldingLastValid,
          ZoneDisplayState::Acquiring, ZoneDisplayState::Inhibited,
          ZoneDisplayState::Paused, ZoneDisplayState::Conflict}) {
        const std::string row = zone_row_text(entry(1, 1, s, 7));
        for (const char* bad : {"SENT", "PENDING", "OFFLINE", "FAILED", "REPORTED"}) {
            INFO(row << " contained " << bad);
            CHECK(row.find(bad) == std::string::npos);
        }
    }
}

// ── Pixels: the values really land on the frame ──────────────────────────────

TEST_CASE("a Healthy value is drawn into the frame", "[zone_draw]") {
    cv::Mat a = black(), b = black();
    draw_zone_runtime_overlay(a, {entry(1, 1, ZoneDisplayState::Healthy, 128)});
    draw_zone_runtime_overlay(b, {entry(1, 1, ZoneDisplayState::Healthy, 999)});

    CHECK(painted(a) > 0);                       // something was drawn
    CHECK(cv::norm(a, b, cv::NORM_L1) > 0.0);    // and the NUMBER is part of it
}

TEST_CASE("an empty projection leaves the frame byte-identical", "[zone_draw]") {
    // A MID-GREY frame, not black: the panel backing is black, so a black frame
    // would hide a panel that was wrongly drawn and the assertion would pass for
    // the wrong reason.
    cv::Mat frame(480, 640, CV_8UC3, cv::Scalar(128, 128, 128));
    cv::rectangle(frame, cv::Rect(10, 10, 50, 50), cv::Scalar(0, 215, 255), 2);
    const cv::Mat before = frame.clone();

    draw_zone_runtime_overlay(frame, {});

    CHECK(cv::norm(before, frame, cv::NORM_L1) == 0.0);
}

TEST_CASE("clearing the zones leaves the next frame unannotated", "[zone_draw]") {
    // Each frame is drawn from scratch, so "clearing" is simply an empty
    // projection on the following frame — no state persists in the drawing code.
    cv::Mat f1 = black();
    draw_zone_runtime_overlay(f1, {entry(1, 1, ZoneDisplayState::Healthy, 128)});
    REQUIRE(painted(f1) > 0);

    cv::Mat f2(480, 640, CV_8UC3, cv::Scalar(128, 128, 128));
    const cv::Mat f2_before = f2.clone();
    draw_zone_runtime_overlay(f2, {});
    CHECK(cv::norm(f2_before, f2, cv::NORM_L1) == 0.0);
}

TEST_CASE("the annotation stays inside a 640x480 frame", "[zone_draw]") {
    cv::Mat frame = black(640, 480);
    const cv::Mat before = frame.clone();
    draw_zone_runtime_overlay(
        frame, {entry(1, 1, ZoneDisplayState::Healthy, 128),
                entry(1, 2, ZoneDisplayState::HoldingLastValid, 95),
                entry(1, 3, ZoneDisplayState::Acquiring),
                entry(1, 4, ZoneDisplayState::Inhibited)});

    const cv::Rect b = changed_bbox(before, frame);
    REQUIRE(b.area() > 0);
    CHECK(b.x >= 0);
    CHECK(b.y >= 0);
    CHECK(b.x + b.width <= 640);
    CHECK(b.y + b.height <= 480);
    // A verification aid, not a blanket over the display being inspected.
    CHECK(b.area() < 640 * 480 * 40 / 100);
}

TEST_CASE("the annotation is contained at many frame resolutions", "[zone_draw]") {
    struct Size { int w, h; };
    for (const Size s : {Size{320, 240}, Size{640, 480}, Size{1280, 720},
                         Size{1920, 1080}}) {
        cv::Mat frame = black(s.w, s.h);
        const cv::Mat before = frame.clone();
        draw_zone_runtime_overlay(
            frame, {entry(1, 1, ZoneDisplayState::Healthy, 128),
                    entry(1, 4, ZoneDisplayState::Conflict)});
        const cv::Rect b = changed_bbox(before, frame);
        INFO("frame " << s.w << "x" << s.h);
        REQUIRE(b.area() > 0);
        CHECK(b.x + b.width <= s.w);
        CHECK(b.y + b.height <= s.h);
        CHECK(b.area() < s.w * s.h * 40 / 100);
    }
}

TEST_CASE("the smallest supported grid frame still gets a readable annotation",
          "[zone_draw]") {
    // Text scale never collapses below the legibility floor, and the panel is
    // still actually drawn.
    CHECK(zone_overlay_scale(160) >= denso::ui::kZoneOverlayMinScale);
    CHECK(zone_overlay_scale(1080) <= denso::ui::kZoneOverlayMaxScale);
    // STRICT: a scale pinned to the floor would satisfy >=, which is exactly the
    // "stopped scaling with the frame" regression.
    CHECK(zone_overlay_scale(1080) > zone_overlay_scale(480));
    CHECK(zone_overlay_scale(720) > zone_overlay_scale(360));

    cv::Mat frame = black(240, 160);
    const cv::Mat before = frame.clone();
    draw_zone_runtime_overlay(frame, {entry(1, 1, ZoneDisplayState::Healthy, 128)});
    const cv::Rect b = changed_bbox(before, frame);
    CHECK(b.area() > 0);
    CHECK(b.x + b.width <= 240);
    CHECK(b.y + b.height <= 160);
}

TEST_CASE("detection boxes and zone values coexist on one frame", "[zone_draw]") {
    cv::Mat frame = black(640, 480);
    // The box the detection path already draws, in its own colour.
    const cv::Scalar box_bgr(0, 215, 255);
    cv::rectangle(frame, cv::Rect(200, 100, 120, 80), box_bgr, 2);
    const cv::Mat with_box = frame.clone();

    draw_zone_runtime_overlay(frame, {entry(1, 1, ZoneDisplayState::Healthy, 128)});

    // The zone panel added pixels...
    CHECK(cv::norm(with_box, frame, cv::NORM_L1) > 0.0);
    // ...and the box is still there: its colour survives somewhere on the frame.
    bool box_survives = false;
    for (int y = 100; y <= 180 && !box_survives; ++y) {
        for (int x = 200; x <= 320 && !box_survives; ++x) {
            const cv::Vec3b p = frame.at<cv::Vec3b>(y, x);
            box_survives = (p[0] == 0 && p[1] == 215 && p[2] == 255);
        }
    }
    CHECK(box_survives);
}

// ── Per-camera correctness ───────────────────────────────────────────────────

TEST_CASE("two cameras sharing zone number 1 get their OWN values", "[zone_draw]") {
    // The caller hands each frame only its own camera's rows; the same zone
    // NUMBER must therefore render two different readings.
    cv::Mat cam1 = black(), cam2 = black();
    draw_zone_runtime_overlay(cam1, {entry(1, 1, ZoneDisplayState::Healthy, 128)});
    draw_zone_runtime_overlay(cam2, {entry(2, 1, ZoneDisplayState::HoldingLastValid, 64)});

    CHECK(cv::norm(cam1, cam2, cv::NORM_L1) > 0.0);
    CHECK(zone_row_text(entry(1, 1, ZoneDisplayState::Healthy, 128)).find("128") !=
          std::string::npos);
    CHECK(zone_row_text(entry(2, 1, ZoneDisplayState::HoldingLastValid, 64)).find("64") !=
          std::string::npos);
}

// ── Frame ownership ──────────────────────────────────────────────────────────

TEST_CASE("drawing touches only the frame it is given", "[zone_draw]") {
    cv::Mat source = black();
    cv::rectangle(source, cv::Rect(5, 5, 30, 30), cv::Scalar(10, 20, 30), cv::FILLED);
    const cv::Mat pristine = source.clone();

    // The display path hands the annotator a frame that is already its own; a
    // caller that must protect a shared source clones first. Prove the clone is
    // genuinely independent — drawing on it must not reach `source`.
    cv::Mat display = source.clone();
    draw_zone_runtime_overlay(display, {entry(1, 1, ZoneDisplayState::Healthy, 128)});

    CHECK(cv::norm(pristine, source, cv::NORM_L1) == 0.0);   // source untouched
    CHECK(cv::norm(display, source, cv::NORM_L1) > 0.0);     // display annotated
}

TEST_CASE("an empty frame is handled safely", "[zone_draw]") {
    cv::Mat empty;
    // Must not throw or write through a null buffer.
    draw_zone_runtime_overlay(empty, {entry(1, 1, ZoneDisplayState::Healthy, 128)});
    CHECK(empty.empty());
}

// MUTATION: "drop the clamp into the frame" must die. A tiny frame with four
// long state words produces a panel wider than the image; without the clamp the
// ROI is invalid and OpenCV throws.
TEST_CASE("a tiny frame with long labels still contains the panel", "[zone_draw]") {
    cv::Mat frame = black(240, 160);
    const cv::Mat before = frame.clone();
    // TWELVE zones — the domain maximum — on the smallest frame. The panel is
    // taller than the image, so the clamp is what stops cv::Mat::operator()
    // being handed a rect that leaves the frame.
    std::vector<ZoneRuntimeEntry> many;
    for (int z = 1; z <= 12; ++z) {
        many.push_back(entry(1, z, ZoneDisplayState::Acquiring));
    }
    draw_zone_runtime_overlay(frame, many);
    const cv::Rect b = changed_bbox(before, frame);
    REQUIRE(b.area() > 0);
    CHECK(b.x >= 0);
    CHECK(b.y >= 0);
    CHECK(b.x + b.width <= 240);
    CHECK(b.y + b.height <= 160);
}

// MUTATION: "colour HOLD like a stopped zone" must die. A hold still has a
// trustworthy number; it must not read as a stop.
//
// Compared by CHANNEL STATISTICS, not by probing for an exact BGR triple:
// cv::putText with LINE_AA antialiases every glyph, so the pen colour may never
// appear unblended and an exact probe fails for the wrong reason.
TEST_CASE("HOLD is drawn in its own colour", "[zone_draw]") {
    const auto mean_painted = [](const cv::Mat& m) {
        cv::Mat grey;
        cv::cvtColor(m, grey, cv::COLOR_BGR2GRAY);
        cv::Mat mask = grey > 40;               // ignore the near-black backing
        return cv::mean(m, mask);               // BGR means of the drawn pixels
    };

    cv::Mat hold = black();
    draw_zone_runtime_overlay(hold, {entry(1, 2, ZoneDisplayState::HoldingLastValid, 95)});
    cv::Mat stopped = black();
    draw_zone_runtime_overlay(stopped, {entry(1, 2, ZoneDisplayState::Inhibited)});

    const cv::Scalar h = mean_painted(hold);
    const cv::Scalar s = mean_painted(stopped);
    INFO("hold BGR " << h[0] << "," << h[1] << "," << h[2]
         << "  stopped BGR " << s[0] << "," << s[1] << "," << s[2]);
    // kHold is amber (21,204,250): much greener and much less blue than
    // kStopped's red (113,113,248).
    CHECK(h[1] > s[1] + 30.0);   // green
    CHECK(h[0] < s[0] - 30.0);   // blue
}
