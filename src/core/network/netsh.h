// Build the ordered `netsh interface ip` argument lists that apply a NetConfig.
// Pure (no process execution) so it can be unit-tested off a live network —
// the runners themselves live in the Windows backend. Ported from
// Rust `network::windows::netsh` (the pure half).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "network/model.h"

namespace denso::network::netsh {

/// Convert a CIDR prefix length (0..=32) to a dotted IPv4 subnet mask.
std::string prefix_to_mask(uint32_t prefix);

/// Build the ordered `netsh` argument lists that apply `c` to its adapter.
/// Throws `std::runtime_error` with a human-readable message for an invalid
/// config (mirrors the Rust `Result::Err`); the backend catches and surfaces it.
std::vector<std::vector<std::string>> build_netsh_commands(const NetConfig& c);

} // namespace denso::network::netsh
