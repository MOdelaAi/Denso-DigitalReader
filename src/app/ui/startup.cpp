#include "ui/startup.h"

#include "ui/camera/shared/detection/engine_registry.h"
#include "ui/mainwindow.h"
#include "ui/startup_screen.h"
#include "ui/warmup_worker.h"

#include <QApplication>
#include <QCoreApplication>
#include <QThread>

#include <memory>
#include <string>

namespace denso::ui {

int launch(QApplication& app, QSqlDatabase db,
           std::shared_ptr<settings::Settings> state) {
    const std::string dir = QCoreApplication::applicationDirPath().toStdString();
    auto engines = std::make_shared<EngineRegistry>(dir + "/models",
                                                    dir + "/models/trt_cache");

    auto splash = std::make_unique<StartupScreen>(state->dark);
    splash->show();

    auto* thread = new QThread;
    auto* worker = new WarmupWorker(engines);
    worker->moveToThread(thread);

    // Built on the main thread once warm-up finishes; must outlive app.exec(),
    // so it lives in this scope and is populated by the finished handler.
    std::unique_ptr<MainWindow> window;

    QObject::connect(thread, &QThread::started, worker, &WarmupWorker::run);
    QObject::connect(worker, &WarmupWorker::progress, splash.get(),
                     &StartupScreen::set_status);
    QObject::connect(worker, &WarmupWorker::finished, &app,
                     [&window, &splash, thread, worker, db, state, engines]() {
                         thread->quit();
                         thread->wait();  // warm-up done before we build the grid
                         delete worker;
                         delete thread;

                         window = std::make_unique<MainWindow>(db, state, engines);
                         window->apply_startup();
                         window->show();
                         splash->close();
                         splash.reset();
                     });

    thread->start();
    return app.exec();
}

} // namespace denso::ui
