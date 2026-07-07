// Startup orchestration: pick the launch UX. A cold start (no cached TensorRT
// engine → the minutes-long build) shows the blocking StartupScreen splash and
// warms behind it, then builds MainWindow. A warm restart builds + shows
// MainWindow immediately and warms in the background (WarmupState). The pure
// ui/startup_mode probe decides. Keeps main.cpp a thin orchestrator.
#pragma once

#include "settings/settings.h"

#include <QSqlDatabase>

#include <memory>

class QApplication;

namespace denso::ui {

int launch(QApplication& app, QSqlDatabase db,
           std::shared_ptr<settings::Settings> state);

} // namespace denso::ui
