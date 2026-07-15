#include "logging/log_rotation.h"

namespace denso::logging {

bool should_rotate(uint64_t current_bytes, uint64_t record_bytes,
                   uint64_t cap_bytes) {
    if (current_bytes == 0) {
        return false;  // never rotate an empty file, even for an oversized record
    }
    return current_bytes + record_bytes > cap_bytes;
}

std::string rotated_path(const std::string& base, int n) {
    if (n <= 0) {
        return base;
    }
    return base + "." + std::to_string(n);
}

} // namespace denso::logging
