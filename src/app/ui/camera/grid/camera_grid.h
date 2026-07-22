// The live 1–4 camera grid. Reads up to the first four cameras, arranges a tile
// per camera (1 / 1×2 / 2×2 via grid_dims), and owns a CameraStream per tile.
// reload() rebuilds from the DB and starts streaming; release_streams() stops
// capture (e.g. so the Camera modal can grab the same USB device) without
// tearing down the tiles. Streaming stops on destruction.
#pragma once

#include "camera/callback_generation.h"
#include "camera/camera.h"
#include "camera/warmup_gate.h"
#include "detection/engine_registry.h"
#include "health/integrity.h"     // IntegrityVerdict
#include "health/zone_health.h"   // ZoneHealth, ZoneCause

#include <QSqlDatabase>
#include <QString>
#include <QWidget>

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

class QGridLayout;

namespace denso::ui {

class CameraStream;
class CameraTile;
class BrazingReporter;
class ZoneReporter;
class WarmupState;

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
    void relayout_letterbox();  // centre the tile block as one 16:9-per-tile wall
    void start_one(const camera::Camera& cam, CameraTile* tile);  // build proc+stream, start
    void on_model_ready(const QString& filename);
    void on_warmup_finished();
    void refresh_tile_inhibit(int64_t camera_id);  // push a camera's causes to its tile
    void refresh_status_file();                    // rewrite status.json (verdict + causes)

    QSqlDatabase db_;
    QGridLayout* grid_ = nullptr;
    std::vector<CameraStream*> streams_;
    std::vector<CameraTile*> tiles_;
    std::shared_ptr<EngineRegistry> engines_;
    WarmupState* warmup_ = nullptr;   // per-model warm readiness (not owned)
    PendingStart pending_;            // detection cams waiting on their models
    // Data needed to build a pending camera's stream once its models are ready.
    struct PendingCam {
        camera::Camera cam;
        CameraTile* tile;
    };
    std::map<int64_t, PendingCam> pending_cams_;
    std::unique_ptr<BrazingReporter> brazing_reporter_;  // GUI-thread reliable sender
    std::unique_ptr<ZoneReporter> reporter_;             // shared ZoneSink (machine)
    // Per-camera inhibit cause owner (GUI thread, no mutex). Drives the reporter
    // gate, the tile banners, and status.json. Rebuilt each reload().
    std::unique_ptr<health::ZoneHealth> health_;
    health::IntegrityVerdict verdict_;                   // boot readiness (per reload)
    std::map<int64_t, CameraTile*> tiles_by_cam_;        // camera id -> its tile
    uint64_t last_applied_seq_ = 0;   // drop-stale guard on the snapshot sequence
    uint64_t generation_ = 0;         // bumped by clear(); guards stale worker callbacks
    uint64_t reload_invocations_ = 0; // test observable: reload() build-path entries
    int rows_ = 0;  // current grid dims (0 = empty); drives the letterbox aspect
    int cols_ = 0;
};

} // namespace denso::ui
