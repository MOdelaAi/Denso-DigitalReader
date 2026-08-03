// The zone values must be part of the PICTURE, not a widget layered over it.
//
// These drive the REAL frame processors and assert on the QImage they hand to
// the grid — so they prove the annotation happened on the cv::Mat, before the
// conversion, and that the tile receives an already-annotated frame.
//
// Synthetic frames only; no camera, no engine, no backend.
#include <catch2/catch_test_macros.hpp>

#include "brazing/zone_reporter.h"
#include "brazing/zone_runtime.h"
#include "camera/camera_stream.h"   // Status
#include "camera/frame_processor.h"
#include "camera/zone_overlay.h"
#include "ui/camera/grid/camera_tile.h"

#include <QFile>
#include <QIODevice>
#include <QImage>
#include <QString>
#include <QStringList>

#include <optional>
#include <vector>
#include "zone_value_compat.h"

using denso::ui::DetectionProcessor;
using denso::ui::OrientationProcessor;
using denso::ui::ZoneDisplayState;
using denso::ui::ZoneReporter;
using denso::ui::ZoneRuntimeEntry;
using denso::ui::ZoneViewFn;

namespace {

ZoneRuntimeEntry entry(int64_t cam, int zone_no, ZoneDisplayState state,
                       std::optional<denso::ui::ZoneValue> value = std::nullopt) {
    ZoneRuntimeEntry e;
    e.camera_id = cam;
    e.zone_no = zone_no;
    e.state = state;
    e.value = value;
    return e;
}

QImage grey_frame(int w = 640, int h = 480) {
    QImage img(w, h, QImage::Format_RGB888);
    img.fill(qRgb(128, 128, 128));
    return img;
}

int differing(const QImage& a, const QImage& b) {
    REQUIRE(a.size() == b.size());
    int n = 0;
    for (int y = 0; y < a.height(); ++y) {
        for (int x = 0; x < a.width(); ++x) {
            if (a.pixel(x, y) != b.pixel(x, y)) ++n;
        }
    }
    return n;
}

ZoneViewFn fixed_view(std::vector<ZoneRuntimeEntry> rows) {
    return [rows = std::move(rows)] { return rows; };
}

QString read_source(const char* rel) {
    QFile f(QStringLiteral(DENSO_SOURCE_DIR) + QString::fromUtf8(rel));
    REQUIRE(f.open(QIODevice::ReadOnly));
    return QString::fromUtf8(f.readAll());
}

} // namespace

// ── The delivered frame carries the values ───────────────────────────────────

TEST_CASE("the orientation path delivers an annotated frame", "[zone_pipeline]") {
    OrientationProcessor plain(0, 0.0, 0.0);
    OrientationProcessor annotated(
        0, 0.0, 0.0,
        fixed_view({entry(1, 1, ZoneDisplayState::Healthy, denso::ui::ZoneValue{128}),
                    entry(1, 2, ZoneDisplayState::HoldingLastValid, denso::ui::ZoneValue{95})}));

    const QImage src = grey_frame();
    const QImage bare = plain.process(src);
    const QImage with = annotated.process(src);

    REQUIRE(bare.size() == with.size());
    CHECK(differing(bare, with) > 0);   // the picture itself changed
}

// A camera with zone-numbered ROIs but NO detection model must still show its
// zones — the annotation lives on the display path, not behind inference.
TEST_CASE("a model-less camera still gets its zone annotation", "[zone_pipeline]") {
    OrientationProcessor p(0, 0.0, 0.0,
                           fixed_view({entry(1, 1, ZoneDisplayState::Acquiring)}));
    const QImage bare = OrientationProcessor(0, 0.0, 0.0).process(grey_frame());
    CHECK(differing(bare, p.process(grey_frame())) > 0);
}

TEST_CASE("no zones means the frame is passed through untouched",
          "[zone_pipeline]") {
    OrientationProcessor none(0, 0.0, 0.0, fixed_view({}));
    const QImage src = grey_frame();
    const QImage bare = OrientationProcessor(0, 0.0, 0.0).process(src);
    CHECK(differing(bare, none.process(src)) == 0);
}

