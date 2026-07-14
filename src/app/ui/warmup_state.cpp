#include "ui/warmup_state.h"

#include "ui/warmup_worker.h"

#include <QThread>

#include <utility>

namespace denso::ui {

WarmupState::WarmupState(std::shared_ptr<EngineRegistry> engines, QObject* parent)
    : QObject(parent), engines_(std::move(engines)) {}

WarmupState::~WarmupState() {
    if (thread_) {
        thread_->quit();
        thread_->wait();  // join the warm-up worker before we (and it) die
    }
}

void WarmupState::start() {
    if (thread_) {
        return;  // already started
    }
    thread_ = new QThread(this);
    worker_ = new WarmupWorker(engines_);
    worker_->moveToThread(thread_);

    connect(thread_, &QThread::started, worker_, &WarmupWorker::run);
    connect(worker_, &WarmupWorker::model_ready, this, &WarmupState::on_model_ready);
    connect(worker_, &WarmupWorker::finished, this, &WarmupState::on_finished);
    // Re-emit a fatal warm-up failure on the GUI thread (queued across threads).
    connect(worker_, &WarmupWorker::failed, this, &WarmupState::failed);
    // Clean up the worker when the thread finishes; the thread is a child of this.
    connect(thread_, &QThread::finished, worker_, &QObject::deleteLater);

    thread_->start();
}

void WarmupState::on_model_ready(const QString& filename) {
    ready_.insert(filename.toStdString());
    emit model_ready(filename);
}

void WarmupState::on_finished() {
    complete_ = true;
    thread_->quit();  // let the thread wind down; worker deleteLater's on finished
    emit finished();
}

bool WarmupState::is_ready(const std::string& filename) const {
    return ready_.count(filename) > 0;
}

} // namespace denso::ui
