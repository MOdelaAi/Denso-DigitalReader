#include "hardware/collect.h"

#include "hardware/format.h"

#include <QStorageInfo>
#include <QString>
#include <QSysInfo>

#include <cstdint>
#include <optional>

#if defined(_WIN32)
#include <windows.h>
#elif defined(__linux__)
#include <unistd.h>
#endif

namespace denso::hardware {
namespace {

/// Total physical RAM in bytes, 0 if undetermined (mirrors sysinfo's
/// `total_memory()` feeding `size_or_unknown`).
uint64_t total_ram() {
#if defined(_WIN32)
    MEMORYSTATUSEX s{};
    s.dwLength = sizeof(s);
    return GlobalMemoryStatusEx(&s) ? static_cast<uint64_t>(s.ullTotalPhys) : 0;
#elif defined(__linux__)
    const long pages = sysconf(_SC_PHYS_PAGES);
    const long page_size = sysconf(_SC_PAGE_SIZE);
    if (pages <= 0 || page_size <= 0) return 0;
    return static_cast<uint64_t>(pages) * static_cast<uint64_t>(page_size);
#else
    return 0;
#endif
}

/// Total bytes across every mounted volume (mirrors sysinfo summing each disk's
/// `total_space()`). KNOWN LIMITATION (carried from the Rust app): on Linux this
/// includes pseudo-filesystems (loop/tmpfs/overlay), over-counting storage.
uint64_t total_storage() {
    uint64_t total = 0;
    for (const QStorageInfo& v : QStorageInfo::mountedVolumes()) {
        if (v.isValid() && v.isReady()) {
            const qint64 bytes = v.bytesTotal();
            if (bytes > 0) total += static_cast<uint64_t>(bytes);
        }
    }
    return total;
}

/// Treat Qt's "unknown" sentinel / empty string as the Rust `None` case.
std::optional<QString> present(QString s) {
    if (s.isEmpty() || s == QStringLiteral("unknown")) return std::nullopt;
    return s;
}

} // namespace

HardwareSpec collect() {
    const auto name = present(QSysInfo::productType());
    const auto version = present(QSysInfo::productVersion());
    QString os;
    if (name && version) {
        os = *name + " " + *version;
    } else if (name) {
        os = *name;
    } else if (version) {
        os = *version;
    } else {
        os = QStringLiteral("Unknown");
    }

    const QString host = QSysInfo::machineHostName();
    const std::string device = host.isEmpty() ? "Unknown" : host.toStdString();

    HardwareSpec spec;
    spec.os = os.toStdString();
    spec.device = device;
    spec.ram = size_or_unknown(total_ram());
    spec.storage = size_or_unknown(total_storage());
    return spec;
}

} // namespace denso::hardware
