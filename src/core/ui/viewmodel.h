// View-model structs: the C++ analog of the Slint-generated view types
// (`NetStatus`, `NetConfigUi`, `WifiRow` from `settings/network.slint`). The
// feature/domain modules never see these — they meet only in `convert`, which
// is the sole boundary between the domain types and the UI (ported 1:1 from the
// Rust `wiring/convert.rs`). Plain `std::string` so this layer and `convert`
// stay Qt-free and unit-testable; the widgets convert to/from QString.
#pragma once

#include <string>

namespace denso::ui {

/// Live, read-only status of one interface, shaped for display.
struct NetStatus {
    bool connected = false;
    std::string ip;
    std::string gateway;
    std::string ssid;
    std::string signal;

    bool operator==(const NetStatus&) const = default;
};

/// Editable network configuration for one interface (strings; the domain
/// layer parses). Empty string means an unset field.
struct NetConfigUi {
    std::string mode;  // "dhcp" | "static"
    std::string ip;
    std::string prefix;
    std::string gateway;
    std::string dns1;
    std::string dns2;

    bool operator==(const NetConfigUi&) const = default;
};

/// One scanned Wi-Fi network row.
struct WifiRow {
    std::string ssid;
    std::string signal;
    bool secured = false;
    bool connected = false;

    bool operator==(const WifiRow&) const = default;
};

} // namespace denso::ui
