//! Boundary converters between the feature modules' domain types and the
//! Slint-generated view types. Feature modules never see Slint types — this is
//! the only place the two worlds meet.

use crate::network;
use crate::{NetConfigUi, NetStatus, WifiRow};

pub(super) fn to_net_status(s: &network::InterfaceStatus) -> NetStatus {
    NetStatus {
        connected: s.connected,
        ip: s.ip.clone().into(),
        gateway: s.gateway.clone().into(),
        ssid: s.ssid.clone().into(),
        signal: s.signal.clone().into(),
    }
}

/// Domain config → editable view model (empty string for unset fields).
pub(super) fn to_ui_config(c: &network::NetConfig) -> NetConfigUi {
    NetConfigUi {
        mode: c.mode.clone().into(),
        ip: c.ip.clone().unwrap_or_default().into(),
        prefix: c.prefix.map(|p| p.to_string()).unwrap_or_default().into(),
        gateway: c.gateway.clone().unwrap_or_default().into(),
        dns1: c.dns1.clone().unwrap_or_default().into(),
        dns2: c.dns2.clone().unwrap_or_default().into(),
    }
}

/// Editable view model → domain config. Blank fields become `None`; an
/// unparseable prefix becomes `None` (apply will reject a static config that
/// then lacks a prefix). Wi-Fi join fields are out of scope here.
pub(super) fn from_ui_config(iface: &str, u: &NetConfigUi) -> network::NetConfig {
    let opt = |s: &slint::SharedString| {
        let t = s.trim();
        (!t.is_empty()).then(|| t.to_string())
    };
    network::NetConfig {
        iface: iface.to_string(),
        mode: u.mode.to_string(),
        ip: opt(&u.ip),
        prefix: u.prefix.trim().parse::<u32>().ok(),
        gateway: opt(&u.gateway),
        dns1: opt(&u.dns1),
        dns2: opt(&u.dns2),
        ssid: None,
        security: None,
    }
}

/// Scanned networks → view-model rows: flag the currently-connected SSID and
/// sort it to the top (stable, so the rest keep their scan order).
pub(super) fn wifi_rows(nets: &[network::WifiNetwork], current_ssid: &str) -> Vec<WifiRow> {
    let mut rows: Vec<WifiRow> = nets
        .iter()
        .map(|n| WifiRow {
            ssid: n.ssid.clone().into(),
            signal: n.signal.clone().into(),
            secured: n.secured,
            connected: !current_ssid.is_empty() && n.ssid == current_ssid,
        })
        .collect();
    rows.sort_by_key(|r| !r.connected); // connected (false→0) first
    rows
}

#[cfg(test)]
mod tests {
    use super::*;

    fn net(ssid: &str) -> network::WifiNetwork {
        network::WifiNetwork {
            ssid: ssid.into(),
            signal: "50%".into(),
            secured: true,
        }
    }

    #[test]
    fn wifi_rows_floats_connected_to_top_and_flags_it() {
        let rows = wifi_rows(&[net("A"), net("HOME"), net("B")], "HOME");
        assert_eq!(rows[0].ssid.as_str(), "HOME");
        assert!(rows[0].connected);
        // the rest keep scan order and are not flagged
        assert_eq!(rows[1].ssid.as_str(), "A");
        assert_eq!(rows[2].ssid.as_str(), "B");
        assert!(!rows[1].connected && !rows[2].connected);
    }

    #[test]
    fn wifi_rows_marks_none_when_no_current_ssid() {
        let rows = wifi_rows(&[net("A"), net("B")], "");
        assert!(rows.iter().all(|r| !r.connected));
        assert_eq!(rows[0].ssid.as_str(), "A");
    }
}
