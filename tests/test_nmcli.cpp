#include <catch2/catch_test_macros.hpp>

#include "network/nmcli.h"

#include <optional>
#include <string>

using denso::network::nmcli::parse_device_show;
using denso::network::nmcli::parse_wifi;
using denso::network::nmcli::pick_devices;

TEST_CASE("picks first connected devices") {
    const std::string out =
        "eth0:ethernet:connected\nwlan0:wifi:connected\nlo:loopback:unmanaged\n";
    const auto [eth, wifi] = pick_devices(out);
    REQUIRE(eth == std::optional<std::string>("eth0"));
    REQUIRE(wifi == std::optional<std::string>("wlan0"));
}

TEST_CASE("parses device show ip gateway") {
    const std::string out = "IP4.ADDRESS[1]:192.168.1.50/24\nIP4.GATEWAY:192.168.1.1\n";
    const auto [ip, gw] = parse_device_show(out);
    REQUIRE(ip == "192.168.1.50");
    REQUIRE(gw == "192.168.1.1");
}

TEST_CASE("parses active wifi") {
    const std::string out = "no:OtherNet:40\nyes:MyNetwork:72\n";
    const auto [ssid, signal] = parse_wifi(out);
    REQUIRE(ssid == "MyNetwork");
    REQUIRE(signal == "72%");
}
