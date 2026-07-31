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
    if (thread_ || retired_) {
        return;  // already started, or retired by a committed mode switch
    }
    thread_ = new QThread(this);
    worker_ = new WarmupWorker(engines_);
    worker_->moveToThread(thread_);

    connect(thread_, &QThread::started, worker_, &WarmupWorker::run);
    connect(worker_, &WarmupWorker::model_ready, this, &WarmupState::on_model_ready);
    connect(worker_, &WarmupWorker::finished, this, &WarmupState::on_finished);
    // Re-emit a fatal warm-up failure on the GUI thread (queued across threads)
    // through on_failed(), which also marks warm-up TERMINAL. The worker emits
    // `failed` and returns WITHOUT `finished`, so without this the completion
    // flag stayed false forever and every camera waiting on a model sat on
    // "Preparing model..." for the life of the process.
    connect(worker_, &WarmupWorker::failed, this, &WarmupState::on_failed);
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

void WarmupState::on_failed(const QString& error) {
    // Terminal, exactly like on_finished(): nothing more will ever warm. The
    // difference is only in WHAT is reported, never in whether waiters are
    // released — is_complete() is what lets a pending camera stop waiting and
    // resolve to Unavailable.
    complete_ = true;
    if (thread_) {
        thread_->quit();
    }
    emit failed(error);
}

void WarmupState::retire() {
    retired_ = true;
    // Every subscription, in both directions: nothing this outgoing coordinator
    // reports may reach the app or the new mode's grid.
    disconnect(this, nullptr, nullptr, nullptr);
    // Stop pinning the outgoing registry here. The worker holds its own
    // shared_ptr, so a deserialize in flight stays valid and the registry is
    // released when that worker finishes.
    engines_.reset();
}

bool WarmupState::worker_running() const {
    return thread_ != nullptr && thread_->isRunning();
}

bool WarmupState::is_ready(const std::string& filename) const {
    return ready_.count(filename) > 0;
}

} // namespace denso::ui
