// The bounded 24/7 log file writer. One writer, mutex-guarded, size-rotating
// (close → roll .N chain → reopen). Never throws, never recurses into Qt logging;
// on file failure it falls back to stderr, marks itself degraded, and retries
// reopening. Installed as the QtMessageHandler in main.cpp.
#pragma once

#include "logging/log_rotation.h"

#include <QFile>
#include <QString>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>

namespace denso::logging {

class RotatingLogSink {
public:
    explicit RotatingLogSink(QString path, uint64_t max_bytes = kDefaultMaxBytes,
                             int max_files = kDefaultMaxFiles);

    /// Append one already-formatted record (no trailing newline). Rotates first
    /// if the record would overflow the per-file cap. Thread-safe. Never throws.
    void write(const QByteArray& record);

    /// True when file logging has failed and output is falling back to stderr —
    /// surfaced in the heartbeat so a degraded logger is visible in the log.
    bool degraded() const { return degraded_.load(std::memory_order_relaxed); }

private:
    void ensure_open_locked();  // open if closed (best-effort); updates size_
    void rotate_locked();       // close, roll the .N chain, reopen fresh

    const QString path_;
    const uint64_t max_bytes_;
    const int max_files_;

    std::mutex mutex_;
    QFile file_;
    uint64_t size_ = 0;  // active file size in bytes
    std::atomic<bool> degraded_{false};
    std::chrono::steady_clock::time_point last_reopen_attempt_{};
    bool reopen_attempted_ = false;
};

} // namespace denso::logging
