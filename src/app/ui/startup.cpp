#include "ui/startup.h"

#include "detection/repo.h"
#include "detection/engine_registry.h"
#include "health/integrity.h"
#include "health/status_file.h"
#include "models/compatibility.h"
#include "models/manifest.h"
#include "models/model_identity.h"
#include "mode/config.h"
#include "mode/mode.h"
#include "paths/paths.h"
#include "platform/platform_info.h"
#include "ui/mainwindow.h"
#include "ui/startup_mode.h"
#include "ui/engine_session.h"
#include "ui/startup_screen.h"
#include "ui/warmup_state.h"
#include "ui/warmup_worker.h"

#include <QApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QThread>

#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace denso::ui {

namespace {

// Cold start: block behind the animated splash while every model warms on the
// worker, then build + show the window. Warm-up finishes before any capture
// thread exists, so the minutes-long TensorRT build never lands on one.
int launch_cold_with_splash(QApplication& app, QSqlDatabase db,
                            std::shared_ptr<settings::Settings> state,
                            std::shared_ptr<EngineRegistry> engines) {
    auto splash = std::make_unique<StartupScreen>(state->dark);
    splash->show();
    splash->raise();
    splash->activateWindow();  // claim the foreground at launch

    auto* thread = new QThread;
    auto* worker = new WarmupWorker(engines);
    worker->moveToThread(thread);

    // Built on the main thread once warm-up finishes; must outlive app.exec(),
    // so it lives in this scope and is populated by the finished handler.
    std::unique_ptr<MainWindow> window;

    QObject::connect(thread, &QThread::started, worker, &WarmupWorker::run);
    QObject::connect(worker, &WarmupWorker::progress, splash.get(),
                     &StartupScreen::set_status);
    // Engine-only, no fallback: a fatal warm-up failure aborts startup cleanly
    // (clear message, non-zero exit) instead of std::terminate on the worker.
    QObject::connect(worker, &WarmupWorker::failed, &app, [&app](const QString& err) {
        qCritical().noquote() << "[fatal] model warm-up failed:" << err;
        // Keep exit 1 (Jetson-verified fail-loud): the shipped systemd unit
        // documents "exit 1 = warm-up found a missing/invalid engine" and does not
        // restart on it (Restart=on-abnormal ignores any clean exit). EX_CONFIG is
        // reserved for the readiness-VERDICT paths (evaluate_db_schema / integrity
        // Blocked), which is where a distinct config-fault code earns its keep.
        app.exit(1);
    });
    QObject::connect(worker, &WarmupWorker::finished, &app,
                     [&window, &splash, thread, worker, db, state, engines]() {
                         thread->quit();
                         thread->wait();  // warm-up done before we build the grid
                         delete worker;
                         delete thread;

                         // warmup=nullptr: every model is warm now, so CameraGrid
                         // starts all cameras immediately (cache-hit get()).
                         window = std::make_unique<MainWindow>(db, state, engines,
                                                               nullptr);
                         window->apply_startup();
                         window->show();
                         // Created after the event loop is already running (via
                         // this queued handler), so pull it to the front and take
                         // the foreground the splash was holding.
                         window->raise();
                         window->activateWindow();
                         splash->close();
                         splash.reset();
                     });

    thread->start();
    // Release THIS scope's reference. The worker and the finished handler each
    // hold their own, and the window takes one when it is built - but a copy
    // parked in a frame that lives across app.exec() would keep the boot registry
    // (and its GPU engines) resident for the life of the process, defeating the
    // release that a committed mode switch performs.
    engines.reset();
    return app.exec();
}

// Warm restart: show the window immediately and warm models in the background;
// each detection camera starts as its model(s) come ready (WarmupState + the
// per-camera gate). No splash.
int launch_warm_ui_first(QApplication& app, QSqlDatabase db,
                         std::shared_ptr<settings::Settings> state,
                         std::shared_ptr<EngineRegistry> engines) {
    // WarmupState owns the worker thread and outlives app.exec() (this scope).
    WarmupState warmup(engines);

    // Build the window first (it subscribes CameraGrid to warmup signals in its
    // ctor), THEN start warming — so any model_ready/finished the worker queues
    // is delivered after the grid has connected (is_ready() covers a race).
    MainWindow window(db, state, engines, &warmup);
    window.apply_startup();
    window.show();
    window.raise();
    window.activateWindow();  // claim the foreground at launch

    // Engine-only, no fallback: a fatal warm-up failure aborts startup cleanly.
    QObject::connect(&warmup, &WarmupState::failed, &app, [&app](const QString& err) {
        qCritical().noquote() << "[fatal] model warm-up failed:" << err;
        // Keep exit 1 (Jetson-verified fail-loud): the shipped systemd unit
        // documents "exit 1 = warm-up found a missing/invalid engine" and does not
        // restart on it (Restart=on-abnormal ignores any clean exit). EX_CONFIG is
        // reserved for the readiness-VERDICT paths (evaluate_db_schema / integrity
        // Blocked), which is where a distinct config-fault code earns its keep.
        app.exit(1);
    });

    warmup.start();
    // As above: WarmupState and MainWindow each hold their own reference, so this
    // frame's copy would only serve to outlive the switch that is supposed to
    // release the outgoing registry.
    engines.reset();
    return app.exec();
}

}  // namespace

