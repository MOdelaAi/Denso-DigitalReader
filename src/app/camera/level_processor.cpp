#include "camera/level_processor.h"

#include "camera/frame_convert.h"   // qimage_to_mat, mat_to_qimage
#include "camera/level_overlay.h"
#include "camera/snapshot.h"        // apply_orientation

#include <QDebug>

#include <chrono>
#include <map>
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
    // No configuration and no zones: nothing is being measured, so nothing is
    // outlined. Only the camera-level state word is drawn, through the same
    // burn-in boundary the measuring path uses.
    draw_level_camera_state(bgr, std::string(level::level_state_label(state_)));
    return mat_to_qimage(bgr);
}

// ── BallLevelProcessor ──────────────────────────────────────────────────────

std::atomic<uint64_t> BallLevelProcessor::s_constructed_{0};
std::atomic<uint64_t> BallLevelProcessor::s_inferences_{0};

uint64_t BallLevelProcessor::constructed_count() {
    return s_constructed_.load(std::memory_order_relaxed);
}

uint64_t BallLevelProcessor::inference_count() {
    return s_inferences_.load(std::memory_order_relaxed);
}

BallLevelProcessor::BallLevelProcessor(int degrees, double pitch, double roll,
                                       InferenceEngine* engine, int class_id,
                                       std::vector<level::LevelZone> zones,
                                       int64_t camera_id, ZoneSink* zone_sink,
                                       WorkerFailedFn on_worker_failed,
                                       ZoneViewFn zone_view)
    : degrees_(degrees), pitch_(pitch), roll_(roll), engine_(engine),
      class_id_(class_id), zones_(std::move(zones)), camera_id_(camera_id),
      zone_sink_(zone_sink), on_worker_failed_(std::move(on_worker_failed)),
      zone_view_(std::move(zone_view)) {
    s_constructed_.fetch_add(1, std::memory_order_relaxed);
    // Start the worker LAST, once every member is initialized — it reads
    // engine_, class_id_, zones_, zone_sink_ and on_worker_failed_ every frame.
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
    // Clearing the WHOLE vector, not just the percentages: a retained ball box
    // would outline a detection from before the interruption on a frame captured
    // after it.
    results_.clear();
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
        // The measurements die with the interruption. Clearing the cause must
        // never resurrect numbers measured before it: the picture the operator
        // sees after a camera returns has to come from a frame taken after it
        // returned.
        invalidate_measurement();
    }
}

level::LevelRuntimeEntry BallLevelProcessor::build_entry() const {
    // Caller holds out_mtx_.
    const int64_t ts = measured_ts_ms_ != 0 ? measured_ts_ms_ : now_ms();
    // Either cause WINS over any measurement, however recent. This is the rule
    // that stops a stale number being presented as live.
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
    if (!have_measurement_) {
        return level::LevelRuntimeEntry::acquiring(camera_id_, ts);
    }
    // A camera is Healthy as soon as it is measuring; whether an INDIVIDUAL zone
    // has a value is the zone's own business and travels through the shared zone
    // projection. Collapsing "some zone has no ball" into a camera fault is
    // exactly the sibling coupling the amendment forbids: one zone without a
    // detection must not erase a healthy sibling.
    return level::LevelRuntimeEntry::healthy(camera_id_, 0.0, ts);
}

level::LevelRuntimeEntry BallLevelProcessor::camera_snapshot() const {
    std::lock_guard<std::mutex> lk(out_mtx_);
    return build_entry();
}

