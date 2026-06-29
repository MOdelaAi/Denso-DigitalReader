//! Windows network backend. Drives the OS through `ipconfig` / `netsh` CLIs;
//! helpers split by concern: [`netsh`] (config apply), [`wifi`] (scan + join),
//! [`parse`] (status parsing).

use super::{NetConfig, NetworkBackend, NetworkSnapshot, WifiNetwork};

mod netsh;
mod parse;
mod wifi;

use netsh::{build_netsh_commands, run, run_checked};
use parse::build_snapshot;
use wifi::{build_profile_xml, parse_wifi_networks};

pub struct WindowsBackend;

impl NetworkBackend for WindowsBackend {
    fn snapshot(&self) -> NetworkSnapshot {
        let ipcfg = run("ipconfig", &[]);
        let wlan = run("netsh", &["wlan", "show", "interfaces"]);
        build_snapshot(&ipcfg, &wlan)
    }

    fn apply_config(&self, config: &NetConfig) -> Result<(), String> {
        for args in build_netsh_commands(config)? {
            run_checked(&args)?;
        }
        Ok(())
    }

    fn scan_wifi(&self) -> Result<Vec<WifiNetwork>, String> {
        let out = run("netsh", &["wlan", "show", "networks", "mode=bssid"]);
        Ok(parse_wifi_networks(&out))
    }

    fn connect_wifi(&self, ssid: &str, password: Option<&str>) -> Result<(), String> {
        // Hand the network (and PSK, if any) to the OS as a WLAN profile, then
        // connect. The key lives in the Windows credential store, not our DB.
        let xml = build_profile_xml(ssid, password);
        let path = std::env::temp_dir().join("denso_wlan_profile.xml");
        std::fs::write(&path, xml).map_err(|e| format!("write profile: {e}"))?;
        let add = run_checked(&[
            "wlan".into(),
            "add".into(),
            "profile".into(),
            format!("filename={}", path.to_string_lossy()),
        ]);
        let _ = std::fs::remove_file(&path);
        add?;
        run_checked(&[
            "wlan".into(),
            "connect".into(),
            format!("name={ssid}"),
            format!("ssid={ssid}"),
        ])
    }
}
