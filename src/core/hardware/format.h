// Human-readable byte sizes for the host spec. Pure; the live `collect()` that
// reads the OS lives separately. Ported from Rust `hardware::repo` (the pure
// formatting half).
#pragma once

#include <cstdint>
#include <string>

namespace denso::hardware {

std::string format_bytes(uint64_t bytes);
std::string size_or_unknown(uint64_t bytes);

} // namespace denso::hardware
