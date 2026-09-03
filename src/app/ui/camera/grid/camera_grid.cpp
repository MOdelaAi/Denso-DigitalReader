#include "ui/camera/grid/camera_grid.h"

#include "brazing/config.h"
#include "brazing/url.h"        // normalize_base_url — ONE URL authority
#include "logging/redact.h"     // sanitize_url (never log a raw address)
#include "camera/repo.h"
#include "detection/repo.h"
#include "brazing/brazing_client.h"
#include "brazing/brazing_reporter.h"
#include "camera/camera_stream.h"
#include "ui/camera/grid/camera_tile.h"
#include "camera/frame_processor.h"
#include "camera/level_processor.h"
#include "level/calibration.h"
#include "level/repo.h"
#include "camera/source_change.h"   // view_revision
#include "models/compatibility.h"
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
#include <QSignalBlocker>
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

CameraGrid::~CameraGrid() {
    // clear() now reports a status transition, and a listener reached during our
    // own destruction would be handed a half-destroyed grid (and, in the widget
    // hierarchy, a half-destroyed parent). A status nobody can act on is worth
    // nothing at teardown, so drop it rather than deliver it late.
    const QSignalBlocker block(this);
    clear();
}

void CameraGrid::poll_zone_runtime() {
    if (!reporter_) {
        return;  // torn down between the last tick and this one
    }
    // The zone VALUES are no longer routed to the tiles from here: they are drawn
    // into the camera frame itself by the annotation step in the frame processor
    // (camera/zone_overlay.h), which reads this same projection on the capture
    // thread through a per-camera ZoneViewFn. That is the ONE visible annotation.
    // This timer keeps the two channels the frame cannot carry:

    // ── The ALARM channel ─────────────────────────────────────────────────────
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
    // Drop the cached zone picture: a rebuilt grid must not inherit an
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
    // Cleared with the streams, not after: every entry pointed INTO a stream we
    // just deleted, so leaving them would be a map of dangling pointers for the
    // duration of the rest of this function.
    level_procs_.clear();
    pending_ball_.clear();
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
    // The BACKEND SENDER deliberately survives this. clear() is the teardown of
    // the CAMERA pipeline, and a camera rebuild (closing the wizard, Refresh
    // Cameras) is not a reason to throw away a snapshot the server has not acked
    // yet, nor to churn the top-bar indicator. Retiring the sender is a
    // MODE-SWITCH act, so it lives in teardown() — the only caller that must not
    // rebuild — which keeps this the one camera-teardown sequence while letting
    // the two callers differ where they genuinely differ (spec 6.6).
    //
    // Nothing can submit in the gap: every capture and inference worker was
    // joined above, and reload() re-runs apply_brazing_config() before any new
    // worker starts.
    // health_ last: its callback references reporter_. No worker can fire it now
    // (workers joined above), and its destructor raises no cause, so this is safe.
    health_.reset();
    last_applied_seq_ = 0;
    admitted_count_ = 0;
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
    // …and THEN retire the backend sender, which clear() deliberately leaves
    // alone. This is the one path that must destroy it: the old mode's readings
    // must never reach the server after the switch (spec §6.6), so an un-acked
    // snapshot dies here — logged by ~BrazingReporter, as it always was.
    brazing_reporter_.reset();
    clear_active_brazing_identity();
    set_brazing_status(BrazingStatus::Off);
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

    // SUBSYSTEM-level mode branch. Read the COMMITTED mode from the database -
    // never a settings-page selector, which may hold an unapplied choice. Taken
    // before any digit machinery exists, so a ball_leveler appliance constructs
    // no ZoneHealth, no reporter and no DetectionProcessor at all.
    if (denso::mode::load(db_) == denso::mode::TargetMode::BallLeveler) {
        reload_ball();
        return;
    }

    // runtime(), not all(): an unfinished camera must never stream, and the
    // filter happens in SQL so it cannot eat one of the four tile slots below.
    std::vector<camera::Camera> cams = camera::runtime(db_);
    if (cams.size() > static_cast<size_t>(kMaxTiles)) {
        cams.resize(kMaxTiles);  // first four by id
    }
    admitted_count_ = cams.size();
    if (cams.empty()) {
        // No pipeline to feed, so no sender may survive the rebuild. This is the
        // branch apply_brazing_config() handles first (`!reporter_`).
        apply_brazing_config();
        return;
    }

    // Boot readiness verdict, computed once per reload: drives the per-camera
    // ModelUnavailable cause below and status.json. Read-only (spec §2). Judged
    // against the committed mode + production manifest view + measured platform,
    // resolved once here for the whole generation.
    refresh_compatibility_inputs();
    verdict_ = health::evaluate_integrity(db_, denso::paths::models_dir(), mode_,
                                          *view_, platform_);

    build_zone_reporting(kStableFrames, kHoldTimeoutMs);

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
            if (a.zone) {  // NULL == ROI-only; v17 left no zero to filter out
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

void CameraGrid::set_engines(std::shared_ptr<EngineRegistry> engines,
                             WarmupState* warmup) {
    if (!streams_.empty()) {
        // A live inference worker holds a raw InferenceEngine* owned by the
        // OUTGOING registry. Swapping underneath it would leave that pointer
        // dangling the moment the old registry is released. The switch tears the
        // grid down first, so reaching here means an ordering invariant broke -
        // refuse loudly rather than corrupt a running pipeline.
        qCritical().noquote()
            << "[mode] refusing to replace the engine registry while"
            << streams_.size() << "stream(s) are live; the grid must be torn"
            << "down first";
        return;
    }
    // Drop the OLD warm-up subscriptions before adopting the new coordinator, so
    // a late model_ready from the previous mode cannot start a camera in this one.
    if (warmup_) {
        disconnect(warmup_, nullptr, this, nullptr);
    }
    engines_ = std::move(engines);
    warmup_ = warmup;
    // drain() is the existing remove-everything primitive; the returned ids are
    // discarded because the OLD mode has no camera left to start.
    (void)pending_.drain();
    pending_cams_.clear();
    pending_ball_.clear();
    if (warmup_) {
        connect(warmup_, &WarmupState::model_ready, this, &CameraGrid::on_model_ready);
        connect(warmup_, &WarmupState::finished, this, &CameraGrid::on_warmup_finished);
    }
}

void CameraGrid::build_zone_reporting(int stable_frames, int64_t hold_timeout_ms) {
    // ONE construction site for the whole zone-reporting subsystem, shared by
    // BOTH modes. It exists as a function precisely so the two reload paths
    // cannot drift into building different pipelines for the same job: whatever
    // the digit reader gets, the Ball Leveler gets, and the ONLY difference
    // between them is the two aggregator parameters passed in here (amendment
    // §10.4). A Ball-specific copy of this block would be the second reporting
    // authority the amendment forbids.

    // Per-camera inhibit owner (GUI thread, no mutex). Any cause transition gates
    // the reporter, repaints the camera's tile, and rewrites status.json. Created
    // even without brazing so tiles + status.json still reflect faults.
    health_ = std::make_unique<health::ZoneHealth>(
        [this](int64_t camera_id, bool inhibited) {
            if (reporter_) reporter_->set_camera_inhibited(camera_id, inhibited);
            refresh_tile_inhibit(camera_id);
            refresh_status_file();
        });

    // Brazing zone reporting: a single machine-wide ZoneReporter collects every
    // camera's assembled zones and, when a backend is configured, POSTs the
    // combined snapshot on change. The reporter is called from capture threads;
    // its callback hops to the GUI thread (post_to_gui) where the BrazingReporter
    // lives.
    //
    // The callback is installed UNCONDITIONALLY and resolves the sender at
    // DELIVERY time rather than capturing one at construction. That is what makes
    // the sender swappable while the appliance runs (apply_brazing_config): the
    // ZoneReporter — and with it every zone's debounce, hold and overlay state —
    // survives a Settings Save untouched. Baking the sender in was the reason a
    // Backend settings change previously needed a restart.
    //
    // It marshals to `this` (the grid outlives every sender it owns) and carries
    // the grid GENERATION it was created in. That generation check replaces the
    // lifetime guard the old "marshal to the BrazingReporter object" trick got for
    // free: a snapshot queued by the previous generation's workers must not be
    // submitted to a sender built by the rebuilt grid.
    const uint64_t gen = generation_;
    std::function<void(const std::map<int, ZoneValue>&, uint64_t)> on_snapshot =
        [this, gen](const std::map<int, ZoneValue>& snap, uint64_t seq) {
            common::post_to_gui(this, [this, gen, snap, seq] {
                if (!camera::callback_is_current(gen, generation_)) return;
                // No backend configured (or one just disabled): aggregation still
                // ran, there is simply nowhere to publish.
                if (!brazing_reporter_) return;
                // Callbacks fire outside the reporter mutex and marshal from
                // several threads, so an older eviction can overtake a newer
                // recovery. Drop the stale one rather than let whole-snapshot
                // latest-wins clobber the recovery (spec §3.3d).
                if (seq <= last_applied_seq_) return;
                last_applied_seq_ = seq;
                brazing_reporter_->submit(snap);
            });
        };
    // DELIVERY is optional; AGGREGATION is not. The ZoneReporter is always built,
    // so zone values are computed, debounced, held and inhibited even with no
    // backend configured — the grid overlay is a LOCAL check and must not depend
    // on the server it exists to cross-check.
    reporter_ = std::make_unique<ZoneReporter>(std::move(on_snapshot), stable_frames,
                                               std::function<int64_t()>{},
                                               hold_timeout_ms);

    // The sender itself, from the persisted configuration. Built through the SAME
    // entry point the Settings Save uses, so boot and a live reconfiguration
    // cannot construct different reporting stacks. Called after reporter_ exists
    // because a newly built sender resets that reporter's delivery baseline.
    apply_brazing_config();

    // Overlay polling. Parented to `this`, so it dies with the grid even if a
    // teardown path ever forgets it; clear() stops it explicitly BEFORE the
    // reporter is destroyed, so no tick can outlive what it reads.
    if (!zone_timer_) {
        zone_timer_ = new QTimer(this);
        zone_timer_->setInterval(kZonePollMs);
        connect(zone_timer_, &QTimer::timeout, this, &CameraGrid::poll_zone_runtime);
    }
    zone_timer_->start();
}

void CameraGrid::set_brazing_status(BrazingStatus status) {
    if (brazing_status_ == status) {
        return;   // no transition: repeated Saves and repeated acks stay silent
    }
    brazing_status_ = status;
    emit brazing_status_changed(brazing_status_);
}

void CameraGrid::apply_brazing_config() {
    // Aggregation must already exist for a sender to be worth anything: with no
    // ZoneReporter nothing produces snapshots, and a sender built here would just
    // be destroyed by the next clear(). The rebuilt grid picks the configuration
    // up itself, because build_zone_reporting() ends by calling this.
    if (!reporter_) {
        brazing_reporter_.reset();
        clear_active_brazing_identity();
        // …and SAY so. clear() no longer reports Off (the sender now survives an
        // ordinary rebuild), so this branch is the only thing standing between a
        // grid that ended up with no pipeline — every camera removed or disabled —
        // and a top bar still claiming readings are going out.
        set_brazing_status(BrazingStatus::Off);
        return;
    }

    const brazing::BrazingConfig bcfg = brazing::load(db_);
    // The SAME rule the Settings dialog validates with, previews with, and the
    // transport composes with. A rejected address or path is not repaired here —
    // reporting simply does not start, loudly, rather than posting somewhere the
    // operator did not ask for.
    const brazing::ApiPathResult api = brazing::normalize_api_path(bcfg.api_path);
    const brazing::BaseUrlResult url = brazing::normalize_base_url(
        bcfg.base_url, api.ok ? api.api_path : std::string(brazing::kDefaultApiPath));
    const bool want =
        bcfg.enabled && url.ok && !url.base_url.empty() && api.ok;

    if (!want) {
        if (bcfg.enabled && !url.ok) {
            qWarning().noquote()
                << "[brazing] reporting is enabled but the stored server address"
                << "is not usable, so no sender was started:"
                << QString::fromStdString(logging::sanitize_url(bcfg.base_url))
                << "-" << QString::fromStdString(url.error);
        }
        if (bcfg.enabled && !api.ok) {
            qWarning().noquote()
                << "[brazing] reporting is enabled but the stored reporting API"
                << "path is not usable, so no sender was started:"
                << QString::fromStdString(bcfg.api_path)
                << "-" << QString::fromStdString(api.error);
        }
        if (brazing_reporter_) {
            qInfo().noquote() << "[brazing] backend reporting stopped";
        }
        // Destroying the reporter is the whole stop: it deletes its single-shot
        // retry QTimer (child), the retry policy with every queued/undelivered
        // snapshot, and the BrazingClient with the QNetworkAccessManager that owns
        // any in-flight reply. The QPointer in BrazingReporter::apply() already
        // prevents a late done() from re-entering a destroyed reporter, so no old
        // callback can start another request.
        brazing_reporter_.reset();
        clear_active_brazing_identity();
        set_brazing_status(BrazingStatus::Off);
        return;
    }

    // The ONE composition site, so the sender's identity is literally the URL it
    // will post to — never a base that happens to match while the path moved.
    const std::string endpoint =
        brazing::endpoint_url(url.base_url, api.api_path);
    if (endpoint.empty()) {
        // Unreachable given the checks above (both halves are already known good),
        // but the composed endpoint is what the client would be built from, so
        // trusting it rather than re-deriving the decision keeps ONE authority.
        qWarning().noquote()
            << "[brazing] the configured server address and reporting API path"
            << "compose to no endpoint; no sender was started";
        brazing_reporter_.reset();
        clear_active_brazing_identity();
        set_brazing_status(BrazingStatus::Off);
        return;
    }

    if (brazing_reporter_ && active_brazing_endpoint_ == endpoint) {
        // Saving an unchanged configuration must be inert: no second reporter, no
        // duplicate POST, and no reset of retry/delivery state that would re-send
        // a value the server already has.
        return;
    }

    // Retire the OLD sender BEFORE constructing the replacement. Ordering is the
    // barrier: once this returns there is no timer left to fire, no policy left
    // holding a pending snapshot, and no QNAM left to carry a request to the old
    // address — so nothing can be sent to the old URL afterwards.
    brazing_reporter_.reset();
    brazing_reporter_ = std::make_unique<BrazingReporter>(
        std::make_unique<BrazingClient>(url.base_url, api.api_path));
    active_brazing_url_ = url.base_url;
    active_brazing_path_ = api.api_path;
    active_brazing_endpoint_ = endpoint;
    ++brazing_sender_builds_;

    // The indicator's Error state comes from the SENDER's own attempts, not from
    // any probe of our own: the backend exposes only the one reporting POST — no
    // health endpoint — so the outcome of a real report is the only evidence
    // there is. Connected to
    // the reporter we just built, so a retired sender's late outcome cannot move
    // the indicator (its connections die with it).
    connect(brazing_reporter_.get(), &BrazingReporter::delivery_succeeded, this,
            [this] { set_brazing_status(BrazingStatus::On); });
    connect(brazing_reporter_.get(), &BrazingReporter::delivery_failed, this,
            [this] { set_brazing_status(BrazingStatus::Error); });
    // A freshly built sender has attempted nothing, so it is On (running), never
    // "Connected" — nothing has been proved about the server yet.
    set_brazing_status(BrazingStatus::On);

    // A fresh sender knows nothing of what the previous one delivered, and the
    // aggregator would otherwise stay silent until a reading CHANGED. Drop the
    // last-sent baseline so the next snapshot that earns the existing stable-frame
    // bar is published once — no value invented, no debounce skipped.
    //
    // The returned sequence number is the RELOAD BARRIER, and raising
    // last_applied_seq_ to it completes the swap. Snapshots reach the GUI thread
    // as queued calls, so one published moments before this Save can still be in
    // the event queue; without the barrier it would be delivered to the
    // replacement sender, handing the new backend a payload that belonged to the
    // retired one. Reusing the existing drop-stale guard means there is ONE place
    // a snapshot is judged too old to deliver, not two.
    last_applied_seq_ =
        std::max(last_applied_seq_, reporter_->reset_delivery_baseline());
    // The composed endpoint, not a second concatenation of the two halves: the
    // log must name the URL the client was actually built with.
    qInfo().noquote() << "[brazing] backend reporting active:"
                      << QString::fromStdString(logging::sanitize_url(endpoint));
}

void CameraGrid::clear_active_brazing_identity() {
    active_brazing_url_.clear();
    active_brazing_path_.clear();
    active_brazing_endpoint_.clear();
}

void CameraGrid::reload_ball() {
    // Judged against the SAME facts for every camera in this generation: the
    // committed mode, the production manifest view and the measured platform.
    refresh_compatibility_inputs();
    verdict_ = health::evaluate_integrity(db_, denso::paths::models_dir(), mode_,
                                          *view_, platform_);

    // active(), not runtime(): setup_complete records that the DIGIT wizard
    // finished. An enabled but uncalibrated camera must appear on the wall as an
    // explicit Unconfigured state rather than be filtered out before it can be
    // seen.
    std::vector<camera::Camera> cams = camera::active(db_);
    if (cams.size() > static_cast<size_t>(kMaxTiles)) {
        cams.resize(kMaxTiles);   // first four by id
    }
    admitted_count_ = cams.size();
    if (cams.empty()) {
        apply_brazing_config();   // no pipeline -> no surviving sender
        publish_idle_status();
        return;
    }

    // The SAME zone-reporting subsystem the digit reader builds, differing only
    // in the two aggregator parameters (amendment §10.4):
    //   stable_frames  = 1 — a continuous quantized measurement will not
    //                        reliably repeat five identical integers, so the
    //                        digit debounce would mean Ball never publishes.
    //   hold_timeout_ms = 0 — a level measurement that stopped is not evidence
    //                        of the current level. Zero makes the hold window
    //                        empty, so the first non-Complete reading evicts the
    //                        value instead of republishing it for 30 s. This is
    //                        what carries Ball's "no old percentage is ever
    //                        reported as live" invariant THROUGH the shared
    //                        reporter rather than around it.
    build_zone_reporting(/*stable_frames=*/1, /*hold_timeout_ms=*/0);

    const GridDims dims = grid_dims(static_cast<int>(cams.size()));
    for (int i = 0; i < static_cast<int>(cams.size()); ++i) {
        const camera::Camera& cam = cams[static_cast<size_t>(i)];
        auto* tile = new CameraTile(QString::fromStdString(cam.name));
        // No set_areas(): ball_leveler reads no camera_area. ROI polygons are a
        // digit-reader concept and drawing them here would show geometry that
        // governs nothing. Ball zone geometry is burned into the frame by the
        // level overlay instead.
        grid_->addWidget(tile, i / dims.cols, i % dims.cols);
        tiles_.push_back(tile);
        tiles_by_cam_[cam.id] = tile;
        start_one_ball(cam, tile);
    }
    for (int r = 0; r < dims.rows; ++r) grid_->setRowStretch(r, 1);
    for (int c = 0; c < dims.cols; ++c) grid_->setColumnStretch(c, 1);
    rows_ = dims.rows;
    cols_ = dims.cols;
    relayout_letterbox();

    // The SAME status writer the digit path uses. Ball zones now flow through
    // the shared ZoneHealth + ZoneReporter, so status.json reports them the way
    // it reports digit zones — CameraGrid remains its single runtime writer.
    refresh_status_file();
}

void CameraGrid::start_one_ball(const camera::Camera& cam, CameraTile* tile) {
    tile->set_preparing(false);

    // A per-camera failure must never take a sibling down, so everything from
    // here is confined by this handler.
    const auto show_state = [this, &cam, tile](level::LevelState state) {
        auto proc = std::make_unique<LevelStateProcessor>(
            static_cast<int>(cam.rotation), cam.pitch, cam.roll, cam.id, state);
        auto* stream = new CameraStream(cam, std::move(proc));
        connect(stream, &CameraStream::frame_ready, tile, &CameraTile::set_frame);
        connect(stream, &CameraStream::status_changed, tile, &CameraTile::set_status);
        tile->set_frame_counter(stream->frame_counter());
        streams_.push_back(stream);
        stream->start();
    };

    // ── 1. The stored configuration. THREE outcomes, kept distinct ───────────
    // A failed QUERY is an infrastructure fault; a camera with no row is an
    // ordinary setup gap. Collapsing them would report a corrupt database as a
    // routine "not set up yet".
    const std::optional<std::optional<level::LevelConfig>> probe =
        level::try_level_config_for(db_, cam.id);
    if (!probe) {
        qCritical().noquote()
            << "[level] camera" << cam.id
            << "- could not read the ball calibration; camera not measuring";
        show_state(level::LevelState::Unavailable);
        return;
    }
    if (!*probe) {
        show_state(level::LevelState::Unconfigured);
        return;
    }
    const level::LevelConfig cfg = **probe;

    // ── 2. Does the calibration still describe THIS view? ────────────────────
    // Geometry is in oriented-frame coordinates, so a change to rotation /
    // pitch / roll / width / height / source makes it refer to a different
    // physical view. Do not measure against it - and do not delete it either.
    if (cfg.view_revision != camera::view_revision(cam)) {
        qWarning().noquote()
            << "[level] camera" << cam.id
            << "- the camera view changed since calibration; measurement paused";
        show_state(level::LevelState::CalibrationInvalid);
        return;
    }
    // A camera owns 1..kMaxBallZones zones. try_level_config_for already reports
    // the empty set as Unconfigured, so what is left to catch here is a set that
    // is too LARGE. The cap is a property of the SET, not of a row, so no CHECK
    // constraint can hold it and the write chokepoint's count test is the only
    // other place it exists — which means a restored backup or a hand-edited
    // database meets this check first and nowhere else.
    if (cfg.zones.size() > static_cast<size_t>(level::kMaxBallZones)) {
        qCritical().noquote()
            << "[level] camera" << cam.id << "- stored configuration has"
            << cfg.zones.size() << "zones, more than the" << level::kMaxBallZones
            << "permitted; camera not measuring";
        show_state(level::LevelState::CalibrationInvalid);
        return;
    }
    // EVERY zone must validate. A camera with one broken zone does not measure
    // its other three: the operator must see and fix the fault, and quietly
    // running the healthy zones would hide it behind a working display.
    for (const level::LevelZone& z : cfg.zones) {
        const auto check = level::validate_calibration(z.calibration);
        if (!check.ok) {
            qWarning().noquote()
                << "[level] camera" << cam.id << "zone" << z.zone_no
                << "- stored calibration is invalid ("
                << QString::fromStdString(check.reason_code)
                << "); measurement paused";
            show_state(level::LevelState::CalibrationInvalid);
            return;
        }
    }

    // ── 3. The bound model, through the ONE central policy ───────────────────
    // Never a filename or family test of our own: this path asks
    // models::model_compatibility for BallLeveler exactly as the write
    // chokepoint did, so a model that could not be SAVED can never be LOADED.
    std::string filename;
    for (const auto& row : detection::list_models(db_)) {
        if (row.id != cfg.model_id) continue;
        const denso::models::ModelMetadata md =
            denso::models::resolve_model_metadata(*view_, row, platform_);
        const auto verdict = denso::models::model_compatibility(
            denso::mode::TargetMode::BallLeveler, md);
        if (!verdict.allowed()) {
            qCritical().noquote()
                << "[level] camera" << cam.id << "- bound model rejected by the"
                << "compatibility policy ("
                << QString::fromStdString(verdict.reason_code)
                << "); camera not measuring";
            break;   // filename stays empty -> Unavailable below
        }
        // The bound CLASS must exist in the resolved metadata. The authority is
        // md.class_names — the canonical list resolved from the manifest/sidecar
        // for the ACTIVE backend — which is the same authority
        // save_level_configuration uses, so a binding that could not be SAVED
        // cannot be LOADED. Selecting a class the model cannot emit would not
        // fail loudly: the processor would find no ball in every frame and
        // report the tank as permanently incomplete rather than misconfigured.
        if (cfg.class_id < 0 ||
            static_cast<size_t>(cfg.class_id) >= md.class_names.size()) {
            qCritical().noquote()
                << "[level] camera" << cam.id << "- bound class" << cfg.class_id
                << "does not exist in" << QString::fromStdString(md.filename)
                << "; camera not measuring";
            break;   // filename stays empty -> Unavailable below
        }
        filename = md.filename;
        break;
    }
    if (filename.empty()) {
        show_state(level::LevelState::Unavailable);
        return;
    }

    // ── 3b. Wait for the engine to warm ──────────────────────────────────────
    // engines_->get() below DESERIALIZES the plan, and this runs on the GUI
    // thread. Enrol in the same warm-up gate the digit path uses so the tile
    // shows "Preparing model..." instead of the window freezing. is_complete()
    // is the terminating condition: once warm-up is done we fall through and
    // let get() report the truth (a failed model returns null -> Unavailable),
    // so a model that never warms cannot park the tile forever.
    if (warmup_ && !warmup_->is_ready(filename) && !warmup_->is_complete()) {
        tile->set_preparing(true);
        pending_ball_[cam.id] = PendingBall{cam, tile};
        pending_.add(cam.id, {filename});
        return;
    }

    // ── 3c. Warm-up finished WITHOUT this model ──────────────────────────────
    // get() is not a sufficient readiness test. EngineRegistry::get() caches the
    // engine it constructs BEFORE warm_up() runs the blank inference on it, so a
    // model whose warm-up inference threw is still cached and non-null - and a
    // second get() would hand it back and build a measuring pipeline on a plan
    // that failed to warm. Warm-up completing without marking this model ready is
    // the authoritative answer, so take it and never ask again.
    if (warmup_ && warmup_->is_complete() && !warmup_->is_ready(filename)) {
        qCritical().noquote()
            << "[level] camera" << cam.id
            << "- the level model did not warm up; camera not measuring";
        show_state(level::LevelState::Unavailable);
        return;
    }

    // ── 4. The engine ────────────────────────────────────────────────────────
    // engines_->get() constructs the native TensorRT engine, whose ctor THROWS
    // on a missing sidecar or an invalid plan. This runs on the GUI thread, so
    // an escaping exception would cross the event loop and terminate the app.
    // Fail loud but survivable: this camera reports Unavailable, its siblings
    // keep running.
    InferenceEngine* engine = nullptr;
    try {
        engine = engines_ ? engines_->get(filename) : nullptr;
    } catch (const std::exception& e) {
        qCritical().noquote() << "[level] camera" << cam.id
                              << "- level model failed to load:" << e.what();
        engine = nullptr;
    }
    if (!engine) {
        show_state(level::LevelState::Unavailable);
        return;
    }

    // ── 5. The measuring pipeline ────────────────────────────────────────────
    // Display ownership comes from CONFIGURATION, not from observations —
    // identically to the digit path. A camera inhibited before any frame arrives
    // never reaches the reporter's observation-derived map, so its zones could
    // otherwise never render as Paused.
    if (reporter_) {
        std::set<int> configured_zones;
        for (const level::LevelZone& z : cfg.zones) configured_zones.insert(z.zone_no);
        reporter_->set_configured_zones(cam.id, std::move(configured_zones));
    }

    // This camera's rows from the SHARED projection, read on the capture thread
    // once per displayed frame — the same ZoneViewFn seam and the same
    // mutex-guarded, copy-returning implementation the digit processors use. The
    // overlay renders the state the REPORTER decided; it does not compute a
    // second opinion.
    ZoneViewFn zone_view;
    if (reporter_) {
        ZoneReporter* rep = reporter_.get();
        const int64_t cid = cam.id;
        zone_view = [rep, cid] {
            std::vector<ZoneRuntimeEntry> mine;
            for (const ZoneRuntimeEntry& e : rep->runtime_view()) {
                if (e.camera_id == cid) mine.push_back(e);
            }
            return mine;
        };
    }

    // WorkerFailedFn now HAS a consumer: ZoneHealth, exactly as in the digit
    // path. It is marshalled to the GUI thread and routed to ZoneHealth — NOT
    // back into set_unavailable(), which is the CAMERA cause slot. Feeding it
    // there is what once re-coupled the two causes a queued "recovered" could
    // then wipe a live camera_offline with. Two causes, two owners.
    auto on_worker_failed = [this, id = cam.id, gen = generation_](int64_t camera_id,
                                                                   bool failed) {
        common::post_to_gui(this, [this, id, camera_id, gen, failed] {
            if (!camera::callback_is_current(gen, generation_)) return;
            if (camera_id != id) return;
            if (health_) {
                health_->set_cause(id, health::ZoneCause::InferenceWorkerFailed, failed);
            }
        });
    };

    auto proc = std::make_unique<BallLevelProcessor>(
        static_cast<int>(cam.rotation), cam.pitch, cam.roll, engine,
        cfg.class_id, cfg.zones, cam.id, reporter_.get(),
        std::move(on_worker_failed), std::move(zone_view));
    BallLevelProcessor* proc_raw = proc.get();

    auto* stream = new CameraStream(cam, std::move(proc));
    connect(stream, &CameraStream::frame_ready, tile, &CameraTile::set_frame);
    connect(stream, &CameraStream::status_changed, tile, &CameraTile::set_status);
    // Camera-offline CLEARS the live value rather than leaving the last reading
    // on screen. Receiver is this grid (GUI thread), so the AutoConnection is
    // queued from the capture thread; the processor's own mutex makes the write
    // safe against the capture thread that reads it while drawing.
    connect(stream, &CameraStream::status_changed, this,
            [this, id = cam.id, gen = generation_](int st) {
                if (!camera::callback_is_current(gen, generation_)) return;
                const auto it = level_procs_.find(id);
                if (it == level_procs_.end()) return;
                const bool offline =
                    st == static_cast<int>(CameraStream::Status::Offline);
                it->second->set_unavailable(
                    offline ? std::optional<std::string>(level::kReasonCameraOffline)
                            : std::nullopt);
                // The processor clearing its own measurements is only half of it:
                // the values already ACCEPTED by the machine-wide aggregator must
                // also go, or the backend keeps seeing this camera's last zone
                // values while it is dark. That eviction is ZoneHealth's job
                // through the shared reporter — the same one authority the digit
                // path uses, not a second eviction written here.
                if (health_) {
                    health_->set_cause(id, health::ZoneCause::CaptureOffline, offline);
                }
            });
    tile->set_frame_counter(stream->frame_counter());
    // Recorded only once the stream that OWNS the processor is in streams_, so
    // clear() can never leave this map holding a pointer into a deleted stream.
    streams_.push_back(stream);
    level_procs_[cam.id] = proc_raw;
    stream->start();
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

    // THIS camera's zone rows, read on the capture thread once per displayed
    // frame and drawn into the image by the frame processor. Bound to cam.id and
    // filtered on camera_id, so a frame can never carry a zone number belonging
    // to another camera even when two cameras share one. The reporter outlives
    // every stream (clear() deletes the streams, which joins their capture
    // threads, BEFORE reporter_.reset()), so this pointer cannot dangle.
    ZoneViewFn zone_view;
    if (reporter_) {
        ZoneReporter* rep = reporter_.get();
        const int64_t cid = cam.id;
        zone_view = [rep, cid] {
            std::vector<ZoneRuntimeEntry> mine;
            for (const ZoneRuntimeEntry& e : rep->runtime_view()) {
                if (e.camera_id == cid) mine.push_back(e);
            }
            // Stable ordering so the rows never jump between frames.
            std::sort(mine.begin(), mine.end(),
                      [](const ZoneRuntimeEntry& a, const ZoneRuntimeEntry& b) {
                          return a.zone_no < b.zone_no;
                      });
            return mine;
        };
    }

    std::unique_ptr<FrameProcessor> proc;
    if (det.models.empty()) {
        proc = std::make_unique<OrientationProcessor>(
            static_cast<int>(cam.rotation), cam.pitch, cam.roll, zone_view);
    } else {
        // Warm-up finished WITHOUT one of this camera's models — the digit
        // analogue of start_one_ball()'s step 3c, and reachable for the same
        // reason: settle_pending_after_warmup() drains BOTH build paths, so a
        // terminal warm-up FAILURE now reaches this one too.
        //
        // get() is not a readiness test. EngineRegistry caches the engine it
        // constructs BEFORE warm_up() runs the blank inference on it, so a plan
        // whose warm-up inference threw is still cached and still non-null.
        // Falling through would either build a DetectionProcessor on a plan that
        // never warmed, or — if the load returned null — quietly demote the
        // camera to orientation-only, which is exactly the silent hiding of a
        // missing detection this function refuses to do below.
        if (warmup_ && warmup_->is_complete()) {
            for (const detection::ResolvedModel& rm : det.models) {
                if (warmup_->is_ready(rm.filename)) {
                    continue;
                }
                qCritical().noquote()
                    << "[camera] camera" << cam.id << "— detection model"
                    << QString::fromStdString(rm.filename)
                    << "did not warm up, camera not started";
                // The documented cause for a rejected/unusable ATTACHED model.
                // Inhibits this camera's reporting only; siblings keep running.
                if (health_) {
                    health_->set_cause(cam.id, health::ZoneCause::ModelUnavailable,
                                       true);
                    refresh_tile_inhibit(cam.id);
                }
                tile->set_preparing(false);
                tile->set_status(static_cast<int>(CameraStream::Status::Offline));
                return;
            }
        }

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
                    static_cast<int>(cam.rotation), cam.pitch, cam.roll, zone_view);
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
                    },
                    // Drawn into the frame right after the detection boxes.
                    zone_view);
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
        if (it != pending_cams_.end()) {
            start_one(it->second.cam, it->second.tile, it->second.det);
            pending_cams_.erase(it);
            continue;
        }
        // A ball_leveler camera waiting on its Float engine. One gate, two build
        // paths - never two coordinators.
        auto bt = pending_ball_.find(id);
        if (bt != pending_ball_.end()) {
            const PendingBall pb = bt->second;
            pending_ball_.erase(bt);   // erase BEFORE the rebuild, so a re-enrol
            start_one_ball(pb.cam, pb.tile);   // cannot be wiped by this erase
        }
    }
}

void CameraGrid::settle_pending_after_warmup() {
    // Same drain as a normal completion: is_complete() is true for a FAILED
    // warm-up too, so each rebuilt camera falls through the gate and reports the
    // truth rather than re-enrolling and waiting forever.
    on_warmup_finished();
}

void CameraGrid::on_warmup_finished() {
    // Any camera still waiting has a model that never loaded → start with whatever
    // resolved (start_one falls back to OrientationProcessor when no model loads).
    for (int64_t id : pending_.drain()) {
        auto it = pending_cams_.find(id);
        if (it != pending_cams_.end()) {
            start_one(it->second.cam, it->second.tile, it->second.det);
            pending_cams_.erase(it);
            continue;
        }
        auto bt = pending_ball_.find(id);
        if (bt != pending_ball_.end()) {
            const PendingBall pb = bt->second;
            pending_ball_.erase(bt);
            // warm-up is complete now, so start_one_ball falls through the gate
            // and get() reports the truth for a model that never loaded.
            start_one_ball(pb.cam, pb.tile);
        }
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
