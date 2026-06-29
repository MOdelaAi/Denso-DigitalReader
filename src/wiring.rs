//! UI callback wiring, kept out of `main` so the entry point stays a thin
//! orchestrator (build window → init DB → wire → run). Maps between the
//! feature modules' domain types and the Slint-generated view types here, at
//! the boundary — feature modules never see Slint types.

use crate::settings::Settings;
use crate::{hardware, network, settings};
use crate::{AppWindow, NetConfigUi, NetStatus, Theme, WifiRow};
use rusqlite::Connection;
use slint::{ComponentHandle, ModelRc, VecModel};
use std::cell::RefCell;
use std::rc::Rc;

type State = Rc<RefCell<Settings>>;

fn to_net_status(s: &network::InterfaceStatus) -> NetStatus {
    NetStatus {
        connected: s.connected,
        ip: s.ip.clone().into(),
        gateway: s.gateway.clone().into(),
        ssid: s.ssid.clone().into(),
        signal: s.signal.clone().into(),
    }
}

/// Domain config → editable view model (empty string for unset fields).
fn to_ui_config(c: &network::NetConfig) -> NetConfigUi {
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
fn from_ui_config(iface: &str, u: &NetConfigUi) -> network::NetConfig {
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
fn wifi_rows(nets: &[network::WifiNetwork], current_ssid: &str) -> Vec<WifiRow> {
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

/// Saved config for `iface`, or a DHCP default when none is stored yet.
fn load_config_or_default(conn: &Connection, iface: &str) -> network::NetConfig {
    network::repo::load(conn, iface)
        .ok()
        .flatten()
        .unwrap_or_else(|| network::NetConfig {
            iface: iface.to_string(),
            mode: "dhcp".to_string(),
            ip: None,
            prefix: None,
            gateway: None,
            dns1: None,
            dns2: None,
            ssid: None,
            security: None,
        })
}

/// Populate read-only fields (version, hardware) and apply persisted settings
/// to the window before it is shown.
pub fn apply_startup(app: &AppWindow, db_conn: &Rc<Connection>, state: &State) {
    app.set_app_version(env!("CARGO_PKG_VERSION").into());

    let hw = hardware::collect();
    app.set_hw_os(hw.os.into());
    app.set_hw_device(hw.device.into());
    app.set_hw_ram(hw.ram.into());
    app.set_hw_storage(hw.storage.into());

    // Seed the network-config editors from saved config (DHCP if unset).
    app.set_eth_config(to_ui_config(&load_config_or_default(db_conn, "ethernet")));
    app.set_wifi_config(to_ui_config(&load_config_or_default(db_conn, "wifi")));

    let s = state.borrow();
    app.window()
        .set_size(slint::LogicalSize::new(s.width as f32, s.height as f32));
    app.window().set_fullscreen(s.fullscreen);
    app.set_resolution_index(settings::preset_index(s.width, s.height));
    app.set_fullscreen(s.fullscreen);
    app.global::<Theme>().set_dark(s.dark);
}

/// Install all UI callbacks: settings persistence (resolution/theme/fullscreen/
/// reset) and the async network status refresh.
pub fn install_handlers(app: &AppWindow, db_conn: &Rc<Connection>, state: &State) {
    // Resolution Apply — resize + persist (preserves other fields).
    let weak = app.as_weak();
    let st = state.clone();
    let dbc = db_conn.clone();
    app.on_apply_resolution(move |index| {
        let (w, h) = settings::PRESETS[index as usize];
        {
            let mut s = st.borrow_mut();
            s.width = w;
            s.height = h;
            settings::save(&dbc, &s);
        }
        if let Some(app) = weak.upgrade() {
            app.window()
                .set_size(slint::LogicalSize::new(w as f32, h as f32));
        }
    });

    // Theme toggle — instant persist.
    let st = state.clone();
    let dbc = db_conn.clone();
    app.on_theme_changed(move |dark| {
        let mut s = st.borrow_mut();
        s.dark = dark;
        settings::save(&dbc, &s);
    });

    // Fullscreen toggle — apply + persist.
    let weak = app.as_weak();
    let st = state.clone();
    let dbc = db_conn.clone();
    app.on_toggle_fullscreen(move |fullscreen| {
        {
            let mut s = st.borrow_mut();
            s.fullscreen = fullscreen;
            settings::save(&dbc, &s);
        }
        if let Some(app) = weak.upgrade() {
            app.window().set_fullscreen(fullscreen);
        }
    });

    // Reset to defaults — persist defaults and reflect them in the UI.
    let weak = app.as_weak();
    let st = state.clone();
    let dbc = db_conn.clone();
    app.on_reset_defaults(move || {
        let d = Settings::default();
        settings::save(&dbc, &d);
        if let Some(app) = weak.upgrade() {
            app.window().set_fullscreen(d.fullscreen);
            app.window()
                .set_size(slint::LogicalSize::new(d.width as f32, d.height as f32));
            app.set_resolution_index(settings::preset_index(d.width, d.height));
            app.set_fullscreen(d.fullscreen);
            app.global::<Theme>().set_dark(d.dark);
        }
        *st.borrow_mut() = d;
    });

    // Apply network config — persist (app owns the truth) then push to the OS,
    // surfacing the outcome as the per-interface status line.
    let weak = app.as_weak();
    let dbc = db_conn.clone();
    app.on_apply_net_config(move |iface, ui| {
        let cfg = from_ui_config(iface.as_str(), &ui);
        let _ = network::repo::save(&dbc, &cfg);
        let result = network::backend().apply_config(&cfg);
        if let Some(app) = weak.upgrade() {
            let status: slint::SharedString = match &result {
                Ok(()) => "Applied".into(),
                Err(e) => format!("Error: {e}").into(),
            };
            let ui_cfg = to_ui_config(&cfg);
            if iface.as_str() == "wifi" {
                app.set_wifi_config(ui_cfg);
                app.set_wifi_config_status(status);
            } else {
                app.set_eth_config(ui_cfg);
                app.set_eth_config_status(status);
            }
        }
    });

    // Wi-Fi scan — runs off-thread (netsh blocks), posts the list back.
    let weak = app.as_weak();
    app.on_scan_wifi(move || {
        let Some(app) = weak.upgrade() else {
            return;
        };
        app.set_wifi_scanning(true);
        // Last-known connected SSID (from status), to float it to the top.
        let current_ssid = app.get_wifi().ssid.to_string();
        let weak2 = app.as_weak();
        std::thread::spawn(move || {
            let result = network::backend().scan_wifi();
            let _ = weak2.upgrade_in_event_loop(move |app| {
                match result {
                    Ok(nets) => {
                        let rows = wifi_rows(&nets, &current_ssid);
                        app.set_wifi_networks(ModelRc::new(VecModel::from(rows)));
                    }
                    Err(e) => app.set_wifi_connect_status(format!("Scan failed: {e}").into()),
                }
                app.set_wifi_scanning(false);
            });
        });
    });

    // Wi-Fi connect — off-thread; empty password means an open network.
    let weak = app.as_weak();
    app.on_connect_wifi(move |ssid, password| {
        let Some(app) = weak.upgrade() else {
            return;
        };
        app.set_wifi_connect_status(format!("Connecting to {ssid}…").into());
        let ssid = ssid.to_string();
        let password = password.to_string();
        let weak2 = app.as_weak();
        std::thread::spawn(move || {
            let pw = (!password.is_empty()).then_some(password.as_str());
            let result = network::backend().connect_wifi(&ssid, pw);
            let _ = weak2.upgrade_in_event_loop(move |app| {
                app.set_wifi_connect_status(match result {
                    Ok(()) => format!("Connected to {ssid}"),
                    Err(e) => format!("Error: {e}"),
                }
                .into());
            });
        });
    });

    // Network status refresh — runs off-thread, posts results back to the UI.
    let weak = app.as_weak();
    app.on_refresh_network(move || {
        let Some(app) = weak.upgrade() else {
            return;
        };
        app.set_net_loading(true);
        let weak2 = app.as_weak();
        std::thread::spawn(move || {
            let snap = network::backend().snapshot();
            let _ = weak2.upgrade_in_event_loop(move |app| {
                app.set_eth(to_net_status(&snap.ethernet));
                app.set_wifi(to_net_status(&snap.wifi));
                app.set_net_loading(false);
            });
        });
    });
}

#[cfg(test)]
mod tests {
    use super::*;

    fn net(ssid: &str) -> network::WifiNetwork {
        network::WifiNetwork { ssid: ssid.into(), signal: "50%".into(), secured: true }
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
