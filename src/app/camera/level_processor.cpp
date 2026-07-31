#include "camera/level_processor.h"

#include "camera/frame_convert.h"   // qimage_to_mat, mat_to_qimage
#include "camera/level_overlay.h"
#include "camera/snapshot.h"        // apply_orientation

#include <QDebug>

#include <chrono>
#include <utility>

namespace denso::ui {

namespace {

int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

}  // namespace

// ── LevelStateProcessor ─────────────────────────────────────────────────────

LevelStateProcessor::LevelStateProcessor(int degrees, double pitch, double roll,
                                         int64_t camera_id, level::LevelState state)
    : degrees_(degrees), pitch_(pitch), roll_(roll), camera_id_(camera_id),
      state_(state) {}

QImage LevelStateProcessor::process(const QImage& frame) {
    const QImage oriented = apply_orientation(frame, degrees_, pitch_, roll_);
    cv::Mat bgr = qimage_to_mat(oriented);   // owns its bytes — see frame_convert.h
    if (bgr.empty()) {
        return oriented;
    }
    level::LevelRuntimeEntry entry;
    entry.camera_id = camera_id_;
    entry.state = state_;
    entry.ts_ms = now_ms();
    // No calibration and no ball: nothing is being measured, so nothing is
    // outlined. Only the state text is drawn.
    draw_level_overlay(bgr, std::nullopt, entry, std::nullopt);
    return mat_to_qimage(bgr);
}

// ── BallLevelProcessor ──────────────────────────────────────────────────────

std::atomic<uint64_t> BallLevelProcessor::s_constructed_{0};

uint64_t BallLevelProcessor::constructed_count() {
    return s_constructed_.load(std::memory_order_relaxed);
}

BallLevelProcessor::BallLevelProcessor(int degrees, double pitch, double roll,
                                       InferenceEngine* engine, int class_id,
                                       level::LevelCalibration calibration,
                                       int64_t camera_id,
                                       WorkerFailedFn on_worker_failed)
    : degrees_(degrees), pitch_(pitch), roll_(roll), engine_(engine),
      class_id_(class_id), calibration_(calibration), camera_id_(camera_id),
      on_worker_failed_(std::move(on_worker_failed)) {
    s_constructed_.fetch_add(1, std::memory_order_relaxed);
    // Start the worker LAST, once every member is initialized — it reads
    // engine_, class_id_, calibration_ and on_worker_failed_ every frame.
    worker_ = std::thread([this] { infer_loop(); });
}

BallLevelProcessor::~BallLevelProcessor() {
    {
        std::lock_guard<std::mutex> lk(slot_mtx_);
        stop_ = true;
    }
    slot_cv_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
}

void BallLevelProcessor::invalidate_measurement() {
    // Caller holds out_mtx_.
    have_measurement_ = false;
    percent_.reset();
    ball_.reset();
    // Drop the measurement's timestamp too, so an Acquiring/Unavailable entry
    // cannot be dated to a measurement that has been discarded.
    measured_ts_ms_ = 0;
    // Anything already inside infer() belongs to the picture we just discarded.
    ++avail_gen_;
}

void BallLevelProcessor::set_unavailable(std::optional<std::string> reason) {
    std::lock_guard<std::mutex> lk(out_mtx_);
    const bool engaging = reason.has_value();
    unavailable_ = std::move(reason);
    if (engaging) {
        // The measurement dies with the interruption. Clearing the cause must
        // never resurrect a number measured before it: the picture the operator
        // sees after a camera returns has to come from a frame taken after it
        // returned.
        invalidate_measurement();
    }
}

level::LevelRuntimeEntry BallLevelProcessor::build_entry() const {
    // Caller holds out_mtx_.
    const int64_t ts = measured_ts_ms_ != 0 ? measured_ts_ms_ : now_ms();
    // Either cause WINS over any measurement, however recent. This is the rule
    // that stops a stale number being presented as live: the entry it builds
    // carries no percentage at all, so there is nothing for a draw site to
    // accidentally render.
    //
    // The camera cause is reported first when both are engaged: an offline
    // camera is the more proximate fault and the one the operator acts on, and a
    // model cannot be shown to be working while no frames are reaching it.
    if (unavailable_) {
        return level::LevelRuntimeEntry::unavailable(camera_id_, *unavailable_, ts);
    }
    if (inference_failed_) {
        return level::LevelRuntimeEntry::unavailable(
            camera_id_, level::kReasonInferenceError, ts);
    }
    // A completed frame that selected no ball is NOT a measurement. Reporting
    // Acquiring is the honest answer — the alternatives are inventing a number
    // or keeping the previous one, and both are forbidden.
    if (!have_measurement_ || !percent_) {
        return level::LevelRuntimeEntry::acquiring(camera_id_, ts);
    }
    return level::LevelRuntimeEntry::healthy(camera_id_, *percent_, ts);
}

level::LevelRuntimeEntry BallLevelProcessor::snapshot() const {
    std::lock_guard<std::mutex> lk(out_mtx_);
    return build_entry();
}

QImage BallLevelProcessor::process(const QImage& frame) {
    const QImage oriented = apply_orientation(frame, degrees_, pitch_, roll_);
    cv::Mat bgr = qimage_to_mat(oriented);
    if (bgr.empty()) {
        return oriented;
    }

    // The epoch this frame belongs to, read BEFORE it is queued. Read outside the
    // slot lock deliberately: if a cause engages between this read and the
    // submission below, the frame is stamped with the OLDER epoch and its result
    // is discarded. That is the safe direction — it costs one extra Acquiring
    // frame, where the reverse would publish a pre-interruption picture.
    uint64_t gen = 0;
    {
        std::lock_guard<std::mutex> lk(out_mtx_);
        gen = avail_gen_;
    }

    // Submit a PRIVATE copy so the worker can read it while we draw on `bgr`.
    // `bgr` from here on is display-only: nothing drawn below can reach
    // inference, a snapshot, or any other consumer.
    {
        std::lock_guard<std::mutex> lk(slot_mtx_);
        bgr.copyTo(pending_);
        pending_gen_ = gen;
        has_pending_ = true;
    }
    slot_cv_.notify_one();

    level::LevelRuntimeEntry entry;
    std::optional<level::BallBox> ball;
    {
        std::lock_guard<std::mutex> lk(out_mtx_);
        entry = build_entry();
        // Draw the box only when it belongs to the picture being reported. An
        // Unavailable camera showing a ball outline would contradict its own
        // "not measuring" caption.
        if (entry.state == level::LevelState::Healthy) {
            ball = ball_;
        }
    }
    // Drawn on the display-only Mat, immediately before the conversion — this is
    // the boundary the zone overlay already proves.
    draw_level_overlay(bgr, calibration_, entry, ball);
    return mat_to_qimage(bgr);
}

void BallLevelProcessor::infer_loop() {
    for (;;) {
        cv::Mat frame;
        uint64_t submitted_gen = 0;
        {
            std::unique_lock<std::mutex> lk(slot_mtx_);
            slot_cv_.wait(lk, [this] { return has_pending_ || stop_; });
            if (stop_) {
                return;
            }
            frame = std::move(pending_);
            // Travels WITH the frame: this is the epoch it was captured in, not
            // the epoch it happens to be dequeued in.
            submitted_gen = pending_gen_;
            has_pending_ = false;
        }
        if (frame.empty() || frame.cols <= 0 || frame.rows <= 0) {
            continue;
        }

        // Exception firewall. TrtEngine::infer() throws on a CUDA copy / enqueue
        // / sync failure, and this is a bare std::thread body, so an escaping
        // exception would std::terminate the whole app — the appliance would die
        // on a transient GPU hiccup. Skip the frame and stay alive.
        std::vector<Detection> raw;
        try {
            raw = engine_ ? engine_->infer(frame) : std::vector<Detection>{};
        } catch (const std::exception& e) {
            if (infer_fail_streak_++ % kInferFailLogEvery == 0) {
                qWarning().noquote()
                    << "[level] camera" << camera_id_ << "inference failed ("
                    << infer_fail_streak_ << "in a row):" << e.what();
            }
            // Level-triggered STATE, edge-triggered NOTIFICATION. `>=` with a
            // latch rather than `==`: an `==` test fires exactly once ever, so a
            // cause cleared by anything else could never be re-raised.
            if (infer_fail_streak_ >= kInferFailInhibitAfter && !escalated_) {
                escalated_ = true;
                {
                    std::lock_guard<std::mutex> lk(out_mtx_);
                    inference_failed_ = true;
                    invalidate_measurement();
                }
                if (on_worker_failed_) {
                    on_worker_failed_(camera_id_, true);
                }
            }
            continue;   // publish NOTHING — a failed frame must not move the value
        }
        if (escalated_) {
            escalated_ = false;
            {
                std::lock_guard<std::mutex> lk(out_mtx_);
                inference_failed_ = false;   // ONLY a real success clears it
            }
            if (on_worker_failed_) {
                on_worker_failed_(camera_id_, false);   // recovered
            }
        }
        infer_fail_streak_ = 0;

        // Backend detections into the pure core's normalized BallBox. Only the
        // ONE bound class survives: the durable configuration names exactly one,
        // and a Float engine could in principle declare more.
        const double w = static_cast<double>(frame.cols);
        const double h = static_cast<double>(frame.rows);
        std::vector<level::BallBox> candidates;
        candidates.reserve(raw.size());
        for (const Detection& d : raw) {
            if (d.class_id != class_id_) continue;
            level::BallBox b;
            b.x1 = d.box.x / w;
            b.y1 = d.box.y / h;
            b.x2 = (d.box.x + d.box.width) / w;
            b.y2 = (d.box.y + d.box.height) / h;
            b.conf = static_cast<double>(d.conf);
            candidates.push_back(b);
        }

        // Selection and mapping belong to the PURE core: containment in the
        // measurement rectangle, the calibration's confidence threshold, the
        // highest-confidence winner, and the bbox vertical centre mapped through
        // the reference lines. No rule of either is restated here.
        const std::optional<level::BallChoice> choice =
            level::select_ball(candidates, calibration_);
        std::optional<double> pct;
        std::optional<level::BallBox> chosen;
        if (choice) {
            chosen = choice->box;
            pct = level::level_percent(calibration_, choice->box.centre_y());
            // level_percent returns nullopt on a calibration that does not
            // validate or a non-finite centre. Keep the box for the overlay only
            // if it produced a number — a drawn box with no reading would say
            // "measured" while the caption reported Acquiring.
            if (!pct) {
                chosen.reset();
            }
        }

        {
            std::lock_guard<std::mutex> lk(out_mtx_);
            // A cause engaged while this frame was in flight, so this result
            // describes the world BEFORE the interruption. Publishing it would
            // reintroduce exactly the stale reading invalidation exists to
            // prevent — the next frame measures the world as it is now.
            if (avail_gen_ == submitted_gen) {
                have_measurement_ = true;
                percent_ = pct;
                ball_ = chosen;
                measured_ts_ms_ = now_ms();
            }
        }
    }
}

}  // namespace denso::ui
