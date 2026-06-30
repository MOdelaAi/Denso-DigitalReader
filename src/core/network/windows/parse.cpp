#include "network/windows/parse.h"

#include "util/strutil.h"

namespace denso::network::parse {

using denso::strutil::iequals;
using denso::strutil::replace_all;
using denso::strutil::split_lines;
using denso::strutil::to_lower;
using denso::strutil::trim;

std::string value_after_colon(const std::string& line) {
    const auto pos = line.find(':');
    if (pos == std::string::npos) return "";
    std::string v = trim(line.substr(pos + 1));
    v = replace_all(v, "(Preferred)", "");
    return trim(v);
}

std::pair<InterfaceStatus, InterfaceStatus> parse_ipconfig(const std::string& out) {
    InterfaceStatus eth;
    InterfaceStatus wifi;
    enum class Target { None, Eth, Wifi };
    Target target = Target::None;

    for (const auto& line : split_lines(out)) {
        const bool indented = !line.empty() && (line[0] == ' ' || line[0] == '\t');
        if (!indented && line.find("adapter") != std::string::npos) {
            const std::string h = to_lower(line);
            if (h.find("wireless") != std::string::npos || h.find("wi-fi") != std::string::npos) {
                target = Target::Wifi;
            } else if (h.find("ethernet") != std::string::npos) {
                target = Target::Eth;
            } else {
                target = Target::None;
            }
            continue;
        }
        if (target == Target::None) continue;
        InterfaceStatus& slot = (target == Target::Wifi) ? wifi : eth;
        const std::string l = trim(line);
        if (l.starts_with("IPv4 Address")) {
            slot.ip = value_after_colon(line);
            slot.connected = !slot.ip.empty();
        } else if (l.starts_with("Default Gateway")) {
            const std::string g = value_after_colon(line);
            if (!g.empty()) slot.gateway = g;
        }
    }
    return {eth, wifi};
}

std::tuple<bool, std::string, std::string> parse_netsh_wlan(const std::string& out) {
    bool connected = false;
    std::string ssid;
    std::string signal;
    for (const auto& line : split_lines(out)) {
        const std::string l = trim(line);
        if (l.starts_with("State")) {
            connected = iequals(value_after_colon(line), "connected");
        } else if (l.starts_with("SSID") && !l.starts_with("BSSID")) {
            ssid = value_after_colon(line);
        } else if (l.starts_with("Signal")) {
            signal = value_after_colon(line);
        }
    }
    return {connected, ssid, signal};
}

NetworkSnapshot build_snapshot(const std::string& ipcfg, const std::string& wlan) {
    auto [eth, wifi] = parse_ipconfig(ipcfg);
    auto [wconn, ssid, signal] = parse_netsh_wlan(wlan);
    wifi.connected = wifi.connected || wconn;
    wifi.ssid = ssid;
    wifi.signal = signal;
    return NetworkSnapshot{eth, wifi};
}

} // namespace denso::network::parse
