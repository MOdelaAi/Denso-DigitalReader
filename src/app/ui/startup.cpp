#include "ui/startup.h"

#include "ui/camera/shared/detection/engine_registry.h"
#include "ui/mainwindow.h"
#include "ui/warmup_state.h"

#include <QApplication>
#include <QCoreApplication>

#include <memory>
#include <string>

namespace denso::ui {

int launch(QApplication& app, QSqlDatabase db,
           std::shared_ptr<settings::Settings> state) {
    const std::string dir = QCoreApplication::applicationDirPath().toStdString();
    auto engines = std::make_shared<EngineRegistry>(dir + "/models",
                                                    dir + "/models/trt_cache");

    // Warm-up runs in the background; the window shows immediately. WarmupState
    // owns the worker thread and outlives app.exec() (it lives in this scope).
    WarmupState warmup(engines);

    // Build the window first (it subscribes CameraGrid to warmup signals in its
    // ctor), THEN start warming — so any model_ready/finished the worker queues
    // is delivered after the grid has connected (and is_ready() covers anything
    // that raced ahead).
    MainWindow window(db, state, engines, &warmup);
    window.apply_startup();
    window.show();
    window.raise();
    window.activateWindow();  // claim the foreground at launch

    warmup.start();
    return app.exec();
}

} // namespace denso::ui
