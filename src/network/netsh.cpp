#include "network/netsh.h"

#include <stdexcept>

namespace denso::network::netsh {
namespace {

/// Map our logical interface id to the Windows adapter (connection) name.
/// TODO(device): detect the real names instead of assuming the defaults.
const char* adapter_name(const std::string& iface) {
    return iface == "wifi" ? "Wi-Fi" : "Ethernet";
}

std::optional<std::string> nonempty(const std::optional<std::string>& o) {
    if (o && !o->empty()) return o;
    return std::nullopt;
}

std::vector<std::string> ip4(const char* verb, const char* noun) {
    return {"interface", "ip", verb, noun};
}

} // namespace

std::string prefix_to_mask(uint32_t prefix) {
    uint32_t bits;
    if (prefix == 0) {
        bits = 0;
    } else if (prefix >= 32) {
        bits = 0xFFFFFFFFu;
    } else {
        bits = 0xFFFFFFFFu << (32 - prefix);
    }
    return std::to_string((bits >> 24) & 0xff) + "." + std::to_string((bits >> 16) & 0xff) + "." +
           std::to_string((bits >> 8) & 0xff) + "." + std::to_string(bits & 0xff);
}

std::vector<std::vector<std::string>> build_netsh_commands(const NetConfig& c) {
    const std::string name = std::string("name=") + adapter_name(c.iface);
    std::vector<std::vector<std::string>> cmds;

    if (c.mode == "dhcp") {
        auto addr = ip4("set", "address");
        addr.push_back(name);
        addr.push_back("dhcp");
        cmds.push_back(std::move(addr));

        auto dns = ip4("set", "dns");
        dns.push_back(name);
        dns.push_back("dhcp");
        cmds.push_back(std::move(dns));
    } else if (c.mode == "static") {
        const auto ip = nonempty(c.ip);
        if (!ip) throw std::runtime_error("static mode requires an IP address");
        if (!c.prefix) throw std::runtime_error("static mode requires a prefix length");

        auto addr = ip4("set", "address");
        addr.push_back(name);
        addr.push_back("static");
        addr.push_back(*ip);
        addr.push_back(prefix_to_mask(*c.prefix));
        if (const auto gw = nonempty(c.gateway)) addr.push_back(*gw);
        cmds.push_back(std::move(addr));

        if (const auto d1 = nonempty(c.dns1)) {
            auto dns = ip4("set", "dns");
            dns.push_back(name);
            dns.push_back("static");
            dns.push_back(*d1);
            cmds.push_back(std::move(dns));

            if (const auto d2 = nonempty(c.dns2)) {
                auto add = ip4("add", "dns");
                add.push_back(name);
                add.push_back(*d2);
                add.push_back("index=2");
                cmds.push_back(std::move(add));
            }
        }
    } else {
        throw std::runtime_error("unknown mode: " + c.mode);
    }
    return cmds;
}

} // namespace denso::network::netsh
