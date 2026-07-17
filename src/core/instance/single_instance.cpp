#include "instance/single_instance.h"

#include <QLockFile>

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
    held_ = lock_->tryLock(0);  // 0 = do not wait
    return held_;
}

bool SingleInstance::is_held() const { return held_; }

bool SingleInstance::is_running(const QString& lock_path) {
    QLockFile probe(lock_path);
    if (probe.tryLock(0)) {
        probe.unlock();  // removes the file we just made — leave no corpse
        return false;
    }
    return true;
}

} // namespace denso::instance
