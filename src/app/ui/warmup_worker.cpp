#include "ui/warmup_worker.h"

namespace denso::ui {

WarmupWorker::WarmupWorker(std::shared_ptr<EngineRegistry> engines,
                           QObject* parent)
    : QObject(parent), engines_(std::move(engines)) {}

void WarmupWorker::run() {
    // Engine-only, no fallback: a missing/incompatible engine (TrtEngine's ctor)
    // throws. Catch it here so it can't std::terminate the worker thread —
    // surface it as a fatal `failed` and do NOT emit finished (which would let
    // the app start cameras as if the models were warm).
    try {
        if (engines_) {
            engines_->warm_up(
                [this](const std::string& name) {
                    emit progress(QStringLiteral("Preparing model %1…")
                                      .arg(QString::fromStdString(name)));
                },
                [this](const std::string& name) {
                    emit model_ready(QString::fromStdString(name));
                });
        }
    } catch (const std::exception& e) {
        emit failed(QString::fromUtf8(e.what()));
        return;
    }
    emit finished();
}

} // namespace denso::ui