// ── Per-camera correctness through the real processors ───────────────────────

TEST_CASE("two cameras sharing zone 1 deliver different frames",
          "[zone_pipeline]") {
    OrientationProcessor cam1(
        0, 0.0, 0.0, fixed_view({entry(1, 1, ZoneDisplayState::Healthy, denso::ui::ZoneValue{128})}));
    OrientationProcessor cam2(
        0, 0.0, 0.0, fixed_view({entry(2, 1, ZoneDisplayState::HoldingLastValid, denso::ui::ZoneValue{64})}));

    const QImage src = grey_frame();
    CHECK(differing(cam1.process(src), cam2.process(src)) > 0);
}

// ── Backend independence, through a real reporter ────────────────────────────

namespace {

// Drive a real ZoneReporter to an accepted value and annotate a frame from its
// projection, exactly as the grid's ZoneViewFn does.
QImage frame_from_reporter(
    std::function<void(const std::map<int, denso::ui::ZoneValue>&, uint64_t)> on_snapshot,
    int* calls = nullptr) {
    ZoneReporter rep(std::move(on_snapshot), 3);
    rep.set_configured_zones(1, {1});
    for (int i = 0; i < 3; ++i) {
        rep.on_zones(1, {denso::ui::ZoneReading{1, {128}, 0.9f,
                                                denso::ui::ReadingKind::Complete}});
    }
    (void)calls;
    OrientationProcessor p(0, 0.0, 0.0, [&rep] {
        std::vector<ZoneRuntimeEntry> mine;
        for (const ZoneRuntimeEntry& e : rep.runtime_view()) {
            if (e.camera_id == 1) mine.push_back(e);
        }
        return mine;
    });
    return p.process(grey_frame());
}

} // namespace

TEST_CASE("backend disabled still annotates the frame", "[zone_pipeline]") {
    const QImage bare = OrientationProcessor(0, 0.0, 0.0).process(grey_frame());
    const QImage no_backend = frame_from_reporter({});        // nothing wired
    CHECK(differing(bare, no_backend) > 0);
}

TEST_CASE("backend offline does not remove the annotation", "[zone_pipeline]") {
    int calls = 0;
    // A downed server: the transport takes the snapshot and never acknowledges.
    const QImage failing = frame_from_reporter(
        [&calls](const std::map<int, denso::ui::ZoneValue>&, uint64_t) { ++calls; });
    const QImage bare = OrientationProcessor(0, 0.0, 0.0).process(grey_frame());

    CHECK(calls > 0);                        // delivery was attempted
    CHECK(differing(bare, failing) > 0);     // and the annotation is still there
}

TEST_CASE("backend enabled and disabled annotate identically", "[zone_pipeline]") {
    const QImage off = frame_from_reporter({});
    const QImage on = frame_from_reporter([](const std::map<int, denso::ui::ZoneValue>&, uint64_t) {});
    CHECK(differing(off, on) == 0);
}

// ── Ordering and ownership, asserted on the production source ────────────────

// The annotation must sit AFTER the boxes and BEFORE the conversion. Pixel tests
// cannot see an ordering that has no observable difference here, so this pins
// the sequence in the one function that owns it.
TEST_CASE("zones are drawn after the boxes and before the QImage conversion",
          "[zone_pipeline]") {
    const QString src = read_source("/src/app/camera/frame_processor.cpp");
    const int fn = src.indexOf(
        QStringLiteral("QImage DetectionProcessor::process(const QImage& frame)"));
    REQUIRE(fn > 0);
    const QString body = src.mid(fn, src.indexOf(QStringLiteral("\n}"), fn) - fn);

    const int boxes_at = body.indexOf(QStringLiteral("draw_detections(bgr, boxes);"));
    const int zones_at = body.indexOf(QStringLiteral("draw_zone_runtime_overlay(bgr,"));
    const int convert_at = body.indexOf(QStringLiteral("return mat_to_qimage(bgr);"));
    REQUIRE(boxes_at > 0);
    REQUIRE(zones_at > 0);
    REQUIRE(convert_at > 0);
    CHECK(boxes_at < zones_at);
    CHECK(zones_at < convert_at);

    // And the worker's private copy is taken BEFORE any drawing, so no annotation
    // can reach inference.
    const int submit_at = body.indexOf(QStringLiteral("bgr.copyTo(pending_);"));
    REQUIRE(submit_at > 0);
    CHECK(submit_at < boxes_at);
    CHECK(submit_at < zones_at);
}

