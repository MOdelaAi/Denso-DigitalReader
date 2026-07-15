// Pure rotation policy for the bounded 24/7 log file — no Qt, unit-tested. The
// RotatingLogSink (log_sink) does the actual file renames using these decisions.
#pragma once

#include <cstdint>
#include <string>

namespace denso::logging {

// Defaults: 5 files of 5 MiB each → ~25 MiB total, bounded forever.
constexpr uint64_t kDefaultMaxBytes = 5ull * 1024 * 1024;  // per file
constexpr int kDefaultMaxFiles = 5;                        // active + 4 archives

/// Rotate before writing `record_bytes` to a file currently `current_bytes`?
/// True only when the record would overflow the cap AND the file already has
/// content — so a single oversized record still lands (in a fresh file) instead
/// of rotating endlessly on an empty file.
bool should_rotate(uint64_t current_bytes, uint64_t record_bytes,
                   uint64_t cap_bytes);

/// Archive path for slot `n` (1-based): rotated_path("denso.log", 1) ==
/// "denso.log.1". n <= 0 returns the base path unchanged.
std::string rotated_path(const std::string& base, int n);

} // namespace denso::logging
