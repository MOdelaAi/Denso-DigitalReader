#include <catch2/catch_test_macros.hpp>

#include "camera/frame_processor.h"
#include "detection/inference_engine.h"

#include <QImage>

#include <atomic>
#include <chrono>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace denso::ui;

namespace {
// An engine whose infer() always throws — stands in for a CUDA copy / enqueueV3
// / sync failure inside TrtEngine::infer(). Counts calls so the test can prove
// the inference worker keeps consuming frames after a throw instead of dying.
class ThrowingEngine : public InferenceEngine {
public:
    std::vector<Detection> infer(const cv::Mat&) override {
        calls.fetch_add(1);
        throw std::runtime_error("simulated CUDA failure");
    }
    const std::vector<std::string>& class_names() const override { return names_; }

    std::atomic<int> calls{0};

private:
    std::vector<std::string> names_;
};
} // namespace

// The inference worker runs as a bare std::thread body, and TrtEngine::infer()
// deliberately throws on a GPU failure. Without a firewall in infer_loop() the
// escaping exception would std::terminate the whole app. Reaching the asserts at
// all proves it didn't: a std::terminate would abort the test binary.
TEST_CASE("DetectionProcessor survives a throwing inference worker", "[frame_processor]") {
    ThrowingEngine engine;
    std::vector<DetectionProcessor::ModelRun> runs;
    runs.push_back({&engine, /*class_names=*/{}, /*classes=*/{}});
    DetectionProcessor proc(/*degrees=*/0, /*pitch=*/0.0, /*roll=*/0.0, std::move(runs));

    QImage frame(8, 8, QImage::Format_RGB32);
    frame.fill(qRgb(0, 0, 0));

    // Submit several frames with a gap so the drop-oldest worker picks up more
    // than one, throwing each time.
    for (int i = 0; i < 10; ++i) {
        QImage out = proc.process(frame);  // display path: must never throw
        CHECK(out.size() == frame.size());
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    // Give the worker a beat to drain the final submission.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // The worker consumed multiple frames despite every infer() throwing —
    // proof the loop recovered rather than terminating on the first throw.
    CHECK(engine.calls.load() >= 2);
}
