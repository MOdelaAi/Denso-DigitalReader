#include "ui/convert.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace denso::ui {
namespace {

/// Trim ASCII whitespace from both ends (matches Rust `str::trim`).
std::string_view trim(std::string_view s) {
    const auto is_ws = [](unsigned char c) { return std::isspace(c) != 0; };
    while (!s.empty() && is_ws(s.front())) s.remove_prefix(1);
    while (!s.empty() && is_ws(s.back())) s.remove_suffix(1);
    return s;
}

/// Trim, then map a blank field to "unset" — Rust's
/// `(!t.is_empty()).then(|| t.to_string())`.
std::optional<std::string> opt(const std::string& s) {
    const std::string_view t = trim(s);
    if (t.empty()) return std::nullopt;
    return std::string(t);
}

/// Parse the trimmed string as a `u32`, exactly as Rust's
/// `str::parse::<u32>().ok()` does: an optional leading '+', then digits only,
/// no overflow. Anything else yields "unset".
std::optional<uint32_t> parse_prefix(const std::string& s) {
    std::string_view t = trim(s);
    if (!t.empty() && t.front() == '+') t.remove_prefix(1);
    if (t.empty()) return std::nullopt;
    uint64_t value = 0;
    for (const unsigned char c : t) {
        if (c < '0' || c > '9') return std::nullopt;
        value = value * 10 + static_cast<uint64_t>(c - '0');
        if (value > UINT32_MAX) return std::nullopt;
    }
    return static_cast<uint32_t>(value);
}

} // namespace

NetStatus to_net_status(const network::InterfaceStatus& s) {
    return NetStatus{
        s.connected,
        s.ip,
        s.gateway,
        s.ssid,
        s.signal,
    };
}

NetConfigUi to_ui_config(const network::NetConfig& c) {
    return NetConfigUi{
        c.mode,
        c.ip.value_or(""),
        c.prefix ? std::to_string(*c.prefix) : "",
        c.gateway.value_or(""),
        c.dns1.value_or(""),
        c.dns2.value_or(""),
    };
}

network::NetConfig from_ui_config(const std::string& iface, const NetConfigUi& u) {
    network::NetConfig c;
    c.iface = iface;
    c.mode = u.mode;
    c.ip = opt(u.ip);
    c.prefix = parse_prefix(u.prefix);
    c.gateway = opt(u.gateway);
    c.dns1 = opt(u.dns1);
    c.dns2 = opt(u.dns2);
    c.ssid = std::nullopt;
    c.security = std::nullopt;
    return c;
}

std::vector<WifiRow> wifi_rows(const std::vector<network::WifiNetwork>& nets,
                               const std::string& current_ssid) {
    std::vector<WifiRow> rows;
    rows.reserve(nets.size());
    for (const auto& n : nets) {
        rows.push_back(WifiRow{
            n.ssid,
            n.signal,
            n.secured,
            !current_ssid.empty() && n.ssid == current_ssid,
        });
    }
    // Float the connected row to the top; stable so the rest keep scan order.
    std::stable_sort(rows.begin(), rows.end(),
                     [](const WifiRow& a, const WifiRow& b) { return a.connected && !b.connected; });
    return rows;
}

} // namespace denso::ui
