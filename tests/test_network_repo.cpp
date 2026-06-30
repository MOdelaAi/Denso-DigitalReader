#include <catch2/catch_test_macros.hpp>

#include "db/db.h"
#include "network/model.h"
#include "network/repo.h"

#include <string>
#include <vector>

using denso::db::Db;
using denso::db::run_migrations;
using denso::network::all;
using denso::network::load;
using denso::network::NetConfig;
using denso::network::save;

namespace {

Db db() {
    auto d = Db::open_in_memory();
    REQUIRE(d.has_value());
    REQUIRE(run_migrations(d->handle()));
    return std::move(*d);
}

NetConfig static_eth() {
    NetConfig c;
    c.iface = "ethernet";
    c.mode = "static";
    c.ip = "192.168.1.50";
    c.prefix = 24;
    c.gateway = "192.168.1.1";
    c.dns1 = "8.8.8.8";
    c.dns2 = "1.1.1.1";
    c.ssid = std::nullopt;
    c.security = std::nullopt;
    return c;
}

} // namespace

TEST_CASE("save then load roundtrips a static config") {
    const Db d = db();
    REQUIRE(save(d.handle(), static_eth()));
    const auto back = load(d.handle(), "ethernet");
    REQUIRE(back.has_value());
    REQUIRE(*back == static_eth());
}

TEST_CASE("load returns none when unset") {
    const Db d = db();
    REQUIRE(load(d.handle(), "wifi") == std::nullopt);
}

TEST_CASE("dhcp config roundtrips with empty fields") {
    const Db d = db();
    NetConfig dhcp;
    dhcp.iface = "wifi";
    dhcp.mode = "dhcp";
    dhcp.ssid = "denso-net";
    dhcp.security = "wpa2";
    // ip/prefix/gateway/dns1/dns2 left as nullopt
    REQUIRE(save(d.handle(), dhcp));
    const auto back = load(d.handle(), "wifi");
    REQUIRE(back.has_value());
    REQUIRE(*back == dhcp);
}

TEST_CASE("save overwrites existing interface") {
    const Db d = db();
    REQUIRE(save(d.handle(), static_eth()));
    NetConfig changed = static_eth();
    changed.ip = "10.0.0.9";
    REQUIRE(save(d.handle(), changed));
    const auto back = load(d.handle(), "ethernet");
    REQUIRE(back.has_value());
    REQUIRE(back->ip == std::optional<std::string>("10.0.0.9"));
    // still one row, not two
    REQUIRE(all(d.handle()).size() == 1);
}

TEST_CASE("all returns every interface ordered") {
    const Db d = db();
    NetConfig wifi = static_eth();
    wifi.iface = "wifi";
    REQUIRE(save(d.handle(), wifi));
    REQUIRE(save(d.handle(), static_eth()));
    const std::vector<NetConfig> configs = all(d.handle());
    std::vector<std::string> ifaces;
    for (const auto& c : configs) {
        ifaces.push_back(c.iface);
    }
    REQUIRE(ifaces == std::vector<std::string>{"ethernet", "wifi"});
}
