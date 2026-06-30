#include <catch2/catch_test_macros.hpp>

#include <string>

#include "network/windows/parse.h"

using denso::network::parse::build_snapshot;
using denso::network::parse::parse_ipconfig;
using denso::network::parse::parse_netsh_wlan;
using denso::network::parse::value_after_colon;

namespace {
const std::string IPCONFIG = R"(Windows IP Configuration

Ethernet adapter Ethernet:

   IPv4 Address. . . . . . . . . . . : 192.168.1.50(Preferred)
   Subnet Mask . . . . . . . . . . . : 255.255.255.0
   Default Gateway . . . . . . . . . : 192.168.1.1

Wireless LAN adapter Wi-Fi:

   IPv4 Address. . . . . . . . . . . : 192.168.1.77(Preferred)
   Default Gateway . . . . . . . . . : 192.168.1.1
)";

const std::string NETSH = R"(There is 1 interface on the system:

    Name                   : Wi-Fi
    State                  : connected
    SSID                   : MyNetwork
    Signal                 : 72%
)";
} // namespace

TEST_CASE("ipconfig extracts eth and wifi") {
    auto [eth, wifi] = parse_ipconfig(IPCONFIG);
    REQUIRE(eth.ip == "192.168.1.50");
    REQUIRE(eth.gateway == "192.168.1.1");
    REQUIRE(eth.connected);
    REQUIRE(wifi.ip == "192.168.1.77");
}

TEST_CASE("netsh extracts ssid signal state") {
    auto [connected, ssid, signal] = parse_netsh_wlan(NETSH);
    REQUIRE(connected);
    REQUIRE(ssid == "MyNetwork");
    REQUIRE(signal == "72%");
}

TEST_CASE("value with colon is kept intact") {
    REQUIRE(value_after_colon("    SSID                   : Corp:Net") == "Corp:Net");
    REQUIRE(value_after_colon("   Default Gateway . . . : fe80::1") == "fe80::1");
}

TEST_CASE("build_snapshot merges wifi") {
    auto snap = build_snapshot(IPCONFIG, NETSH);
    REQUIRE(snap.ethernet.ip == "192.168.1.50");
    REQUIRE(snap.wifi.ssid == "MyNetwork");
    REQUIRE(snap.wifi.signal == "72%");
    REQUIRE(snap.wifi.connected);
}
