// Startup orchestration: build + show MainWindow immediately with the shared
// EngineRegistry injected, then start warming the detection engines on a
// background thread (WarmupState). CameraGrid starts each camera as its models
// come ready. Keeps main.cpp a thin orchestrator.
#pragma once

#include "settings/settings.h"

#include <QSqlDatabase>

#include <memory>

class QApplication;

namespace denso::ui {

int launch(QApplication& app, QSqlDatabase db,
           std::shared_ptr<settings::Settings> state);

} // namespace denso::ui
