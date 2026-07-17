#include "instance/single_instance.h"

#include <QFileInfo>
#include <QLockFile>

#include <cstdio>
#include <utility>

namespace denso::instance {

SingleInstance::SingleInstance(QString lock_path)
    : path_(std::move(lock_path)), lock_(std::make_unique<QLockFile>(path_)) {
    // QLockFile's default 30s staleness heuristic is what recovers the lock
    // after a hard kill: it reads the recorded pid and reclaims only if that
    // process is gone. Keep the default — a 0 here would mean "never stale".
}

SingleInstance::~SingleInstance() = default;  // ~QLockFile unlocks + removes

bool SingleInstance::acquire() {
    if (held_) return true;
    // Recorded before tryLock so a successful lock can tell "created fresh"
    // from "reclaimed a corpse left by a process that died without releasing
    // it" — the latter is worth a stderr note (the log sink doesn't exist yet).
    const bool existed_before = QFileInfo::exists(path_);
    held_ = lock_->tryLock(0);  // 0 = do not wait
    if (held_ && existed_before) {
        std::fprintf(stderr, "denso: recovered a stale lock at %s\n", qPrintable(path_));
    }
    return held_;
}

bool SingleInstance::is_held() const { return held_; }

QLockFile::LockError SingleInstance::last_error() const { return lock_->error(); }

RunState SingleInstance::running_state(const QString& lock_path) {
    QLockFile probe(lock_path);
    if (probe.tryLock(0)) {
        probe.unlock();  // removes the file we just made — leave no corpse
        return RunState::NotRunning;
    }
    // LockFailedError means another process genuinely holds it — a real
    // "running" answer. Anything else (PermissionError, UnknownError) means
    // the probe couldn't even create/open the lock file, so it has no basis
    // for either NotRunning or Running.
    if (probe.error() == QLockFile::LockFailedError) {
        return RunState::Running;
    }
    return RunState::Indeterminate;
}

} // namespace denso::instance
