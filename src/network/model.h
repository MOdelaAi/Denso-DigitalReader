// Network domain types. Ported 1:1 from the Rust `network::model`. Plain
// std types (no Qt) so the logic core stays portable and unit-testable.
#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace denso::network {

// ─── DB (persisted → net_config table) ───────────────────────────────────────

/// User-editable config for one interface. The app owns this (source of truth)
/// and reasserts it to the OS on boot. The WiFi PSK is intentionally absent —
/// it belongs in the OS secret store, never here.
struct NetConfig {
    std::string iface;                    // "ethernet" | "wifi"
    std::string mode;                     // "dhcp" | "static"
    std::optional<std::string> ip;
    std::optional<uint32_t> prefix;       // CIDR length, e.g. 24
    std::optional<std::string> gateway;
    std::optional<std::string> dns1;
    std::optional<std::string> dns2;
    std::optional<std::string> ssid;      // wifi only
    std::optional<std::string> security;  // wifi only (e.g. "wpa2"); PSK not stored

    bool operator==(const NetConfig&) const = default;
};

// ─── Runtime (transient — never stored) ──────────────────────────────────────

/// Live, read-only status of one interface, read from the OS.
struct InterfaceStatus {
    bool connected = false;
    std::string ip;
    std::string gateway;
    std::string ssid;
    std::string signal;

    bool operator==(const InterfaceStatus&) const = default;
};

/// Snapshot of both interfaces' status at a point in time.
struct NetworkSnapshot {
    InterfaceStatus ethernet;
    InterfaceStatus wifi;
};

/// One Wi-Fi network found by a scan. Temporary — discarded after UI update.
struct WifiNetwork {
    std::string ssid;
    std::string signal;
    bool secured = false;

    bool operator==(const WifiNetwork&) const = default;
};

} // namespace denso::network
