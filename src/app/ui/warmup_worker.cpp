#include "ui/warmup_worker.h"

namespace denso::ui {

WarmupWorker::WarmupWorker(std::shared_ptr<EngineRegistry> engines,
                           QObject* parent)
    : QObject(parent), engines_(std::move(engines)) {}

void WarmupWorker::run() {
    if (engines_) {
        engines_->warm_up([this](const std::string& name) {
            emit progress(QStringLiteral("Preparing model %1…")
                              .arg(QString::fromStdString(name)));
        });
    }
    emit finished();
}

} // namespace denso::ui
