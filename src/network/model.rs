//! Network domain types. Two distinct datasets share the Network tab:
//! live **status** (read-only) and user-editable **config** (persisted, and
//! reasserted to the OS by the app).

/// User-editable, persisted network configuration for one interface. The app
/// owns this (source of truth) and reasserts it to the OS on boot. The WiFi
/// PSK is intentionally absent — it belongs in the OS secret store, never here.
#[derive(Clone, Debug, PartialEq)]
pub struct NetConfig {
    pub iface: String, // "ethernet" | "wifi"
    pub mode: String,  // "dhcp" | "static"
    pub ip: Option<String>,
    pub prefix: Option<u32>, // CIDR length, e.g. 24
    pub gateway: Option<String>,
    pub dns1: Option<String>,
    pub dns2: Option<String>,
    pub ssid: Option<String>,     // wifi only
    pub security: Option<String>, // wifi only (e.g. "wpa2"); the PSK is not stored
}

/// Live, read-only status of one interface, read from the OS.
#[derive(Default, Clone, PartialEq, Debug)]
pub struct InterfaceStatus {
    pub connected: bool,
    pub ip: String,
    pub gateway: String,
    pub ssid: String,
    pub signal: String,
}

/// Snapshot of both interfaces' status at a point in time.
#[derive(Default, Clone, Debug)]
pub struct NetworkSnapshot {
    pub ethernet: InterfaceStatus,
    pub wifi: InterfaceStatus,
}

/// One Wi-Fi network found by a scan. `secured` is false for open networks.
#[derive(Clone, Debug, PartialEq)]
pub struct WifiNetwork {
    pub ssid: String,
    pub signal: String,
    pub secured: bool,
}
