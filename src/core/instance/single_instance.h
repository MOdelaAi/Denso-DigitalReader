// Single-instance guard over QLockFile.
//
// Why: the appliance autostarts AND has a clickable menu icon, so a second
// process is inevitable. Two processes would open the same cameras, compete to
// write network config, contend on one SQLite file, and silently eat log data —
// rename-based rotation does not move another process's open fd to the new
// pathname, so the loser keeps writing into denso.log.1 after the winner rotates.
//
// Acquire this BEFORE the DB opens, logging initializes, or cameras start.
#pragma once

#include <QLockFile>
#include <QString>

#include <memory>

namespace denso::instance {

/// Tri-state result of probing whether some process holds the lock.
/// Indeterminate means the lock file itself could not be created/opened
/// (missing parent dir, permissions, read-only mount, wrong ownership) — the
/// probe simply could not answer. Callers MUST treat this as its own case:
/// never fold it into NotRunning (a caller could then proceed as if it were
/// safe to start) or into Running (a caller could then refuse a legitimate
/// action, e.g. an upgrade, forever on a false positive).
enum class RunState { NotRunning, Running, Indeterminate };

class SingleInstance {
public:
    explicit SingleInstance(QString lock_path);
    ~SingleInstance();

    SingleInstance(const SingleInstance&) = delete;
    SingleInstance& operator=(const SingleInstance&) = delete;

    /// Try to become the one live instance. Non-blocking. Idempotent: calling it
    /// again while already held returns true without re-locking.
    bool acquire();

    bool is_held() const;

    /// The QLockFile error from the last tryLock() attempt (NoError if none has
    /// failed). Lets a caller distinguish "another process genuinely holds the
    /// lock" (LockFailedError) from "could not even create the lock file"
    /// (PermissionError / UnknownError — missing dir, read-only mount, wrong
    /// ownership) — those are not the same failure and must not be reported
    /// identically.
    QLockFile::LockError last_error() const;

    /// Tri-state liveness probe over `lock_path`.
    ///
    /// This is the SOLE exemption to "checks never take the production lock":
    /// answering the question requires tryLock(), which briefly acquires and
    /// releases when nothing is running. It must therefore run as the target
    /// user — as root it would leave a root-owned lock artifact in an
    /// operator-owned data dir, the exact poisoning the data-dir rules prevent.
    static RunState running_state(const QString& lock_path);

private:
    QString path_;
    std::unique_ptr<QLockFile> lock_;
    bool held_ = false;
};

} // namespace denso::instance