std::vector<LevelZoneResult> BallLevelProcessor::snapshot() const {
    std::lock_guard<std::mutex> lk(out_mtx_);
    return results_;
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

    // The zone states the REPORTER decided, keyed by zone number. Reading them
    // here rather than deriving them locally is what keeps one authority: the
    // aggregator owns Acquiring/Healthy/Inhibited and the camera-level Paused,
    // and this overlay renders that decision instead of forming a second one.
    std::map<int, ZoneDisplayState> published;
    if (zone_view_) {
        for (const ZoneRuntimeEntry& e : zone_view_()) {
            published[e.zone_no] = e.state;
        }
    }

    std::vector<LevelZoneResult> results;
    bool measuring = false;
    {
        std::lock_guard<std::mutex> lk(out_mtx_);
        const level::LevelRuntimeEntry entry = build_entry();
        measuring = entry.state == level::LevelState::Healthy;
        if (measuring) {
            results = results_;
        }
    }

    // Build one draw row per CONFIGURED zone, in configured order — never per
    // RESULT. A zone that produced nothing must still appear, outlined and
    // captioned, or the operator loses sight of where it was configured.
    std::vector<LevelZoneDraw> draws;
    draws.reserve(zones_.size());
    for (size_t i = 0; i < zones_.size(); ++i) {
        LevelZoneDraw d;
        d.zone_no = zones_[i].zone_no;
        d.calib = zones_[i].calibration;
        const auto it = published.find(d.zone_no);
        d.state = it != published.end() ? it->second : ZoneDisplayState::Acquiring;
        // A number is drawn only where BOTH authorities agree there is one: the
        // reporter says the zone is Healthy, and this frame's own measurement
        // carries a percentage. Either alone would be a stale-value path — the
        // reporter's Healthy survives a moment longer than the measurement, and
        // the measurement outlives a camera-level pause.
        if (measuring && d.state == ZoneDisplayState::Healthy &&
            i < results.size() && results[i].percent) {
            d.percent = results[i].percent;
            d.ball = results[i].ball;
        }
        draws.push_back(std::move(d));
    }

    // Drawn on the display-only Mat, immediately before the conversion — the
    // same composition boundary the digit zone panel uses.
    draw_level_overlay(bgr, draws);
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
        //
        // ONE call, for the whole camera, however many zones it has. The zone
        // loop is below and downstream of this — it cannot reach the engine.
        std::vector<Detection> raw;
        try {
            if (engine_) {
                s_inferences_.fetch_add(1, std::memory_order_relaxed);
                raw = engine_->infer(frame);
            }
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
            continue;   // publish NOTHING — a failed frame must not move any value
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
        // ONE bound class survives: the durable configuration names exactly one
        // for the whole CAMERA, and a Float engine could in principle declare
        // more. Converted ONCE and shared by every zone.
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

        // Selection and mapping belong to the PURE core, applied per zone over
        // the SAME candidate set. No rule of either is restated here.
        const std::vector<LevelZoneResult> results =
            evaluate_level_zones(candidates, zones_);

        bool publish = false;
        {
            std::lock_guard<std::mutex> lk(out_mtx_);
            // A cause engaged while this frame was in flight, so this result
            // describes the world BEFORE the interruption. Publishing it would
            // reintroduce exactly the stale reading invalidation exists to
            // prevent — the next frame measures the world as it is now.
            if (avail_gen_ == submitted_gen) {
                have_measurement_ = true;
                results_ = results;
                measured_ts_ms_ = now_ms();
                publish = true;
            }
        }

        // The ZoneSink hand-off, OUTSIDE out_mtx_: ZoneReporter takes its own
        // mutex and invokes the snapshot callback, and holding two locks across
        // that boundary is how a deadlock gets built. Emitted only for a frame
        // that survived the epoch check — a discarded measurement must not reach
        // the backend either.
        //
        // EVERY configured zone is emitted, including those that selected
        // nothing: the aggregator uses continued presence as a zone's liveness
        // signal, and a zone that simply stopped appearing would take the slow
        // expiry path instead of being recognised as present-but-not-reading.
        // With the Ball aggregator's hold_timeout_ms = 0 a NoValue reading
        // evicts the zone's value immediately, which is what makes "no stale
        // percentage is ever reported" true through the shared reporter.
        if (publish && zone_sink_) {
            zone_sink_->on_zones(camera_id_, level_zone_readings(results));
        }
    }
}

}  // namespace denso::ui
