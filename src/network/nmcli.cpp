#include "network/nmcli.h"

#include "strutil.h"

#include <cstddef>
#include <string>
#include <vector>

namespace denso::network::nmcli {
namespace {

using denso::strutil::split_lines;
using denso::strutil::trim;

/// Split on every ':' (Rust `str::split(':')`): a line with no colon yields one
/// element, so callers gate on the expected field count.
std::vector<std::string> split_colons(const std::string& s) {
    std::vector<std::string> out;
    size_t start = 0;
    while (true) {
        const size_t pos = s.find(':', start);
        if (pos == std::string::npos) {
            out.push_back(s.substr(start));
            break;
        }
        out.push_back(s.substr(start, pos - start));
        start = pos + 1;
    }
    return out;
}

} // namespace

std::pair<std::optional<std::string>, std::optional<std::string>> pick_devices(
    const std::string& out) {
    std::optional<std::string> eth;
    std::optional<std::string> wifi;
    for (const auto& line : split_lines(out)) {
        const auto f = split_colons(line);
        if (f.size() < 3) continue;
        const std::string& dev = f[0];
        const std::string& ty = f[1];
        const std::string& state = f[2];
        const bool connected = state == "connected";
        if (ty == "ethernet" && !eth && connected) {
            eth = dev;
        } else if (ty == "wifi" && !wifi && connected) {
            wifi = dev;
        }
    }
    return {eth, wifi};
}

std::pair<std::string, std::string> parse_device_show(const std::string& out) {
    std::string ip;
    std::string gateway;
    for (const auto& line : split_lines(out)) {
        const auto pos = line.find(':');
        if (pos == std::string::npos) continue;
        const std::string key = line.substr(0, pos);
        const std::string val = line.substr(pos + 1);
        if (key.starts_with("IP4.ADDRESS") && ip.empty()) {
            const auto slash = val.find('/');
            ip = trim(slash == std::string::npos ? val : val.substr(0, slash));
        } else if (key.starts_with("IP4.GATEWAY")) {
            gateway = trim(val);
        }
    }
    return {ip, gateway};
}

std::pair<std::string, std::string> parse_wifi(const std::string& out) {
    for (const auto& line : split_lines(out)) {
        const auto f = split_colons(line);
        if (f.size() >= 3 && f[0] == "yes") {
            return {f[1], f[2] + "%"};
        }
    }
    return {"", ""};
}

} // namespace denso::network::nmcli
