// The live 1–4 camera grid. Reads up to the first four cameras, arranges a tile
// per camera (1 / 1×2 / 2×2 via grid_dims), and owns a CameraStream per tile.
// reload() rebuilds from the DB and starts streaming; release_streams() stops
// capture (e.g. so the Camera modal can grab the same USB device) without
// tearing down the tiles. Streaming stops on destruction.
#pragma once

#include "camera/callback_generation.h"
#include "camera/camera.h"
#include "camera/warmup_gate.h"
#include "detection/detection.h"   // CameraDetection
#include "detection/engine_registry.h"
#include "health/integrity.h"     // IntegrityVerdict
#include "health/status_file.h"   // ZoneInhibitRecord
#include "ui/camera/grid/zone_status_publication.h"
#include "brazing/zone_runtime.h"  // ZoneRuntimeEntry
#include "health/zone_health.h"   // ZoneHealth, ZoneCause
#include "level/runtime.h"   // LevelState, LevelRuntimeEntry
#include "mode/mode.h"            // TargetMode
#include "models/model_identity.h"  // ManifestView, PlatformInfo

#include <QSqlDatabase>
#include <QString>
#include <QWidget>

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

class QGridLayout;

namespace denso::ui {

class CameraStream;
class CameraTile;
class BrazingReporter;
class ZoneReporter;
class WarmupState;
class BallLevelProcessor;

class CameraGrid : public QWidget {
    Q_OBJECT

public:
    explicit CameraGrid(QSqlDatabase db, std::shared_ptr<EngineRegistry> engines,
                        WarmupState* warmup, QWidget* parent = nullptr);
    ~CameraGrid() override;

    void reload();            // rebuild tiles + streams from the DB, then start
    void release_streams();   // stop capture, keep the tiles on screen
    void start_streams();     // (re)start the existing streams

    // The ONE authoritative teardown of the live pipeline, exposed as a callable
    // pre-transaction primitive for the mode switch (spec §6.2). It delegates to
    // clear() — the single teardown sequence — and is NOT a second copy of it. It
    // does NOT re-query runtime() or restart anything (unlike reload()).
    void teardown();

    /// Adopt a NEW inference session after a committed mode switch (spec 3.1,
    /// and the 9 lifecycle decision).
    ///
    /// An EngineRegistry is immutable and mode-pure for its whole life, so a
    /// switch cannot widen the boot registry - it must replace it. This does not
    /// weaken that property: the OBJECT is swapped, the allow-list never is, and
    /// the incoming registry was built for exactly one mode by the ONE builder in
    /// ui/engine_session.h.
    ///
    /// MUST be called only when the grid is torn down (no live stream), because
    /// a running inference worker holds a raw engine pointer from the OUTGOING
    /// registry. The existing switch ordering already guarantees this: teardown
    /// runs, and joins every capture and inference thread, BEFORE the mode
    /// transaction commits. Refuses (and logs) rather than corrupting a live
    /// pipeline if that ordering is ever broken.
    void set_engines(std::shared_ptr<EngineRegistry> engines, WarmupState* warmup);

    /// Release every camera still waiting on a model that will now never warm.
    ///
    /// A failed warm-up emits `failed` and never `finished`, so the pending gate
    /// has no terminating edge of its own. Each waiting camera is rebuilt now
    /// that WarmupState::is_complete() is true, which makes it fall THROUGH the
    /// gate and resolve honestly - a ball camera to Unavailable, a digit camera
    /// to its OrientationProcessor fallback - instead of showing
    /// "Preparing model..." for the life of the process.
    void settle_pending_after_warmup();

    // The intentionally-idle runtime status writer (spec §9, single-owner rule).
    // Used when the view deliberately avoids the live grid (ball_leveler): computes
    // the REAL integrity verdict (never a placeholder IntegrityVerdict{}), then
    // writes status.json with empty runtime causes/zones (nothing streams) plus the
    // committed mode + mode_setup_required flag. This keeps CameraGrid the sole
    // runtime writer of status.json for both the live and idle cases.
    void publish_idle_status();

    // Test-only: how many times reload()'s build path was ENTERED (monotonic,
    // increment-only, per-grid). Because the reporter, ZoneHealth, every
    // DetectionProcessor and every CameraStream are constructed ONLY inside
    // reload()/start_one, an unchanged value proves none of them was built. No
    // production behavior depends on it. Distinct from CameraStream::constructed_count().
    uint64_t reload_invocations() const { return reload_invocations_; }

    // Test-only: the current grid generation. Every authoritative teardown
    // (clear()) advances it, so a worker callback captured before a rebuild is
    // dropped by callback_is_current(). Consumed by the Slice-8 teardown proof.
    uint64_t generation() const { return generation_; }