// ── Exactly one visible annotation ───────────────────────────────────────────

// MUTATION: "leave the old QPainter zone panel in place" must die. Two overlays
// drawing the same values is precisely the duplication this change removes.
TEST_CASE("CameraTile no longer paints a zone panel of its own",
          "[zone_pipeline]") {
    const QStringList paths{QStringLiteral("/src/app/ui/camera/grid/camera_tile.h"),
                            QStringLiteral("/src/app/ui/camera/grid/camera_tile.cpp")};
    const QStringList forbidden{QStringLiteral("draw_zone_panel"),
                                QStringLiteral("set_zone_runtime_view"),
                                QStringLiteral("clear_zone_runtime_view"),
                                QStringLiteral("ZoneRuntimeEntry"),
                                QStringLiteral("zone_row_text")};
    for (const QString& rel : paths) {
        const QString text = read_source(rel.toUtf8().constData());
        for (const QString& token : forbidden) {
            INFO("token '" << token.toStdString() << "' still in "
                           << rel.toStdString());
            CHECK_FALSE(text.contains(token));
        }
    }
}

// The tile must not be able to render zone values at all: with no zone API left,
// a tile given a frame draws exactly the frame plus its own chrome.
TEST_CASE("a tile renders the frame it is handed, zone text included",
          "[zone_pipeline]") {
    OrientationProcessor annotated(
        0, 0.0, 0.0, fixed_view({entry(1, 1, ZoneDisplayState::Healthy, denso::ui::ZoneValue{128})}));
    const QImage annotated_frame = annotated.process(grey_frame(1280, 720));
    const QImage plain_frame = OrientationProcessor(0, 0.0, 0.0).process(grey_frame(1280, 720));

    denso::ui::CameraTile a(QStringLiteral("cam"));
    a.resize(640, 360);
    a.set_frame(annotated_frame);

    denso::ui::CameraTile b(QStringLiteral("cam"));
    b.resize(640, 360);
    b.set_frame(plain_frame);

    // The difference on screen comes ENTIRELY from the frame contents now.
    CHECK(differing(a.grab().toImage(), b.grab().toImage()) > 0);
}

// A value burned into the picture cannot update itself. If the tile kept the last
// frame after the stream dropped, the operator would keep reading a number that
// nothing maintains — a dead reading that still looks live. The Qt panel this
// replaced updated independently and could not go stale, so this is the hazard
// the move into the frame introduces, and it must be closed.
TEST_CASE("an offline camera drops its burned-in zone values", "[zone_pipeline]") {
    OrientationProcessor p(
        0, 0.0, 0.0, fixed_view({entry(1, 1, ZoneDisplayState::Healthy, denso::ui::ZoneValue{128})}));

    denso::ui::CameraTile tile(QStringLiteral("cam"));
    tile.resize(640, 360);
    tile.set_status(static_cast<int>(denso::ui::CameraStream::Status::Live));
    tile.set_frame(p.process(grey_frame(1280, 720)));
    const QImage live = tile.grab().toImage();

    tile.set_status(static_cast<int>(denso::ui::CameraStream::Status::Offline));
    const QImage offline = tile.grab().toImage();
    CHECK(differing(live, offline) > 0);          // the stale picture is gone

    // ...and it now matches a tile that never received a frame at all, so no
    // remnant of the annotated image survives.
    denso::ui::CameraTile fresh(QStringLiteral("cam"));
    fresh.resize(640, 360);
    fresh.set_status(static_cast<int>(denso::ui::CameraStream::Status::Offline));
    CHECK(differing(fresh.grab().toImage(), offline) == 0);
}
