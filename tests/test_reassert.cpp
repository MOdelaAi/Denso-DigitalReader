#include <catch2/catch_test_macros.hpp>

#include "db/db.h"
#include "network/backend.h"
#include "network/model.h"
#include "network/repo.h"

#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using denso::db::Db;
using denso::db::run_migrations;
using denso::network::NetConfig;
using denso::network::NetworkBackend;
using denso::network::NetworkSnapshot;
using denso::network::NullBackend;
using denso::network::reassert;
using denso::network::WifiNetwork;

namespace {

/// Records which interfaces had `apply_config` called, and optionally fails one
/// of them, so reassert's orchestration can be asserted off-device.
class FakeBackend : public NetworkBackend {
public:
    explicit FakeBackend(std::optional<std::string> fail_iface = std::nullopt)
        : fail_iface_(std::move(fail_iface)) {}

    NetworkSnapshot snapshot() const override { return {}; }
    void apply_config(const NetConfig& config) const override {
        applied_.push_back(config.iface);
        if (fail_iface_ && *fail_iface_ == config.iface) {
            throw std::runtime_error("boom");
        }
    }
    std::vector<WifiNetwork> scan_wifi() const override { return {}; }
    void connect_wifi(const std::string&, const std::optional<std::string>&) const override {}

    const std::vector<std::string>& applied() const { return applied_; }

private:
    mutable std::vector<std::string> applied_;
    std::optional<std::string> fail_iface_;
};

Db db() {
    auto d = Db::open_in_memory();
    REQUIRE(d.has_value());
    REQUIRE(run_migrations(d->handle()));
    return std::move(*d);
}

NetConfig cfg(const std::string& iface) {
    NetConfig c;
    c.iface = iface;
    c.mode = "dhcp";
    return c;
}

} // namespace

TEST_CASE("null backend is all disconnected") {
    const NullBackend nb;
    const NetworkSnapshot snap = nb.snapshot();
    REQUIRE(!snap.ethernet.connected);
    REQUIRE(!snap.wifi.connected);
    REQUIRE(snap.ethernet.ip == "");
}

TEST_CASE("reassert applies each saved config") {
    const Db d = db();
    REQUIRE(denso::network::save(d.handle(), cfg("ethernet")));
    REQUIRE(denso::network::save(d.handle(), cfg("wifi")));

    const FakeBackend backend;
    const auto errors = reassert(d.handle(), backend);

    REQUIRE(errors.empty());
    REQUIRE(backend.applied() == std::vector<std::string>{"ethernet", "wifi"});
}

TEST_CASE("reassert is noop with no saved config") {
    const Db d = db();
    const FakeBackend backend;
    const auto errors = reassert(d.handle(), backend);
    REQUIRE(errors.empty());
    REQUIRE(backend.applied().empty());
}

TEST_CASE("reassert collects errors and keeps going") {
    const Db d = db();
    REQUIRE(denso::network::save(d.handle(), cfg("ethernet")));
    REQUIRE(denso::network::save(d.handle(), cfg("wifi")));

    const FakeBackend backend(std::optional<std::string>("ethernet"));
    const auto errors = reassert(d.handle(), backend);

    // ethernet failed, but wifi was still attempted (non-fatal continue).
    using Err = std::pair<std::string, std::string>;
    REQUIRE(errors == std::vector<Err>{{"ethernet", "boom"}});
    REQUIRE(backend.applied() == std::vector<std::string>{"ethernet", "wifi"});
}
