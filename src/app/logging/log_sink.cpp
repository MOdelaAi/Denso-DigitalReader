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
    QFile::rename(path_, archive(path_, 1));
    ensure_open_locked();
    size_ = file_.isOpen() ? static_cast<uint64_t>(file_.size()) : 0;
}

void RotatingLogSink::write(const QByteArray& record) {
    const QByteArray line = record + '\n';
    std::lock_guard<std::mutex> lock(mutex_);

    // While degraded, retry reopening at most once a minute so a transient
    // disk-full recovers on its own without hammering the filesystem.
    if (!file_.isOpen()) {
        const auto now = std::chrono::steady_clock::now();
        if (!reopen_attempted_ ||
            now - last_reopen_attempt_ >= std::chrono::seconds(60)) {
            reopen_attempted_ = true;
            last_reopen_attempt_ = now;
            ensure_open_locked();
        }
    }

    if (file_.isOpen()) {
        if (should_rotate(size_, static_cast<uint64_t>(line.size()), max_bytes_)) {
            rotate_locked();
        }
        const qint64 n = file_.write(line);
        if (n < 0) {
            if (!degraded_.load(std::memory_order_relaxed)) {
                degraded_.store(true, std::memory_order_relaxed);
                std::fprintf(stderr, "[log] file logging DEGRADED (write failed): %s\n",
                             file_.errorString().toLocal8Bit().constData());
            }
            file_.close();  // force a reopen attempt next time
        } else {
            file_.flush();  // low volume → flush each record for crash diagnosis
            size_ += static_cast<uint64_t>(n);
        }
    }

    // Always mirror to stderr (dev + a fallback when the file sink is degraded).
    std::fprintf(stderr, "%s", line.constData());
}

} // namespace denso::logging
