// GUI-thread owner of the background warm-up: spins up the WarmupWorker on its
// own QThread, records which model files have finished warming, and re-emits
// per-model readiness + completion on the GUI thread. The CameraGrid subscribes
// here (and queries is_ready/is_complete at reload) to start each detection
// camera as its models come ready. Records readiness in a set AND emits, so a
// model that finishes before a subscriber connects is not missed.
#pragma once

#include "detection/engine_registry.h"

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

    /// Sever this coordinator from everything it drives and stop pinning its
    /// registry. Called on the OUTGOING coordinator when a mode switch commits.
    ///
    /// Needed because the BOOT coordinator is owned by ui::launch, not by the
    /// window, so a switch cannot destroy it — and its `failed` is wired at boot
    /// to app.exit(1), which is BOOT-only semantics. Left connected, an outgoing
    /// mode's warm-up failure arriving after a committed switch would take a
    /// working appliance dark, contrary to spec 7.3/7.5. Its `model_ready` would
    /// likewise be able to start a camera in a mode it was not built for.
    ///
    /// Does NOT join: warm_up() has no cancellation point, and blocking the GUI
    /// thread on a deserialize in progress is worse than letting the retired
    /// worker finish into a disconnected object. The destructor still joins.
    void retire();

    /// Is the warm-up thread still running? Lets an owner hold a RETIRED
    /// coordinator until its uncancellable work drains, instead of destroying it
    /// inline and blocking on the join in ~WarmupState.
    bool worker_running() const;

    bool is_ready(const std::string& filename) const;
    /// True once warm-up can produce nothing further — completed OR failed.
    /// Failure counts: a caller waiting for a model that will now never warm
    /// must fall through and resolve, not wait forever.
    bool is_complete() const { return complete_; }

signals:
    void model_ready(const QString& filename);
    void finished();
    /// Re-emitted from the worker: a fatal warm-up failure (engine-only, no
    /// fallback). The app connects this to a clean abort.
    void failed(const QString& error);

private slots:
    void on_model_ready(const QString& filename);
    void on_finished();
    void on_failed(const QString& error);

private:
    std::shared_ptr<EngineRegistry> engines_;
    QThread* thread_ = nullptr;
    WarmupWorker* worker_ = nullptr;
    std::set<std::string> ready_;
    bool complete_ = false;
    bool retired_ = false;
};

} // namespace denso::ui