    // Test-only: whether any live capture stream currently exists. Used by the
    // teardown-seam proof to assert nothing streams after teardown().
    bool has_live_streams() const { return !streams_.empty(); }

protected:
    void resizeEvent(QResizeEvent* event) override;
    void paintEvent(QPaintEvent* event) override;  // black letterbox margins

private:
    void clear();             // stop + delete all streams and tiles
    /// Poll the ONE runtime projection, route it per camera, and repaint only the
    /// tiles whose rows actually changed. GUI thread only.
    void poll_zone_runtime();
    void relayout_letterbox();  // centre the tile block as one 16:9-per-tile wall
    // Build the processor + stream and start. `det` is the camera's ALREADY-RESOLVED
    // detection config, passed in rather than re-queried: resolving it runs the
    // compatibility policy, which hashes every attached artifact (a multi-MB
    // TensorRT plan) on the GUI thread. reload() has just computed it, so taking it
    // as a parameter removes a second full hash per camera from the boot path.
    void start_one(const camera::Camera& cam, CameraTile* tile,
                   const detection::CameraDetection& det);
    /// The ball_leveler build path, branched at the SUBSYSTEM level rather than
    /// inside start_one(). reload() builds ZoneHealth and the zone/brazing
    /// reporters before any per-camera work, and start_one() reads camera_area
    /// and wires zone plumbing; branching only at processor construction would
    /// make Ball Leveler inherit digit quarantine and zone-reporting semantics it
    /// has no use for. This path builds NO ZoneHealth, NO ZoneReporter, NO
    /// BrazingReporter, reads NO camera_area and constructs NO DetectionProcessor.
    void reload_ball();
    /// One ball_leveler camera: resolve its stored configuration, decide its
    /// state, and build the matching processor. A failure here is confined to
    /// this camera - siblings are untouched.
    void start_one_ball(const camera::Camera& cam, CameraTile* tile);
    void on_model_ready(const QString& filename);
    void on_warmup_finished();
    void refresh_tile_inhibit(int64_t camera_id);  // push a camera's causes to its tile
    /// Rewrite status.json: boot verdict + runtime camera causes + the current
    /// held/inhibited zone picture + any owed inhibit onsets. Reports the write's
    /// OUTCOME to `zone_status_`, so a failed write stays owed and is retried.
    void refresh_status_file();
    /// Drain the aggregator's inhibit escalations, log each exactly once, and
    /// hand them to `zone_status_` to be published. The ONE consume path —
    /// called from the 5 Hz poll and again at teardown, so both behave
    /// identically.
    void consume_zone_onsets();
    /// {held, inhibited} zone numbers from the runtime projection. GUI thread.
    std::pair<std::set<int>, std::set<int>> zone_status_projection() const;
    // Re-read the committed mode, the production manifest view and the measured
    // platform. Called ONCE per build path, so every camera in one grid generation
    // is judged against the SAME facts — the readiness verdict, the runtime
    // resolution and the engine requests cannot drift apart mid-reload.
    void refresh_compatibility_inputs();

    QSqlDatabase db_;
    QGridLayout* grid_ = nullptr;
    std::vector<CameraStream*> streams_;
    std::vector<CameraTile*> tiles_;
    std::shared_ptr<EngineRegistry> engines_;
    WarmupState* warmup_ = nullptr;   // per-model warm readiness (not owned)
    PendingStart pending_;            // detection cams waiting on their models
    // Data needed to build a pending camera's stream once its models are ready.
    // The resolved detection config is carried along so the deferred start does
    // not re-run resolution (and re-hash every artifact) a second time.
    struct PendingCam {
        camera::Camera cam;
        CameraTile* tile;
        detection::CameraDetection det;
    };
    std::map<int64_t, PendingCam> pending_cams_;
    std::unique_ptr<BrazingReporter> brazing_reporter_;  // GUI-thread reliable sender
    std::unique_ptr<ZoneReporter> reporter_;             // shared ZoneSink (machine)
    // Per-camera inhibit cause owner (GUI thread, no mutex). Drives the reporter
    // gate, the tile banners, and status.json. Rebuilt each reload().
    std::unique_ptr<health::ZoneHealth> health_;
    health::IntegrityVerdict verdict_;                   // boot readiness (per reload)
    // Compatibility inputs for this grid generation (see refresh_compatibility_
    // inputs). ManifestView has no default state to fall back on — it is either
    // loaded or absent — so it is held as an optional rather than given a
    // meaningless empty default that could be mistaken for "no manifest".
    denso::mode::TargetMode mode_ = denso::mode::TargetMode::DigitReader;
    std::optional<denso::models::ManifestView> view_;
    denso::models::PlatformInfo platform_;
    std::map<int64_t, CameraTile*> tiles_by_cam_;        // camera id -> its tile
    // Ball Leveler measuring processors by camera id. NON-OWNING: each is owned
    // by its CameraStream (as its FrameProcessor), so this map is cleared in
    // clear() alongside the streams that own them and can never outlive one.
    // It exists so a GUI-thread availability change (camera offline, a failed
    // inference streak) can reach the right camera's processor.
    std::map<int64_t, BallLevelProcessor*> level_procs_;
    // Ball cameras waiting on their engine to warm, so the tile shows the same
    // "Preparing model..." state a digit camera does instead of stalling the GUI
    // thread on a deserialize. Enrolled in the SAME PendingStart gate as the
    // digit path, so one coordinator decides when anything starts.
    struct PendingBall {
        camera::Camera cam;
        CameraTile* tile;
    };
    std::map<int64_t, PendingBall> pending_ball_;
    // Zone housekeeping tick. The VALUES do not travel through here — they are
    // drawn straight into each camera frame by the frame processor's annotation
    // step, which pulls this same projection on the capture thread. What is left
    // for this timer is the two things a frame cannot carry: draining the inhibit
    // onsets to the log, and keeping status.json current.
    QTimer* zone_timer_ = nullptr;
    // What has actually reached status.json, and what is still owed. Owns both
    // the 5 Hz write throttle and the buffer of drained-but-unpublished onsets;
    // neither advances on a write that failed.
    ZoneStatusPublication zone_status_;
    uint64_t last_applied_seq_ = 0;   // drop-stale guard on the snapshot sequence
    uint64_t generation_ = 0;         // bumped by clear(); guards stale worker callbacks
    uint64_t reload_invocations_ = 0; // test observable: reload() build-path entries
    int rows_ = 0;  // current grid dims (0 = empty); drives the letterbox aspect
    int cols_ = 0;
};

} // namespace denso::ui
