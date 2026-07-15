#include "logging/log_sink.h"

#include <cstdio>

namespace denso::logging {
namespace {
QString archive(const QString& base, int n) {
    return QString::fromStdString(rotated_path(base.toStdString(), n));
}
}  // namespace

RotatingLogSink::RotatingLogSink(QString path, uint64_t max_bytes, int max_files)
    : path_(std::move(path)),
      max_bytes_(max_bytes),
      max_files_(max_files < 1 ? 1 : max_files) {
    std::lock_guard<std::mutex> lock(mutex_);
    ensure_open_locked();
}

void RotatingLogSink::ensure_open_locked() {
    if (file_.isOpen()) {
        return;
    }
    file_.setFileName(path_);
    if (file_.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        size_ = static_cast<uint64_t>(file_.size());
        if (degraded_.load(std::memory_order_relaxed)) {
            degraded_.store(false, std::memory_order_relaxed);
            std::fprintf(stderr, "[log] file logging resumed: %s\n",
                         path_.toLocal8Bit().constData());
        }
    } else if (!degraded_.load(std::memory_order_relaxed)) {
        degraded_.store(true, std::memory_order_relaxed);
        std::fprintf(stderr, "[log] file logging DEGRADED (open failed): %s — %s\n",
                     path_.toLocal8Bit().constData(),
                     file_.errorString().toLocal8Bit().constData());
    }
}

void RotatingLogSink::rotate_locked() {
    file_.close();
    // Drop the oldest archive (.(max_files-1)), then shift each up one slot, then
    // move the active file to .1. Renaming into a just-vacated slot, in descending
    // order, avoids "target exists" failures. A missing source rename is a no-op.
    QFile::remove(archive(path_, max_files_ - 1));
    for (int n = max_files_ - 1; n >= 2; --n) {
        QFile::rename(archive(path_, n - 1), archive(path_, n));
    }
    const bool rolled = QFile::rename(path_, archive(path_, 1));
    if (rolled) {
        ensure_open_locked();  // fresh empty active file
        size_ = file_.isOpen() ? static_cast<uint64_t>(file_.size()) : 0;
        return;
    }
    // Could not archive the active file (locked / permission / disk). Enforce the
    // size bound anyway by TRUNCATING the active file, and flag degraded so the
    // loss is visible — better than an unbounded file that silently grows.
    file_.setFileName(path_);
    if (file_.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        size_ = 0;
    } else {
        size_ = 0;
    }
    if (!degraded_.load(std::memory_order_relaxed)) {
        degraded_.store(true, std::memory_order_relaxed);
        std::fprintf(stderr,
                     "[log] rotate failed — truncated active log to hold the size bound\n");
    }
}

void RotatingLogSink::write(const QByteArray& record) noexcept {
    // Cap the total written record (incl. suffix + newline) so one pathological
    // message can't blow the per-file size bound (Qt messages have no intrinsic
    // limit). kMaxRecord is the HARD ceiling on line.size().
    static constexpr int kMaxRecord = 16 * 1024;
    static const QByteArray kSuffix = "…[truncated]";
    QByteArray line;
    try {
        if (record.size() + 1 > kMaxRecord) {  // +1 for the '\n' below
            line = record.left(kMaxRecord - static_cast<int>(kSuffix.size()) - 1) +
                   kSuffix;
        } else {
            line = record;
        }
        line += '\n';  // total now == kMaxRecord when truncated
    } catch (...) {
        std::fprintf(stderr, "[log] record allocation failed\n");
        return;
    }

    // The whole handler boundary must contain every exception (allocation, etc.)
    // and never propagate back through Qt's logging, and never recurse into it.
    try {
        std::lock_guard<std::mutex> lock(mutex_);

        // While degraded, retry reopening at most once a minute so a transient
        // disk-full recovers without hammering the filesystem.
        if (!file_.isOpen()) {
            const auto now = std::chrono::steady_clock::now();
            if (!reopen_attempted_ ||
                now - last_reopen_attempt_ >= std::chrono::seconds(60)) {
                reopen_attempted_ = true;
                last_reopen_attempt_ = now;
                ensure_open_locked();
            }
        }

        bool wrote = false;
        if (file_.isOpen()) {
            if (should_rotate(size_, static_cast<uint64_t>(line.size()), max_bytes_)) {
                rotate_locked();
            }
            const qint64 n = file_.write(line);
            // Require a COMPLETE write AND a successful flush — a partial write or a
            // failed flush (ENOSPC) must not be reported as healthy.
            if (n == line.size() && file_.flush()) {
                size_ += static_cast<uint64_t>(n);
                wrote = true;
            } else {
                if (!degraded_.load(std::memory_order_relaxed)) {
                    degraded_.store(true, std::memory_order_relaxed);
                    std::fprintf(stderr, "[log] file logging DEGRADED (write/flush): %s\n",
                                 file_.errorString().toLocal8Bit().constData());
                }
                file_.close();  // force a reopen attempt next time
            }
        }

        // Mirror to stderr ONLY as a fallback when the file sink didn't take it —
        // an always-on mirror would be unbounded if stderr is redirected to a file.
        if (!wrote) {
            std::fprintf(stderr, "%.*s", static_cast<int>(line.size()), line.constData());
        }
    } catch (...) {
        std::fprintf(stderr, "[log] write failed (exception)\n");
    }
}

} // namespace denso::logging
