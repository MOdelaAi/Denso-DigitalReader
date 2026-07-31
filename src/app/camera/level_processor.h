// The Ball Leveler per-camera runtime: a FrameProcessor that measures floating
// balls against 1..kMaxBallZones stored calibrations and burns the results into
// the displayed frame.
//
// Ball Leveler is another PRODUCER of per-zone numeric values on the SAME zone
// infrastructure the digit reader uses (amendment §10). What differs is only the
// zone-processing logic:
//
//     digit: detections -> digit assembly                       -> zone value
//     ball:  detections -> ball selection -> percentage mapping -> zone value
//
// Below the ZoneSink seam nothing differs at all: the same ZoneReporter, the
// same aggregator (with Ball's two parameters), the same BrazingReporter, the
// same retry policy, the same `{"zoneN": v}` payload.
//
// ONE MODEL, ONE INFERENCE, N ZONES. The camera binds exactly one Float model
// (level::LevelConfig has one model_id for the whole camera) and this class runs
// exactly one infer() per submitted frame, then hands the SAME detection set to
// every zone. Per-zone inference is not merely discouraged here, it is
// unsayable: there is one engine_ pointer and one call site.
//
// Threading contract, copied verbatim from DetectionProcessor because it is the
// proven one:
//   * every member is initialized BEFORE the worker thread starts;
//   * the display path submits a private copy into a drop-oldest latest-frame
//     slot and never blocks on inference;
//   * the worker's body is wrapped in an exception firewall (this is a bare
//     std::thread, so an escaping exception would std::terminate the app);
//   * the destructor stops and joins;
//   * the pending input and the published output each have their own mutex;
//   * NO SQLite is touched from the worker or the capture thread.
#pragma once

#include "brazing/zone_sink.h"     // ZoneSink
#include "camera/frame_processor.h"  // FrameProcessor, ZoneViewFn
#include "camera/level_zone_assembly.h"  // LevelZoneResult
#include "detection/inference_engine.h"
#include "level/calibration.h"
#include "level/measure.h"
#include "level/runtime.h"

#include <QImage>

#include <opencv2/core.hpp>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace denso::ui {

/// The display-only processor for a Ball Leveler camera that is NOT measuring:
/// no stored configuration (Unconfigured), a calibration that no longer applies
/// to the view (CalibrationInvalid), or a configuration that could not be read.
///
/// It runs NO inference, holds NO engine and starts NO thread — it exists so
/// that "this camera is not measuring, and here is why" is visible on the wall
/// instead of being an unexplained live picture. Keeping it a separate type from
/// BallLevelProcessor is deliberate: BallLevelProcessor::constructed_count()
/// then means exactly "a measuring pipeline was built", which is what the
/// no-processor-for-an-unconfigured-camera tests assert.
///
/// It owns no zone concept: these states are CAMERA-level, and a camera with no
/// readable configuration has no zones to speak of.
class LevelStateProcessor : public FrameProcessor {
public:
    LevelStateProcessor(int degrees, double pitch, double roll,
                        int64_t camera_id, level::LevelState state);

    QImage process(const QImage& frame) override;

private:
    int degrees_;
    double pitch_;
    double roll_;
    int64_t camera_id_;
    level::LevelState state_;
};

/// The measuring processor. Constructed ONLY for a camera with a stored,
/// currently-valid configuration bound to a compatible, loadable Float engine.
class BallLevelProcessor : public FrameProcessor {
public:
    /// Raised when consecutive inference failures cross the escalation
    /// threshold, and cleared once inference recovers. CONTRACT: invoked on the
    /// INFERENCE WORKER THREAD — the wiring must marshal to the GUI thread
    /// before touching any GUI-owned state. Passed at construction, not via a
    /// setter, so it is set before the worker starts: the worker reads it every
    /// frame, and a later write from another thread would be a data race on the
    /// std::function.
    using WorkerFailedFn = std::function<void(int64_t camera_id, bool failed)>;

    /// Test-only: how many BallLevelProcessors this process has EVER constructed
    /// (monotonic, increment-only). Mirrors DetectionProcessor::constructed_count()
    /// and CameraStream::constructed_count(). It is what lets a test PROVE that an
    /// unconfigured or invalid camera built no measuring pipeline, rather than
    /// inferring it from an absence of side effects. No production behaviour reads it.
    static uint64_t constructed_count();

    /// Test-only: how many times infer() has been invoked across all instances.
    /// This is what lets a test PROVE "four zones do not cause four engine calls"
    /// directly, instead of inferring it from timings. No production behaviour
    /// reads it.
    static uint64_t inference_count();

    /// `engine` is shared and non-owning (owned by EngineRegistry). `class_id` is
    /// the single class the durable configuration bound for this CAMERA —
    /// detections of any other class are discarded before selection. `zones`
    /// carries the 1..kMaxBallZones measurement geometries; each supplies its own
    /// confidence threshold, and there is deliberately no second copy of any of
    /// them anywhere.
    ///
    /// `zone_sink` is the SHARED machine-wide ZoneReporter (non-owning; null
    /// disables reporting, as it does for DetectionProcessor). `zone_view`
    /// supplies this camera's rows from that same reporter's projection, so the
    /// overlay renders the state the REPORTER decided rather than a second
    /// opinion computed here.
    BallLevelProcessor(int degrees, double pitch, double roll,
                       InferenceEngine* engine, int class_id,
                       std::vector<level::LevelZone> zones, int64_t camera_id,
                       ZoneSink* zone_sink = nullptr,
                       WorkerFailedFn on_worker_failed = {},
                       ZoneViewFn zone_view = {});
    ~BallLevelProcessor() override;   // stops + joins the inference worker

