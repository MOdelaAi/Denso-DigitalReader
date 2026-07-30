#include "ui/camera/grid/camera_grid.h"

#include "brazing/config.h"
#include "camera/repo.h"
#include "detection/repo.h"
#include "brazing/brazing_client.h"
#include "brazing/brazing_reporter.h"
#include "camera/camera_stream.h"
#include "ui/camera/grid/camera_tile.h"
#include "camera/frame_processor.h"
#include "ui/camera/grid/grid_layout.h"
#include "brazing/zone_reporter.h"
#include "detection/engine_registry.h"
#include "health/status_file.h"
#include "mode/config.h"
#include "models/model_identity.h"   // load_manifest_view
#include "paths/paths.h"
#include "platform/platform_info.h"  // measured_platform_info
#include "ui/common/async_runner.h"  // post_to_gui
#include "ui/warmup_state.h"

#include <QCoreApplication>
#include <QColor>
#include <QDebug>
#include <QDir>
#include <QGridLayout>
#include <QPainter>
#include <QPaintEvent>
#include <QResizeEvent>
#include <QString>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <memory>
#include <set>

namespace denso::ui {

namespace {
constexpr int kMaxTiles = 4;
constexpr int kZonePollMs = 200;  // ~5 Hz — readable, not frame-accurate
constexpr double kTileAspect = 16.0 / 9.0;  // each cell is 16:9 (camera native)
}

CameraGrid::CameraGrid(QSqlDatabase db, std::shared_ptr<EngineRegistry> engines,
                       WarmupState* warmup, QWidget* parent)
    : QWidget(parent), db_(std::move(db)), engines_(std::move(engines)),
      warmup_(warmup) {
    grid_ = new QGridLayout(this);
    grid_->setContentsMargins(0, 0, 0, 0);
    grid_->setSpacing(0);  // flush tiles — no gap between feeds (CCTV wall)
    if (warmup_) {
        connect(warmup_, &WarmupState::model_ready, this, &CameraGrid::on_model_ready);
        connect(warmup_, &WarmupState::finished, this, &CameraGrid::on_warmup_finished);
    }
}

CameraGrid::~CameraGrid() { clear(); }

void CameraGrid::poll_zone_runtime() {
    if (!reporter_) {
        return;  // torn down between the last tick and this one
    }
    // ONE call, then group. The projection is already a value copy, so nothing
    // below holds the reporter's lock.
    std::map<int64_t, std::vector<ZoneRuntimeEntry>> by_camera;
    for (const ZoneRuntimeEntry& e : reporter_->runtime_view()) {
        by_camera[e.camera_id].push_back(e);
    }
    // Stable ordering so a tile's rows never jump around between ticks.
    for (auto& [camera_id, rows] : by_camera) {
        (void)camera_id;
        std::sort(rows.begin(), rows.end(),
                  [](const ZoneRuntimeEntry& a, const ZoneRuntimeEntry& b) {
                      return a.zone_no < b.zone_no;
                  });
    }

    for (const auto& [camera_id, tile] : tiles_by_cam_) {
        const auto it = by_camera.find(camera_id);
        const std::vector<ZoneRuntimeEntry> rows =
            (it == by_camera.end()) ? std::vector<ZoneRuntimeEntry>{} : it->second;

        // Repaint ONLY on change: at 5 Hz across four tiles an unconditional
        // update() would burn the compositor redrawing identical text.
        auto& last = last_zone_view_[camera_id];
        if (last == rows) {
            continue;
        }
        last = rows;
        if (rows.empty()) {
            tile->clear_zone_runtime_view();   // camera lost its zones
        } else {
            tile->set_zone_runtime_view(rows);
        }
    }

    // Forget cameras that no longer have a tile, so a rebuilt grid cannot
    // inherit a stale "unchanged" verdict and skip the first real update.
    for (auto it = last_zone_view_.begin(); it != last_zone_view_.end();) {
        it = (tiles_by_cam_.count(it->first) == 0) ? last_zone_view_.erase(it)
                                                   : std::next(it);
    }

    // ── The ALARM channel, distinct from the rendering above ──────────────────
    // Drain the escalations the aggregator raised since the last tick. The drain
    // is destructive, so a standing inhibit is announced once and this timer
    // cannot re-announce it however often it fires.
    consume_zone_onsets();

    // ── The STATE channel ────────────────────────────────────────────────────
    // status.json carries the held/inhibited picture, rewritten only when it
    // actually moves — at 5 Hz an unconditional write would be pointless disk
    // churn on a box that runs for months. An owed alarm forces the write
    // through regardless, and a write that FAILED stays owed, so the throttle
    // can never leave the file silently stale.
    if (health_ && zone_status_.needs_write(zone_status_projection())) {
        refresh_status_file();
    }
}

std::pair<std::set<int>, std::set<int>> CameraGrid::zone_status_projection() const {
    std::pair<std::set<int>, std::set<int>> out;   // {held, inhibited}
    if (!reporter_) {
        return out;
    }
    for (const ZoneRuntimeEntry& e : reporter_->runtime_view()) {
        if (e.state == ZoneDisplayState::HoldingLastValid) {
            out.first.insert(e.zone_no);
        } else if (e.state == ZoneDisplayState::Inhibited) {
            out.second.insert(e.zone_no);
        }
    }
    return out;
}

void CameraGrid::consume_zone_onsets() {
    if (!reporter_) {
        return;
    }
    // ONE consumption. Everything downstream is driven by what this returns, so
    // "logged once" and "published once" cannot drift apart.
    const std::vector<ZoneInhibitOnset> drained = reporter_->take_newly_inhibited();
    if (drained.empty()) {
        return;   // nothing to say — leave status.json untouched
    }
    std::vector<health::ZoneInhibitRecord> onsets;
    onsets.reserve(drained.size());
    for (const ZoneInhibitOnset& o : drained) {
        // LOG FIRST, and unconditionally: the log is the durable record of the
        // alarm, so it is written before publication is even attempted. Two
        // integers and a fixed sentence — no URL, no credential, nothing that
        // needs redacting. This is the appliance's own reading having stopped;
        // it says nothing about any backend.
        qCritical().noquote()
            << "[zone] camera" << o.camera_id << "zone" << o.zone_no
            << "inhibited - no complete reading within the hold timeout";
        onsets.push_back(health::ZoneInhibitRecord{
            o.camera_id, o.zone_no, QStringLiteral("hold_timeout")});
    }
    // Hand them to the publication tracker rather than writing here. The drain
    // was destructive, so from now on that buffer is the only holder of these
    // alarms — it releases them only once a write has actually committed.
    zone_status_.enqueue(onsets);
}

void CameraGrid::clear() {
    // Advance the generation FIRST — before any stop/delete — so every callback
    // the workers we are about to tear down captured belongs to the now-stale
    // epoch. A queued WorkerFailedFn / status_changed that Qt still delivers to
    // this retained grid after the rebuild is then dropped by callback_is_current.
    ++generation_;
    // Stop the overlay poll FIRST: it reads reporter_ and writes to tiles, both
    // of which are destroyed below. Stopping here (not merely at destruction)
    // means no queued timeout can fire against a half-torn-down grid.
    if (zone_timer_) {
        zone_timer_->stop();
    }
    last_zone_view_.clear();
    // Drop the cached zone picture too: a rebuilt grid must not inherit an
    // "unchanged" verdict and skip publishing its first real zone state. Owed
    // alarms are deliberately NOT dropped — they have been logged but not yet
    // published, and a rebuild is not a reason to lose one.
    zone_status_.reset_published();
    // Drop any deferred-start bookkeeping; the tiles it references are deleted
    // just below, so the dangling pointers must not outlive this call.
    pending_ = PendingStart{};
    pending_cams_.clear();
    // Stop (join) every worker before deleting anything so no frame can land on
    // a destroyed tile; Qt then drops any queued events for the deleted objects.
    for (CameraStream* s : streams_) {
        s->stop();
        delete s;
    }
    streams_.clear();
    for (CameraTile* t : tiles_) {
        grid_->removeWidget(t);
        delete t;
    }
    tiles_.clear();
    tiles_by_cam_.clear();   // the tiles it pointed at were just deleted
    // Safe to tear down now: deleting each stream above destroyed the
    // FrameProcessor it owns, and ~DetectionProcessor joins its INFERENCE worker
    // — the thread that actually calls on_zones (capture threads never do). That
    // join, not the capture-thread stop, is what guarantees no worker can still
    // reach the reporter. Tear down the reporter, then the client it posts to.
    //
    // FINAL DRAIN, immediately before that teardown. The joins above mean no new
    // escalation can appear, and the reporter still exists, so an alarm raised
    // since the last timer tick is still reachable — after reporter_.reset() it
    // would be gone with no one having heard it. The same destructive drain the
    // poll uses, so nothing is reported twice; and it writes nothing when the
    // batch is empty, leaving an ordinary shutdown byte-for-byte as before.
    consume_zone_onsets();
    // Publish only when something is actually owed, so an ordinary shutdown
    // leaves status.json byte-for-byte as it was. If this write fails the alarms
    // stay owed: a reload republishes them, and the ball_leveler switch path
    // carries them in publish_idle_status(). At FINAL process destruction there
    // is no later moment to retry — the ceiling there is the rotating log, which
    // received every alarm first and unconditionally, before publication was even
    // attempted. That is the durable record; status.json is the convenience copy.
    if (health_ && !zone_status_.pending().empty()) {
        refresh_status_file();
    }
    reporter_.reset();
    brazing_reporter_.reset();
    // health_ last: its callback references reporter_. No worker can fire it now
    // (workers joined above), and its destructor raises no cause, so this is safe.
    health_.reset();
    last_applied_seq_ = 0;
    verdict_ = health::IntegrityVerdict{};
    rows_ = 0;
    cols_ = 0;
    grid_->setContentsMargins(0, 0, 0, 0);
    // Reset stretch so a previous larger layout doesn't leave empty tracks.
    for (int i = 0; i < 2; ++i) {
        grid_->setRowStretch(i, 0);
        grid_->setColumnStretch(i, 0);
    }
}

void CameraGrid::teardown() {
    // Delegate to the single authoritative teardown. The mode switch tears the
    // pipeline down BEFORE the reset transaction and must not rebuild anything, so
    // this is clear() with no reload() — one sequence, defined once (spec §6.2).
    clear();
}

void CameraGrid::publish_idle_status() {
    // Idle runtime status: no grid is streaming (the caller deliberately avoided
    // reload() — e.g. ball_leveler), but status.json must still carry the REAL
    // integrity verdict, never a default-constructed placeholder that would erase
    // real blockers/issues. Recompute it, then write with EMPTY runtime causes/zones
    // (nothing streams) and the committed mode + setup-required flag. For
    // ball_leveler mode_setup_required is permanently true (spec §2.1).
    refresh_compatibility_inputs();
    verdict_ = health::evaluate_integrity(db_, denso::paths::models_dir(), mode_,
                                          *view_, platform_);
    const auto m = denso::mode::load(db_);
    // Owed alarms ride along. This is the LAST writer on the ball_leveler switch
    // path — clear() has already drained and torn the reporter down, so if this
    // document went out without them, an escalation that was logged moments
    // earlier would be silently absent from status output. It also reports its
    // outcome, so a failed idle write leaves the alarms owed rather than eaten.
    const bool ok = health::write_status_file(
        denso::paths::status_file(),
        verdict_, {}, {}, {},
        QString::fromLatin1(denso::mode::to_string(m)),
        denso::mode::mode_setup_required(db_, m),
        zone_status_.pending());
    // Idle means nothing streams, so the published picture is empty by definition.
    zone_status_.on_write(ok, ZoneStatusPublication::Projection{});
    if (!ok) {
        qWarning().noquote()
            << "[zone] idle status.json write failed;"
            << zone_status_.pending().size() << "alarm(s) still owed";
    }
}

void CameraGrid::refresh_compatibility_inputs() {
    // The COMMITTED mode from the database — never a settings-page selector value,
    // which may hold an unconfirmed choice the operator has not applied.
    mode_ = denso::mode::load(db_);
    // The production manifest view (active backend bound inside ManifestView) and
    // the ONE shared measured-platform provider — the same seams boot and --check
    // use, so all three agree about what is declared and what device this is.
    view_.emplace(denso::models::load_manifest_view(denso::paths::models_dir()));
    platform_ = denso::platform::measured_platform_info();
}

void CameraGrid::reload() {
    // Monotonic build-path observable (test-only; no production behavior depends on
    // it). Incremented ONLY here, so an unchanged value across a CameraView::reload()
    // proves the grid never built a stream/processor/reporter/ZoneHealth.
    ++reload_invocations_;
    clear();

    // runtime(), not all(): an unfinished camera must never stream, and the
    // filter happens in SQL so it cannot eat one of the four tile slots below.
    std::vector<camera::Camera> cams = camera::runtime(db_);
    if (cams.size() > static_cast<size_t>(kMaxTiles)) {
        cams.resize(kMaxTiles);  // first four by id
    }
    if (cams.empty()) {
        return;
    }

    // Boot readiness verdict, computed once per reload: drives the per-camera
    // ModelUnavailable cause below and status.json. Read-only (spec §2). Judged
    // against the committed mode + production manifest view + measured platform,
    // resolved once here for the whole generation.
    refresh_compatibility_inputs();
    verdict_ = health::evaluate_integrity(db_, denso::paths::models_dir(), mode_,
                                          *view_, platform_);

    // Per-camera inhibit owner (GUI thread, no mutex). Any cause transition gates
    // the reporter, repaints the camera's tile, and rewrites status.json. Created
    // even without brazing so tiles + status.json still reflect faults.
    health_ = std::make_unique<health::ZoneHealth>(
        [this](int64_t camera_id, bool inhibited) {
            if (reporter_) reporter_->set_camera_inhibited(camera_id, inhibited);
            refresh_tile_inhibit(camera_id);
            refresh_status_file();
        });

    // Brazing zone reporting: when enabled, a single machine-wide ZoneReporter
    // collects every camera's assembled zones and POSTs the combined snapshot on
    // change. The reporter is called from capture threads; its callback hops to
    // the GUI thread (post_to_gui) where the BrazingReporter lives.
    const brazing::BrazingConfig bcfg = brazing::load(db_);
    // DELIVERY is optional; AGGREGATION is not. The ZoneReporter is always built
    // below, so zone values are computed, debounced, held and inhibited even with
    // no backend configured — the grid overlay is a LOCAL check and must not
    // depend on the server it exists to cross-check. Only the sender and this
    // callback are gated on configuration; an empty callback is a supported state
    // (ZoneReporter guards every publish with `if (snapshot && on_snapshot_)`).
    std::function<void(const std::map<int, int>&, uint64_t)> on_snapshot;
    if (bcfg.enabled && !bcfg.base_url.empty()) {
        brazing_reporter_ = std::make_unique<BrazingReporter>(
            std::make_unique<BrazingClient>(bcfg.base_url));
        BrazingReporter* reporter = brazing_reporter_.get();
        on_snapshot =
            [this, reporter](const std::map<int, int>& snap, uint64_t seq) {
                // Marshal to `reporter` (not `this`) so Qt drops the queued call if
                // the BrazingReporter is torn down; `reporter` is owned by this
                // grid, so `this` (for last_applied_seq_) is valid whenever it runs.
                common::post_to_gui(reporter, [this, reporter, snap, seq] {
                    // Callbacks fire outside the reporter mutex and marshal from
                    // several threads, so an older eviction can overtake a newer
                    // recovery. Drop the stale one rather than let whole-snapshot
                    // latest-wins clobber the recovery (spec §3.3d).
                    if (seq <= last_applied_seq_) return;
                    last_applied_seq_ = seq;
                    reporter->submit(snap);
                });
            };
    }
    // ALWAYS constructed — see above. With no backend configured this holds an
    // empty callback and simply publishes nowhere.
    reporter_ = std::make_unique<ZoneReporter>(std::move(on_snapshot));

    // Overlay polling. Parented to `this`, so it dies with the grid even if a
    // teardown path ever forgets it; clear() stops it explicitly BEFORE the
    // reporter is destroyed, so no tick can outlive what it reads.
    if (!zone_timer_) {
        zone_timer_ = new QTimer(this);
        zone_timer_->setInterval(kZonePollMs);
        connect(zone_timer_, &QTimer::timeout, this, &CameraGrid::poll_zone_runtime);
    }
    zone_timer_->start();

    const GridDims dims = grid_dims(static_cast<int>(cams.size()));
    for (int i = 0; i < static_cast<int>(cams.size()); ++i) {
        const camera::Camera& cam = cams[static_cast<size_t>(i)];

        auto* tile = new CameraTile(QString::fromStdString(cam.name));
        std::vector<camera::CameraArea> areas = camera::areas_for(db_, cam.id);
        tile->set_areas(areas);  // ROI overlay (if any)

        // Display ownership comes from CONFIGURATION, not from observations: a
        // camera inhibited before any frame arrives (the quarantine case) never
        // reaches the reporter's observation-derived map, so its zones could
        // otherwise never render as Paused. This map routes the overlay only —
        // eviction still keys off what was actually published (spec §3.3b).
        std::set<int> configured_zones;
        for (const camera::CameraArea& a : areas) {
            if (a.zone && *a.zone != 0) {  // 0 / unset == ROI-only, never reported
                configured_zones.insert(*a.zone);
            }
        }
        reporter_->set_configured_zones(cam.id, std::move(configured_zones));

        grid_->addWidget(tile, i / dims.cols, i % dims.cols);
        tiles_.push_back(tile);
        tiles_by_cam_[cam.id] = tile;

        // Install the static causes BEFORE the camera can publish (spec §8): the
        // tile is now in tiles_by_cam_, so the cause transition paints it. A
        // renumbered/quarantined ROI (areas_need_review) and a camera whose engine
        // is missing from disk (per-zone EngineMissing issue) both boot inhibited.
        health_->set_cause(cam.id, health::ZoneCause::AreasNeedReview,
                           cam.areas_need_review);
        for (const auto& iss : verdict_.issues) {
            if (iss.camera_id != cam.id) continue;
            // NO new ZoneCause bit (spec §7.3): a model the policy rejected and a
            // model missing from disk are the SAME thing to this camera — it has
            // no usable model. The cause bitmask is a file format, so the
            // semantically correct existing bit is reused and the distinct
            // diagnosis survives in the issue's reason code + detail.
            if (iss.kind == health::ZoneIssue::Kind::EngineMissing ||
                iss.kind == health::ZoneIssue::Kind::ModelCompatibilityRejected) {
                health_->set_cause(cam.id, health::ZoneCause::ModelUnavailable, true);
            }
        }

        const detection::CameraDetection det =
            detection::detection_for(db_, cam.id, mode_, *view_, platform_);
        // No detection (or no warm-up coordinator): go straight to start_one,
        // exactly as before — it handles the orientation/detection selection.
        //
        // A compatibility-rejected camera resolves to an EMPTY model set too, so it
        // takes this same branch and start_one refuses it there — ONE refusal
        // point, not two. `compatibility_rejected` is named explicitly so the
        // intent survives, and so a future change that let the flag arrive with a
        // non-empty set could not silently fall through. What it must never do is
        // reach the warm-up gate below: there is nothing to wait for, and enrolling
        // it in pending_ would park the tile on "Preparing model…" forever.
        if (det.compatibility_rejected || det.models.empty() || warmup_ == nullptr) {
            start_one(cam, tile, det);
            continue;
        }
        // Which of this camera's models are not yet warm?
        std::vector<std::string> waiting;
        for (const detection::ResolvedModel& rm : det.models) {
            if (!warmup_->is_ready(rm.filename)) {
                waiting.push_back(rm.filename);
            }
        }
        if (waiting.empty()) {
            start_one(cam, tile, det);  // all models already warm → cache-hit get()
        } else {
            tile->set_preparing(true);
            pending_cams_[cam.id] = PendingCam{cam, tile, det};
            pending_.add(cam.id, std::move(waiting));
        }
    }
    for (int r = 0; r < dims.rows; ++r) grid_->setRowStretch(r, 1);
    for (int c = 0; c < dims.cols; ++c) grid_->setColumnStretch(c, 1);
    rows_ = dims.rows;
    cols_ = dims.cols;
    relayout_letterbox();
    // Write status.json once now that every boot cause is installed, even if none
    // fired (a healthy machine still publishes a "ready" file for SSH inspection).
    refresh_status_file();
    // Streams are created + started per camera in start_one (now or as models
    // warm), so there is no batch start here.
}

void CameraGrid::start_one(const camera::Camera& cam, CameraTile* tile,
                           const detection::CameraDetection& det) {
    // ROI quarantine: after a view-significant source/geometry edit the camera's
    // areas may no longer align with the frame, so they are excluded from ROI
    // filtering and zone reporting is PAUSED until the operator re-verifies them
    // (Areas → "Verify & save" clears the flag). Feeding stale geometry could POST
    // wrong brazing zone numbers — so under review we pass NO areas and NO zone
    // sink (see below), and flag the tile.
    const bool review = cam.areas_need_review;
    std::vector<camera::CameraArea> areas =
        review ? std::vector<camera::CameraArea>{} : camera::areas_for(db_, cam.id);
    if (review) {
        qWarning().noquote() << "[camera] camera" << cam.id
                             << "— areas need review; zone reporting paused";
    }
    // ── Camera-scoped compatibility inhibition (spec §7.2) ───────────────────
    // At least one ATTACHED model is rejected by the central policy, so this
    // camera is inhibited AS A WHOLE — before any pipeline exists. Deliberately
    // NOT demoted to an OrientationProcessor: a demotion would look like a working
    // camera that has quietly stopped reading, which is the failure this design
    // exists to prevent. No DetectionProcessor is constructed and engines_->get()
    // is never called, so an incompatible plan is never deserialized and the
    // fail-loud TrtEngine ctor is never the thing that discovers the problem.
    // The ModelUnavailable cause was already installed in reload(), so the tile
    // shows the inhibit banner and the reporter has evicted this camera's zones.
    if (det.compatibility_rejected) {
        qCritical().noquote()
            << "[camera] camera" << cam.id
            << "— attached model rejected by the compatibility policy ("
            << QString::fromStdString(det.policy_reason)
            << "); camera inhibited, not started";
        tile->set_preparing(false);
        tile->set_status(static_cast<int>(CameraStream::Status::Offline));
        return;
    }

    std::unique_ptr<FrameProcessor> proc;
    if (det.models.empty()) {
        proc = std::make_unique<OrientationProcessor>(
            static_cast<int>(cam.rotation), cam.pitch, cam.roll);
    } else {
        // Engine-construction firewall. engines_->get() builds the native TensorRT
        // engine on Linux, whose ctor THROWS on a missing/bad <engine>.names.json
        // sidecar or an invalid engine. start_one() runs on the GUI thread — often
        // from a Qt slot (on_model_ready / on_warmup_finished) — so an escaping
        // exception crosses the event loop and std::terminates the whole app
        // (this is exactly the field core-dump we saw). Fail loud but survivable:
        // log, show the tile Offline, and skip this camera — never a fake
        // orientation-only stream that would silently hide the missing detection.
        try {
            std::vector<DetectionProcessor::ModelRun> runs;
            for (const detection::ResolvedModel& rm : det.models) {
                InferenceEngine* eng = engines_->get(rm.filename);  // may throw (TRT)
                if (!eng) continue;  // model failed to load — skip it
                runs.push_back({eng, rm.class_names, rm.classes});
            }
            if (runs.empty()) {
                proc = std::make_unique<OrientationProcessor>(
                    static_cast<int>(cam.rotation), cam.pitch, cam.roll);
            } else {
                proc = std::make_unique<DetectionProcessor>(
                    static_cast<int>(cam.rotation), cam.pitch, cam.roll,
                    std::move(runs), std::move(areas), cam.id,
                    /*ReadingSink*/ nullptr,
                    /*ZoneSink*/ review ? nullptr : reporter_.get(),
                    // Consecutive inference failures inhibit the camera. The handler
                    // fires on the INFERENCE WORKER THREAD, so marshal to the GUI
                    // before touching ZoneHealth (single-threaded by design).
                    /*WorkerFailedFn*/ [this, id = cam.id, gen = generation_](int64_t, bool failed) {
                        common::post_to_gui(this, [this, id, failed, gen] {
                            // Drop a callback from a torn-down generation: after a
                            // rebuild the same retained grid must not be inhibited
                            // by a worker that no longer streams.
                            if (!health_ || !camera::callback_is_current(gen, generation_)) return;
                            health_->set_cause(
                                id, health::ZoneCause::InferenceWorkerFailed, failed);
                        });
                    });
            }
        } catch (const std::exception& e) {
            qCritical().noquote()
                << "[camera] camera" << cam.id
                << "— detection model failed to load, camera not started:" << e.what();
            tile->set_preparing(false);
            tile->set_status(static_cast<int>(CameraStream::Status::Offline));
            return;
        }
    }
    tile->set_preparing(false);
    // The inhibit banner is driven by health_ causes (AreasNeedReview was set in
    // reload()), not set here — set_inhibited superseded set_review_paused.
    auto* stream = new CameraStream(cam, std::move(proc));
    connect(stream, &CameraStream::frame_ready, tile, &CameraTile::set_frame);
    connect(stream, &CameraStream::status_changed, tile, &CameraTile::set_status);
    // Camera-offline is a CAMERA-level cause: it drops the whole observation and
    // evicts the camera's zones immediately. Receiver is this grid (GUI thread) so
    // the AutoConnection is queued from the capture thread — the queued delivery is
    // what keeps ZoneHealth single-owner (mutex-free); a DirectConnection here
    // would break that invariant.
    connect(stream, &CameraStream::status_changed, this,
            [this, id = cam.id, gen = generation_](int s) {
                // Generation-guarded: an old-generation status_changed is dropped
                // even if Qt happened to deliver it after the grid rebuilt.
                if (!health_ || !camera::callback_is_current(gen, generation_)) return;
                health_->set_cause(
                    id, health::ZoneCause::CaptureOffline,
                    s == static_cast<int>(CameraStream::Status::Offline));
            });
    tile->set_frame_counter(stream->frame_counter());
    streams_.push_back(stream);
    stream->start();
}

void CameraGrid::on_model_ready(const QString& filename) {
    const std::vector<int64_t> ids = pending_.ready(filename.toStdString());
    for (int64_t id : ids) {
        auto it = pending_cams_.find(id);
        if (it == pending_cams_.end()) continue;
        start_one(it->second.cam, it->second.tile, it->second.det);
        pending_cams_.erase(it);
    }
}

void CameraGrid::on_warmup_finished() {
    // Any camera still waiting has a model that never loaded → start with whatever
    // resolved (start_one falls back to OrientationProcessor when no model loads).
    for (int64_t id : pending_.drain()) {
        auto it = pending_cams_.find(id);
        if (it == pending_cams_.end()) continue;
        start_one(it->second.cam, it->second.tile, it->second.det);
        pending_cams_.erase(it);
    }
}

void CameraGrid::refresh_tile_inhibit(int64_t camera_id) {
    const auto it = tiles_by_cam_.find(camera_id);
    if (it == tiles_by_cam_.end()) return;
    it->second->set_inhibited(health_ ? health_->causes(camera_id) : 0u);
}

void CameraGrid::refresh_status_file() {
    if (!health_) return;
    // Reuse the boot verdict (per-zone INTEGRITY issues do not change at
    // runtime); the runtime camera causes and the zone picture do move. The
    // held/inhibited lists finally have their producer: they are derived from the
    // ONE authority's projection, so they are idempotent and any number of
    // rewrites is safe. `verdict_` is not touched — a runtime alarm is reported
    // as its own record and never rewrites the installation's readiness. The DB
    // is open here, so the real mode + setup-required flag ride along
    // (nullopt-omitted on query fail, always true for ball_leveler).
    const auto zones = zone_status_projection();
    const auto m = denso::mode::load(db_);
    const bool ok = health::write_status_file(
        denso::paths::status_file(),
        verdict_, health_->all(), zones.first, zones.second,
        QString::fromLatin1(denso::mode::to_string(m)),
        denso::mode::mode_setup_required(db_, m),
        zone_status_.pending());
    // The write can fail (unopenable path, full disk) and QSaveFile leaves the
    // previous file intact when it does. Report the OUTCOME, never the intent:
    // only a committed write may advance the throttle or release an owed alarm.
    zone_status_.on_write(ok, zones);
    if (!ok) {
        qWarning().noquote()
            << "[zone] status.json write failed; retrying on the next tick ("
            << zone_status_.pending().size() << "alarm(s) still owed)";
    }
}

void CameraGrid::release_streams() {
    for (CameraStream* s : streams_) {
        s->stop();
    }
}

void CameraGrid::start_streams() {
    for (CameraStream* s : streams_) {
        s->start();
    }
}

void CameraGrid::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    relayout_letterbox();
}

void CameraGrid::paintEvent(QPaintEvent*) {
    // The tiles cover the centred block; this fills the letterbox margins around
    // it with black so the wall reads as one framed 16:9 surface.
    QPainter p(this);
    p.fillRect(rect(), QColor(0, 0, 0));
}

void CameraGrid::relayout_letterbox() {
    if (rows_ <= 0 || cols_ <= 0) {
        return;  // no cameras → nothing to centre
    }
    // Largest rect whose per-tile aspect is 16:9 that fits the widget, centred.
    // Block aspect = (cols·16) : (rows·9); solve for the block, then split the
    // leftover space into equal margins the grid layout reserves on each side.
    const double block_aspect = (cols_ * kTileAspect) / rows_;
    const double avail_w = width();
    const double avail_h = height();
    double block_w = avail_w;
    double block_h = avail_w / block_aspect;
    if (block_h > avail_h) {
        block_h = avail_h;
        block_w = avail_h * block_aspect;
    }
    const int mx = std::max(0, static_cast<int>(std::lround((avail_w - block_w) / 2.0)));
    const int my = std::max(0, static_cast<int>(std::lround((avail_h - block_h) / 2.0)));
    grid_->setContentsMargins(mx, my, mx, my);
}

} // namespace denso::ui
