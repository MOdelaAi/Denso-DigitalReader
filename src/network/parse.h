// Parse Windows status CLIs (`ipconfig`, `netsh wlan show interfaces`) into the
// domain NetworkSnapshot. Pure string work, unit-tested off-device. Ported from
// Rust `network::windows::parse`.
#pragma once

#include <string>
#include <tuple>
#include <utility>

#include "network/model.h"

namespace denso::network::parse {

/// Value after the FIRST ':' on a "key : value" line, trimmed; strips a
/// trailing "(Preferred)". Splitting on the first colon keeps values that
/// contain ':' intact (an SSID like "Corp:Net", an IPv6 gateway "fe80::1").
std::string value_after_colon(const std::string& line);

/// Parse `ipconfig` into (ethernet, wifi) IP/gateway.
std::pair<InterfaceStatus, InterfaceStatus> parse_ipconfig(const std::string& out);

/// Parse `netsh wlan show interfaces` for (connected, ssid, signal).
std::tuple<bool, std::string, std::string> parse_netsh_wlan(const std::string& out);

NetworkSnapshot build_snapshot(const std::string& ipcfg, const std::string& wlan);

} // namespace denso::network::parse