    BallLevelProcessor(const BallLevelProcessor&) = delete;
    BallLevelProcessor& operator=(const BallLevelProcessor&) = delete;

    QImage process(const QImage& frame) override;   // display path: submit + draw

    /// CAMERA-level unavailability (offline, paused, inhibited). Set from the
    /// GUI thread; read on the capture thread, so it is mutex-guarded.
    ///
    /// While a reason is set the published picture carries NO percentage for any
    /// zone — which is how "an offline camera clears every live value" is
    /// guaranteed by construction rather than by every draw site remembering to
    /// check. Pass nullopt to clear.
    ///
    /// This cause is INDEPENDENT of the inference-failure cause below. They were
    /// one slot once, and that was a defect: the camera coming back online wrote
    /// nullopt over a live `inference_error`, and because escalation is
    /// edge-triggered the worker never re-raised it — so a dead model published
    /// its last good percentage as a live reading, forever. Two causes, two
    /// slots, and neither may clear the other.
    ///
    /// Note what this does NOT do: it does not evict the camera's zones from the
    /// aggregator. That is ZoneReporter::set_camera_inhibited's job and the grid
    /// calls it, exactly as it does for a digit camera — one eviction authority,
    /// not two.
    void set_unavailable(std::optional<std::string> reason);

    /// The per-zone measurements the next drawn frame would use, in configured
    /// order. GUI-thread observable for tests; production reads them only through
    /// process(). Empty percentages where a zone is not currently measuring.
    std::vector<LevelZoneResult> snapshot() const;

    /// The camera-level picture, for the tile status. Zone VALUES do not travel
    /// through here — they go to the shared reporter — so this carries state and
    /// reason only.
    level::LevelRuntimeEntry camera_snapshot() const;

private:
    void infer_loop();
    /// Drop every published measurement. Called whenever a cause is ENGAGED, so a
    /// number measured before an interruption can never be republished when that
    /// cause clears: every zone returns to Acquiring and must measure again.
    /// Caller holds out_mtx_.
    void invalidate_measurement();
    /// The camera-level entry every consumer sees. The single place where
    /// camera state is decided — scattering this across processor, overlay and
    /// grid is exactly the second policy authority the design forbids.
    level::LevelRuntimeEntry build_entry() const;

    int degrees_;
    double pitch_;
    double roll_;
    InferenceEngine* engine_ = nullptr;   // shared, non-owning
    int class_id_ = 0;
    /// The camera's zones, in configured order. IMMUTABLE after construction:
    /// the worker reads it on every frame without a lock, and a reconfiguration
    /// rebuilds the processor rather than mutating this.
    std::vector<level::LevelZone> zones_;
    int64_t camera_id_ = 0;
    ZoneSink* zone_sink_ = nullptr;       // shared, non-owning; null = no reporting
    WorkerFailedFn on_worker_failed_;
    ZoneViewFn zone_view_;

    // Latest-frame slot handed to the inference worker (drop-oldest).
    mutable std::mutex slot_mtx_;
    std::condition_variable slot_cv_;
    cv::Mat pending_;
    /// The availability epoch the QUEUED frame was captured in, stamped when it
    /// is submitted. Stamping at DEQUEUE instead would re-date a frame that was
    /// already waiting when the interruption happened, and publish a
    /// pre-interruption picture as a post-recovery measurement.
    uint64_t pending_gen_ = 0;
    bool has_pending_ = false;
    bool stop_ = false;

    // Published measurements (worker -> display) and external availability
    // (GUI -> display), under one mutex because the draw path reads both and
    // must see a consistent pair.
    mutable std::mutex out_mtx_;
    bool have_measurement_ = false;            ///< the worker has completed a frame
    std::vector<LevelZoneResult> results_;     ///< one per configured zone
    std::optional<std::string> unavailable_;   ///< CAMERA-level reason, if any
    /// WORKER-level cause, owned by the inference thread and cleared ONLY by a
    /// genuine inference success. Kept apart from unavailable_ so no camera
    /// status transition can silently retire a broken model.
    bool inference_failed_ = false;
    int64_t measured_ts_ms_ = 0;
    /// Bumped every time a cause is ENGAGED. Each frame is stamped with the
    /// current value AS IT IS SUBMITTED, and the worker refuses to publish a
    /// result whose stamp is stale — otherwise a frame already inside infer()
    /// when the camera dropped would land AFTER invalidate_measurement() and
    /// become the "live" reading the moment the cause cleared. Invalidating
    /// without this closes the window from one side only.
    uint64_t avail_gen_ = 0;

    // Consecutive inference-failure count (worker thread only; no lock needed).
    static constexpr int kInferFailLogEvery = 60;
    static constexpr int kInferFailInhibitAfter = 10;
    int infer_fail_streak_ = 0;
    /// Latches the escalation so the notification stays edge-triggered (one
    /// raise, one clear) while the STATE it reports is level-triggered. Worker
    /// thread only.
    bool escalated_ = false;

    static std::atomic<uint64_t> s_constructed_;
    static std::atomic<uint64_t> s_inferences_;

    std::thread worker_;   // started LAST in the ctor
};

}  // namespace denso::ui