int launch(QApplication& app, QSqlDatabase db,
           std::shared_ptr<settings::Settings> state) {
    // Boot readiness gate. A WHOLE-MACHINE blocker (models dir unreadable, a
    // corrupt manifest, a failed catalog query) is a configuration fault no
    // restart fixes: fail with EX_CONFIG BEFORE building the window or warming
    // engines, leaving an inspectable status.json. Per-zone / Degraded issues do
    // NOT stop boot — CameraGrid installs those as inhibit causes (spec §8), and
    // it rewrites status.json with the live causes once the grid is up. The
    // DB-stage blockers (schema newer / unopenable / migration failed) were
    // already handled in main.cpp before the DB was opened.
    //
    // The verdict now consults the central compatibility policy, so it needs the
    // COMMITTED mode, the production manifest view and the measured platform —
    // resolved here, once, and reused for the warm-up firewall below so boot can
    // never judge an attachment by one manifest and warm by another.
    const denso::mode::TargetMode mode = denso::mode::load(db);
    const denso::models::ManifestView view =
        denso::models::load_manifest_view(denso::paths::models_dir());
    // Measured platform for the TensorRT built_for corroboration — read ONLY on
    // the TensorRt backend, ignored under ONNX Runtime (spec §3.2.1). The ONE
    // shared provider (probing + normalization defined once, used by
    // run_headless.cpp and CameraGrid too). A probe failure FAILS CLOSED: an empty
    // PlatformInfo corroborates no built_for, so nothing is authorized — never a
    // substituted constant.
    const denso::models::PlatformInfo platform = denso::platform::measured_platform_info();

    const auto verdict = denso::health::evaluate_integrity(
        db, denso::paths::models_dir(), mode, view, platform);
    if (verdict.status == denso::health::Readiness::Blocked) {
        // The DB is already open + migrated here (main.cpp cleared the DB-stage
        // preflight), so the real mode is determinable — emit it alongside the
        // real Blocked verdict. mode_setup_required is nullopt-omitted if the
        // camera query fails, always true for ball_leveler.
        const auto m = denso::mode::load(db);
        denso::health::write_status_file(
            denso::paths::status_file(), verdict, {}, {}, {},
            QString::fromLatin1(denso::mode::to_string(m)),
            denso::mode::mode_setup_required(db, m));
        for (const auto& b : verdict.blockers) {
            qCritical().noquote() << "[startup] BLOCKED:"
                                  << denso::health::reason_code(b.kind) << "—" << b.detail;
        }
        return denso::health::exit_code_for(verdict.status);
    }

    const std::string models_dir = denso::paths::models_dir().toStdString();
    const std::string cache_dir = denso::paths::trt_cache_dir().toStdString();

    // Release-B warm-up firewall (spec 7.0). The compatibility allow-list and the
    // mode-filtered fail-loud required set come from the ONE central policy,
    // through the ONE builder that a committed mode switch also uses
    // (ui/engine_session.h). A rejected model - wrong mode, undeclared,
    // metadata/provenance fault - is excluded from BOTH, so it is never scanned,
    // never get()-ed, never deserialized, and can never abort startup.
    //
    // Boot and the switch therefore cannot derive different sets for the same
    // mode: there is ONE construction site, not two that must be kept in step.
    // There is never a union allow-list - each registry is built for exactly one
    // mode and stays immutable and mode-pure for its whole life.
    auto engines = build_engine_registry(db, mode);

    // Splash only when there's a minutes-long build to wait on (cold); otherwise
    // the fast UI-first load.
    if (cold_start_needs_splash(models_dir, cache_dir)) {
        return launch_cold_with_splash(app, std::move(db), std::move(state),
                                       std::move(engines));
    }
    return launch_warm_ui_first(app, std::move(db), std::move(state),
                                std::move(engines));
}

} // namespace denso::ui
