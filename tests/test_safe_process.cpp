#include <catch2/catch_test_macros.hpp>

#include "ui/camera/grid/safe_process.h"
#include "ui/camera/grid/frame_processor.h"

#include <QImage>

#include <stdexcept>

using namespace denso::ui;

namespace {
// A processor that always throws — stands in for a malformed-frame cv::Exception.
class ThrowingProcessor : public FrameProcessor {
public:
    QImage process(const QImage&) override {
        throw std::runtime_error("boom");
    }
};
// A processor that inverts the first pixel — proves the result is passed through
// when no exception occurs.
class TaggingProcessor : public FrameProcessor {
public:
    QImage process(const QImage& in) override {
        QImage out = in.copy();
        out.setPixel(0, 0, qRgb(1, 2, 3));
        return out;
    }
};
} // namespace

TEST_CASE("safe_process returns the input frame when the processor throws", "[safe_process]") {
    QImage frame(4, 4, QImage::Format_RGB32);
    frame.fill(qRgb(9, 9, 9));
    ThrowingProcessor p;
    QImage out = safe_process(&p, frame);       // must not throw
    CHECK(out.size() == frame.size());
    CHECK(out.pixel(0, 0) == qRgb(9, 9, 9));     // unchanged input returned
}

TEST_CASE("safe_process passes through a successful result", "[safe_process]") {
    QImage frame(4, 4, QImage::Format_RGB32);
    frame.fill(qRgb(9, 9, 9));
    TaggingProcessor p;
    QImage out = safe_process(&p, frame);
    CHECK(out.pixel(0, 0) == qRgb(1, 2, 3));
}

TEST_CASE("safe_process tolerates a null processor", "[safe_process]") {
    QImage frame(2, 2, QImage::Format_RGB32);
    frame.fill(qRgb(5, 5, 5));
    QImage out = safe_process(nullptr, frame);
    CHECK(out.pixel(0, 0) == qRgb(5, 5, 5));
}
