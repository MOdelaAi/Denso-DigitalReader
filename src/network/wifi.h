// Wi-Fi support: parse `netsh wlan show networks` into WifiNetworks and build
// the WLAN profile XML used to join a network. Pure. Ported from Rust
// `network::windows::wifi`.
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "network/model.h"

namespace denso::network::wifi {

/// Parse `netsh wlan show networks mode=bssid` into the visible networks.
std::vector<WifiNetwork> parse_wifi_networks(const std::string& out);

/// Escape XML text / attribute special characters.
std::string xml_escape(const std::string& s);

/// Build a WLAN profile XML for `ssid`. A present password produces a WPA2-PSK
/// profile carrying the key; absent produces an open-network profile.
std::string build_profile_xml(const std::string& ssid, const std::optional<std::string>& password);

} // namespace denso::network::wifi
