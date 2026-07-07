// GUI-thread owner of the background warm-up: spins up the WarmupWorker on its
// own QThread, records which model files have finished warming, and re-emits
// per-model readiness + completion on the GUI thread. The CameraGrid subscribes
// here (and queries is_ready/is_complete at reload) to start each detection
// camera as its models come ready. Records readiness in a set AND emits, so a
// model that finishes before a subscriber connects is not missed.
#pragma once

#include "ui/camera/shared/detection/engine_registry.h"

#include <QObject>
#include <QString>

#include <memory>
#include <set>
#include <string>

class QThread;

namespace denso::ui {

class WarmupWorker;

class WarmupState : public QObject {
    Q_OBJECT

public:
    explicit WarmupState(std::shared_ptr<EngineRegistry> engines,
                         QObject* parent = nullptr);
    ~WarmupState() override;

    /// Start warming on the background thread. Call once, after subscribers have
    /// connected (or rely on is_ready/is_complete for anything that races).
    void start();

    bool is_ready(const std::string& filename) const;
    bool is_complete() const { return complete_; }

signals:
    void model_ready(const QString& filename);
    void finished();

private slots:
    void on_model_ready(const QString& filename);
    void on_finished();

private:
    std::shared_ptr<EngineRegistry> engines_;
    QThread* thread_ = nullptr;
    WarmupWorker* worker_ = nullptr;
    std::set<std::string> ready_;
    bool complete_ = false;
};

} // namespace denso::ui
