#include "hardware/format.h"

#include <cstdio>

namespace denso::hardware {

std::string format_bytes(uint64_t bytes) {
    constexpr uint64_t GB = 1'000'000'000ull;
    constexpr uint64_t TB = 1'000'000'000'000ull;
    if (bytes >= TB) {
        char buf[32];
        std::snprintf(buf, sizeof buf, "%.1f TB", static_cast<double>(bytes) / static_cast<double>(TB));
        return buf;
    }
    return std::to_string(bytes / GB) + " GB";
}

std::string size_or_unknown(uint64_t bytes) {
    if (bytes == 0) return "Unknown";
    return format_bytes(bytes);
}

} // namespace denso::hardware
