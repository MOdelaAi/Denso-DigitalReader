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

#include <QString>

#include <memory>

class QLockFile;

namespace denso::instance {

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

    /// True if some process currently holds `lock_path`.
    ///
    /// This is the SOLE exemption to "checks never take the production lock":
    /// answering the question requires tryLock(), which briefly acquires and
    /// releases when nothing is running. It must therefore run as the target
    /// user — as root it would leave a root-owned lock artifact in an
    /// operator-owned data dir, the exact poisoning the data-dir rules prevent.
    static bool is_running(const QString& lock_path);

private:
    QString path_;
    std::unique_ptr<QLockFile> lock_;
    bool held_ = false;
};

} // namespace denso::instance
