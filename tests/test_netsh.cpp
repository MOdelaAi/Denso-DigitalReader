#include <catch2/catch_test_macros.hpp>

#include <stdexcept>
#include <string>
#include <vector>

#include "network/netsh.h"

using denso::network::NetConfig;
using denso::network::netsh::build_netsh_commands;
using denso::network::netsh::prefix_to_mask;

namespace {
NetConfig cfg(const std::string& iface, const std::string& mode) {
    NetConfig c;
    c.iface = iface;
    c.mode = mode;
    return c;
}

/// Each built command joined back to a string, for readable assertions.
std::vector<std::string> lines(const NetConfig& c) {
    std::vector<std::string> out;
    for (const auto& cmd : build_netsh_commands(c)) {
        std::string s;
        for (size_t i = 0; i < cmd.size(); ++i) {
            if (i) s += ' ';
            s += cmd[i];
        }
        out.push_back(s);
    }
    return out;
}
} // namespace

TEST_CASE("prefix_to_mask common values") {
    REQUIRE(prefix_to_mask(24) == "255.255.255.0");
    REQUIRE(prefix_to_mask(16) == "255.255.0.0");
    REQUIRE(prefix_to_mask(8) == "255.0.0.0");
    REQUIRE(prefix_to_mask(25) == "255.255.255.128");
    REQUIRE(prefix_to_mask(32) == "255.255.255.255");
    REQUIRE(prefix_to_mask(0) == "0.0.0.0");
}

TEST_CASE("dhcp sets address and dns to dhcp") {
    REQUIRE(lines(cfg("ethernet", "dhcp")) ==
            std::vector<std::string>{
                "interface ip set address name=Ethernet dhcp",
                "interface ip set dns name=Ethernet dhcp",
            });
}

TEST_CASE("static builds address then dns with mask and gateway") {
    NetConfig c = cfg("wifi", "static");
    c.ip = "192.168.1.50";
    c.prefix = 24;
    c.gateway = "192.168.1.1";
    c.dns1 = "8.8.8.8";
    c.dns2 = "1.1.1.1";
    REQUIRE(lines(c) ==
            std::vector<std::string>{
                "interface ip set address name=Wi-Fi static 192.168.1.50 255.255.255.0 192.168.1.1",
                "interface ip set dns name=Wi-Fi static 8.8.8.8",
                "interface ip add dns name=Wi-Fi 1.1.1.1 index=2",
            });
}

TEST_CASE("static without gateway or dns emits only address") {
    NetConfig c = cfg("ethernet", "static");
    c.ip = "10.0.0.2";
    c.prefix = 8;
    REQUIRE(lines(c) ==
            std::vector<std::string>{"interface ip set address name=Ethernet static 10.0.0.2 255.0.0.0"});
}

TEST_CASE("static without ip is an error") {
    REQUIRE_THROWS_AS(build_netsh_commands(cfg("ethernet", "static")), std::runtime_error);
}
