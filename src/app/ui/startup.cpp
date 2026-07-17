#include "ui/startup.h"

#include "detection/repo.h"
#include "detection/engine_registry.h"
#include "paths/paths.h"
#include "ui/mainwindow.h"
#include "ui/startup_mode.h"
#include "ui/startup_screen.h"
#include "ui/warmup_state.h"
#include "ui/warmup_worker.h"

#include <QApplication>
#include <QDebug>
#include <QThread>

#include <memory>
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
        app.exit(1);
    });

    warmup.start();
    return app.exec();
}

}  // namespace

int launch(QApplication& app, QSqlDatabase db,
           std::shared_ptr<settings::Settings> state) {
    const std::string models_dir = denso::paths::models_dir().toStdString();
    const std::string cache_dir = denso::paths::trt_cache_dir().toStdString();
    // The models configured cameras actually need — warm_up fails loud if any is
    // missing, instead of silently demoting the camera to no-detection.
    std::vector<std::string> required = detection::attached_model_filenames(db);
    auto engines = std::make_shared<EngineRegistry>(models_dir, cache_dir,
                                                    std::move(required));

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
