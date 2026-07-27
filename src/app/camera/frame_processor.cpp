#include "camera/frame_processor.h"

#include "camera/area_geometry.h"             // inside_any_area
#include "camera/zone_assembly.h"    // group_into_zones
#include "camera/frame_convert.h"  // qimage_to_mat, mat_to_qimage
#include "camera/snapshot.h"       // apply_orientation
#include "detection/merge_detections.h"  // merge_detections

#include <QDebug>

#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <optional>

namespace denso::ui {

OrientationProcessor::OrientationProcessor(int degrees, double pitch, double roll)
    : degrees_(degrees), pitch_(pitch), roll_(roll) {}

QImage OrientationProcessor::process(const QImage& frame) {
    return apply_orientation(frame, degrees_, pitch_, roll_);
}

std::atomic<uint64_t> DetectionProcessor::s_constructed_{0};

uint64_t DetectionProcessor::constructed_count() {
    return s_constructed_.load(std::memory_order_relaxed);
}

DetectionProcessor::DetectionProcessor(int degrees, double pitch, double roll,
                                       std::vector<ModelRun> models,
                                       std::vector<denso::camera::CameraArea> areas,
                                       int64_t camera_id, ReadingSink* sink,
                                       ZoneSink* zone_sink,
                                       WorkerFailedFn on_worker_failed)
    : degrees_(degrees), pitch_(pitch), roll_(roll),
      models_(std::move(models)), areas_(std::move(areas)),
      camera_id_(camera_id), sink_(sink), zone_sink_(zone_sink),
      on_worker_failed_(std::move(on_worker_failed)) {
    // Monotonic process-lifetime tally (see constructed_count()). relaxed: this is
    // an observable counter, not an ordering primitive for other state.
    s_constructed_.fetch_add(1, std::memory_order_relaxed);
    // Start the inference worker LAST, once every member is initialized — the
    // worker reads on_worker_failed_ every frame, so it must be set before start.
    worker_ = std::thread([this] { infer_loop(); });
}

DetectionProcessor::~DetectionProcessor() {
    {
        std::lock_guard<std::mutex> lk(slot_mtx_);
        stop_ = true;
    }
    slot_cv_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
}

namespace {
// Min confidence a camera keeps for class_id, or nullopt if not selected.
std::optional<float> selected_conf(
    const std::vector<denso::detection::ModelClassSelection>& sel, int class_id) {
    for (const auto& s : sel) {
        if (s.class_id == class_id) return s.conf;
    }
    return std::nullopt;
}
constexpr float kMergeIoU = 0.5f;  // cross-model boxes of a class over this merge

void draw_detections(cv::Mat& bgr, const std::vector<NamedDetection>& kept) {
    for (const NamedDetection& d : kept) {
        cv::rectangle(bgr, d.box, cv::Scalar(0, 215, 255), 2);
        std::string label = d.name;
        char buf[16];
        std::snprintf(buf, sizeof(buf), " %.0f%%", d.conf * 100.0f);
        label += buf;
        cv::putText(bgr, label, cv::Point(d.box.x, std::max(0, d.box.y - 4)),
                    cv::FONT_HERSHEY_SIMPLEX, 0.5, cv::Scalar(0, 215, 255), 1);
    }
}
} // namespace

// Display path (capture thread): orient, hand the newest frame to the inference
// worker (dropping any older un-processed one), and draw the most-recent
// detections. No inference here — the video stays smooth regardless of model
// speed.
QImage DetectionProcessor::process(const QImage& frame) {
    const QImage oriented = apply_orientation(frame, degrees_, pitch_, roll_);
    cv::Mat bgr = qimage_to_mat(oriented);
    if (bgr.empty()) {
        return oriented;
    }

    // Submit a private copy so the worker can read it while we draw on `bgr`.
    {
        std::lock_guard<std::mutex> lk(slot_mtx_);
        bgr.copyTo(pending_);
        has_pending_ = true;
    }
    slot_cv_.notify_one();

    // Overlay the latest detections (computed asynchronously). Only when the
    // snapshot's frame size matches this frame do the boxes align.
    std::vector<NamedDetection> boxes;
    int dw = 0;
    int dh = 0;
    {
        std::lock_guard<std::mutex> lk(det_mtx_);
        boxes = latest_;
        dw = det_w_;
        dh = det_h_;
    }
    if (dw == bgr.cols && dh == bgr.rows && !boxes.empty()) {
        draw_detections(bgr, boxes);
    }
    return mat_to_qimage(bgr);
}

// Pool every model's kept detections, tagged by class *name* so the same class
// from different models can merge despite differing ids, then cross-model NMS.
std::vector<NamedDetection> DetectionProcessor::run_inference(const cv::Mat& bgr) {
    const float w = static_cast<float>(bgr.cols);
    const float h = static_cast<float>(bgr.rows);

    std::vector<NamedDetection> pool;
    for (const ModelRun& run : models_) {
        if (!run.engine) continue;
        for (const Detection& d : run.engine->infer(bgr)) {
            const auto conf = selected_conf(run.classes, d.class_id);
            if (!conf || d.conf < *conf) continue;  // not selected / below thr
            if (!areas_.empty() && w > 0.0f && h > 0.0f) {
                const denso::camera::Point center{
                    (d.box.x + d.box.width * 0.5f) / w,
                    (d.box.y + d.box.height * 0.5f) / h};
                if (!denso::camera::inside_any_area(areas_, center)) continue;
            }
            std::string name =
                (d.class_id < static_cast<int>(run.class_names.size())
                     ? run.class_names[d.class_id]
                     : std::to_string(d.class_id));
            pool.push_back({d.box, d.conf, std::move(name)});
        }
    }
    return merge_detections(std::move(pool), kMergeIoU);
}

// Inference worker: consume the newest submitted frame, run the model(s), publish
// the detections snapshot, and feed the reading/zone sinks — all off the display
// path.
void DetectionProcessor::infer_loop() {
    for (;;) {
        cv::Mat frame;
        {
            std::unique_lock<std::mutex> lk(slot_mtx_);
            slot_cv_.wait(lk, [this] { return has_pending_ || stop_; });
            if (stop_) {
                return;
            }
            frame = std::move(pending_);
            has_pending_ = false;
        }
        if (frame.empty()) {
            continue;
        }

        // Exception firewall for the inference worker. TrtEngine::infer() (and
        // the ORT path) THROWS on a CUDA copy / enqueue / sync failure; this is
        // a bare std::thread body, so an escaping exception would std::terminate
        // the whole app — the field device would die on a transient GPU hiccup.
        // Skip the failed frame (don't publish stale/empty boxes, don't feed the
        // zone sink a phantom reading) and stay alive; the next frame retries.
        // Log is throttled so a persistent failure surfaces without spamming.
        std::vector<NamedDetection> kept;
        try {
            kept = run_inference(frame);
        } catch (const std::exception& e) {
            if (infer_fail_streak_++ % kInferFailLogEvery == 0) {
                qWarning().noquote()
                    << "[infer] camera" << camera_id_ << "inference failed ("
                    << infer_fail_streak_ << " in a row):" << e.what();
            }
            // Escalate exactly once, when the streak first reaches the threshold —
            // a persistent GPU/engine fault inhibits the camera's zones rather than
            // holding a stale reading forever. The handler marshals to the GUI.
            if (infer_fail_streak_ == kInferFailInhibitAfter && on_worker_failed_) {
                on_worker_failed_(camera_id_, true);
            }
            continue;
        }
        // Clear the escalation on the first good frame after we had inhibited.
        if (infer_fail_streak_ >= kInferFailInhibitAfter && on_worker_failed_) {
            on_worker_failed_(camera_id_, false);
        }
        infer_fail_streak_ = 0;  // recovered

        {
            std::lock_guard<std::mutex> lk(det_mtx_);
            latest_ = kept;
            det_w_ = frame.cols;
            det_h_ = frame.rows;
        }

        // Reading-capture seam (dormant until a sink is wired). Timestamp only
        // when a sink exists.
        if (sink_) {
            const int64_t ts_ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch())
                    .count();
            sink_->on_reading(camera_id_, ts_ms, kept);
        }
        // Zone-reporting seam (brazing): assemble kept digits per zone.
        if (zone_sink_) {
            const float w = static_cast<float>(frame.cols);
            const float h = static_cast<float>(frame.rows);
            zone_sink_->on_zones(camera_id_, group_into_zones(kept, areas_, w, h));
        }
    }
}

} // namespace denso::ui
