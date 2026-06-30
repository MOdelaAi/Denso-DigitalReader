// OS network backend: the abstract contract plus the per-platform factory and
// the boot-time reassert. Live status (`snapshot`) and config push
// (`apply_config`/`scan_wifi`/`connect_wifi`) are OS-specific; the app owns the
// saved config and reasserts it on startup. Ported 1:1 from Rust `network`
// (the trait + `backend()` + `reassert`). Process execution moves from
// `std::process::Command` to `QProcess` in the platform backends.
//
// Errors mirror Rust's `Result::Err(String)` as a thrown `std::runtime_error`
// (the same convention `netsh::build_netsh_commands` already uses); `reassert`
// catches them and collects `(iface, message)` so a failed apply is non-fatal.
#pragma once

#include "network/model.h"

#include <QSqlDatabase>

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace denso::network {

class NetworkBackend {
public:
    virtual ~NetworkBackend() = default;

    virtual NetworkSnapshot snapshot() const = 0;

    /// Push a saved configuration to the OS. Privileged (netsh / nmcli) and
    /// fallible; throws `std::runtime_error` with a human-readable message so
    /// the caller can surface it without crashing.
    virtual void apply_config(const NetConfig& config) const = 0;

    /// List Wi-Fi networks currently in range. Throws on failure.
    virtual std::vector<WifiNetwork> scan_wifi() const = 0;

    /// Join a Wi-Fi network. `password` is `nullopt` for open networks; when
    /// present it is handed to the OS secret store, never persisted by us.
    /// Throws on failure.
    virtual void connect_wifi(const std::string& ssid,
                              const std::optional<std::string>& password) const = 0;
};

/// A backend that does nothing — every interface disconnected, every apply a
/// no-op. Used on platforms that are neither Windows nor Linux.
class NullBackend : public NetworkBackend {
public:
    NetworkSnapshot snapshot() const override { return {}; }
    void apply_config(const NetConfig&) const override {}
    std::vector<WifiNetwork> scan_wifi() const override { return {}; }
    void connect_wifi(const std::string&, const std::optional<std::string>&) const override {}
};

// Per-platform constructors. Exactly one is compiled (see CMakeLists); the
// other declaration is never odr-used, so it needs no definition.
std::unique_ptr<NetworkBackend> make_windows_backend();
std::unique_ptr<NetworkBackend> make_linux_backend();

/// The backend for the current OS — Windows, Linux, or a NullBackend fallback.
std::unique_ptr<NetworkBackend> backend();

/// Reassert every saved interface configuration to the OS — the app is the
/// source of truth, so this runs at boot. Best-effort and **non-fatal**: a
/// failed apply (no privilege, adapter error) is collected and returned as
/// `(iface, message)` rather than aborting startup.
std::vector<std::pair<std::string, std::string>> reassert(const QSqlDatabase& db,
                                                          const NetworkBackend& backend);

} // namespace denso::network
