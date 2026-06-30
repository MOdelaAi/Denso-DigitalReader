#include <catch2/catch_test_macros.hpp>

#include "network/model.h"
#include "ui/convert.h"

#include <algorithm>
#include <string>
#include <vector>

using denso::network::InterfaceStatus;
using denso::network::NetConfig;
using denso::network::WifiNetwork;
using denso::ui::from_ui_config;
using denso::ui::NetConfigUi;
using denso::ui::to_net_status;
using denso::ui::to_ui_config;
using denso::ui::wifi_rows;

namespace {
WifiNetwork net(const std::string& ssid) {
    return WifiNetwork{ssid, "50%", true};
}
} // namespace

TEST_CASE("wifi_rows floats connected to top and flags it") {
    const auto rows = wifi_rows({net("A"), net("HOME"), net("B")}, "HOME");
    REQUIRE(rows[0].ssid == "HOME");
    REQUIRE(rows[0].connected);
    // the rest keep scan order and are not flagged
    REQUIRE(rows[1].ssid == "A");
    REQUIRE(rows[2].ssid == "B");
    REQUIRE_FALSE(rows[1].connected);
    REQUIRE_FALSE(rows[2].connected);
}

TEST_CASE("wifi_rows marks none when no current ssid") {
    const auto rows = wifi_rows({net("A"), net("B")}, "");
    REQUIRE(std::none_of(rows.begin(), rows.end(), [](const auto& r) { return r.connected; }));
    REQUIRE(rows[0].ssid == "A");
}

// ── Extra coverage of the boundary mapping (round-trips the domain↔view edge).

TEST_CASE("to_net_status copies every field") {
    const InterfaceStatus s{true, "192.168.1.5", "192.168.1.1", "HOME", "80%"};
    const auto v = to_net_status(s);
    REQUIRE(v.connected);
    REQUIRE(v.ip == "192.168.1.5");
    REQUIRE(v.gateway == "192.168.1.1");
    REQUIRE(v.ssid == "HOME");
    REQUIRE(v.signal == "80%");
}

TEST_CASE("to_ui_config blanks unset fields and stringifies prefix") {
    NetConfig c;
    c.iface = "ethernet";
    c.mode = "static";
    c.ip = "10.0.0.2";
    c.prefix = 24u;
    // gateway/dns unset
    const auto u = to_ui_config(c);
    REQUIRE(u.mode == "static");
    REQUIRE(u.ip == "10.0.0.2");
    REQUIRE(u.prefix == "24");
    REQUIRE(u.gateway.empty());
    REQUIRE(u.dns1.empty());
    REQUIRE(u.dns2.empty());
}

TEST_CASE("from_ui_config trims, drops blanks, and parses prefix") {
    NetConfigUi u;
    u.mode = "static";
    u.ip = "  10.0.0.2 ";
    u.prefix = " 24 ";
    u.gateway = "";       // blank → unset
    u.dns1 = "8.8.8.8";
    u.dns2 = "   ";        // whitespace → unset
    const auto c = from_ui_config("wifi", u);
    REQUIRE(c.iface == "wifi");
    REQUIRE(c.mode == "static");
    REQUIRE(c.ip == "10.0.0.2");
    REQUIRE(c.prefix == 24u);
    REQUIRE_FALSE(c.gateway.has_value());
    REQUIRE(c.dns1 == "8.8.8.8");
    REQUIRE_FALSE(c.dns2.has_value());
    REQUIRE_FALSE(c.ssid.has_value());
    REQUIRE_FALSE(c.security.has_value());
}

TEST_CASE("from_ui_config rejects an unparseable prefix") {
    NetConfigUi u;
    u.mode = "static";
    u.prefix = "abc";
    REQUIRE_FALSE(from_ui_config("ethernet", u).prefix.has_value());

    NetConfigUi blank;
    blank.mode = "dhcp";
    REQUIRE_FALSE(from_ui_config("ethernet", blank).prefix.has_value());
}
