// Runs EngineRegistry::warm_up() off the main thread (moveToThread'd onto a
// QThread by ui/startup) so the StartupScreen keeps animating during the
// minutes-long TensorRT build. Emits progress per model and finished() at the
// end; both cross to the main thread via queued connections.
#pragma once

#include "ui/camera/shared/detection/engine_registry.h"

#include <QObject>
#include <QString>

#include <memory>

namespace denso::ui {

class WarmupWorker : public QObject {
    Q_OBJECT

public:
    explicit WarmupWorker(std::shared_ptr<EngineRegistry> engines,
                          QObject* parent = nullptr);

public slots:
    void run();

signals:
    void progress(const QString& model);
    void finished();

private:
    std::shared_ptr<EngineRegistry> engines_;
};

} // namespace denso::ui
