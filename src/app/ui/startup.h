// Startup orchestration: show the StartupScreen, warm the detection engines on
// a background thread, then build and show MainWindow with the pre-warmed,
// shared EngineRegistry injected. Keeps main.cpp a thin orchestrator.
#pragma once

#include "settings/settings.h"

#include <QSqlDatabase>

#include <memory>

class QApplication;

namespace denso::ui {

int launch(QApplication& app, QSqlDatabase db,
           std::shared_ptr<settings::Settings> state);

} // namespace denso::ui
