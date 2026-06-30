#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>
#include <vector>

#include "network/windows/wifi.h"

using denso::network::WifiNetwork;
using denso::network::wifi::build_profile_xml;
using denso::network::wifi::parse_wifi_networks;
using denso::network::wifi::xml_escape;

namespace {
const std::string NETWORKS = R"(Interface name : Wi-Fi
There are 2 networks currently visible.

SSID 1 : DENSO-FACTORY
    Network type            : Infrastructure
    Authentication          : WPA2-Personal
    Encryption              : CCMP
    BSSID 1                 : aa:bb:cc:dd:ee:ff
         Signal             : 72%
         Radio type         : 802.11ac

SSID 2 : OpenCafe
    Network type            : Infrastructure
    Authentication          : Open
    Encryption              : None
    BSSID 1                 : 11:22:33:44:55:66
         Signal             : 41%
)";

bool contains(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}
} // namespace

TEST_CASE("parse networks extracts ssid signal secured") {
    REQUIRE(parse_wifi_networks(NETWORKS) ==
            std::vector<WifiNetwork>{
                WifiNetwork{"DENSO-FACTORY", "72%", true},
                WifiNetwork{"OpenCafe", "41%", false},
            });
}

TEST_CASE("parse networks skips hidden empty ssid") {
    const std::string out = "SSID 1 : \n    Authentication : Open\n         Signal : 30%\n";
    REQUIRE(parse_wifi_networks(out).empty());
}

TEST_CASE("xml_escape encodes specials") {
    REQUIRE(xml_escape("a&b<c>\"d'") == "a&amp;b&lt;c&gt;&quot;d&apos;");
}

TEST_CASE("profile xml secured carries escaped psk") {
    const auto xml = build_profile_xml("MyNet", std::optional<std::string>("s3cret&pw"));
    REQUIRE(contains(xml, "<name>MyNet</name>"));
    REQUIRE(contains(xml, "WPA2PSK"));
    REQUIRE(contains(xml, "<keyMaterial>s3cret&amp;pw</keyMaterial>"));
}

TEST_CASE("profile xml open has no shared key") {
    const auto xml = build_profile_xml("OpenCafe", std::nullopt);
    REQUIRE(contains(xml, "<authentication>open</authentication>"));
    REQUIRE_FALSE(contains(xml, "sharedKey"));
}
