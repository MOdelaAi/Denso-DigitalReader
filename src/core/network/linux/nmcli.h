// Parse `nmcli` (NetworkManager) status output into the domain types. Pure
// string work, unit-tested off-device — the Linux backend's runner half lives
// in linux_backend.cpp. Ported from Rust `network::linux` (the pure helpers).
#pragma once

#include <optional>
#include <string>
#include <utility>

namespace denso::network::nmcli {

/// From `nmcli -t -f DEVICE,TYPE,STATE device`, return the first connected
/// (ethernet, wifi) device names (nullopt when none).
std::pair<std::optional<std::string>, std::optional<std::string>> pick_devices(
    const std::string& out);

/// From `nmcli -t -f IP4.ADDRESS,IP4.GATEWAY device show <dev>`, return
/// (ip-without-prefix, gateway).
std::pair<std::string, std::string> parse_device_show(const std::string& out);

/// From `nmcli -t -f ACTIVE,SSID,SIGNAL device wifi`, return the active
/// (ssid, "NN%").
std::pair<std::string, std::string> parse_wifi(const std::string& out);

} // namespace denso::network::nmcli
