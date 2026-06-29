//! Network feature: live status (`snapshot`) plus user-editable config that
//! the app owns and reasserts to the OS (`reassert`). Domain types live in
//! [`model`], persistence in [`repo`], OS-specific work in the platform
//! adapters.

#[cfg(windows)]
mod windows;
#[cfg(target_os = "linux")]
mod linux;

mod model;
pub mod repo;

pub use model::{InterfaceStatus, NetConfig, NetworkSnapshot, WifiNetwork};

use rusqlite::Connection;

pub trait NetworkBackend {
    fn snapshot(&self) -> NetworkSnapshot;

    /// Push a saved configuration to the OS. Privileged (netsh / nmcli) and
    /// fallible; returns a human-readable error so the caller can surface it
    /// without crashing.
    fn apply_config(&self, config: &NetConfig) -> Result<(), String>;

    /// List Wi-Fi networks currently in range.
    fn scan_wifi(&self) -> Result<Vec<WifiNetwork>, String>;

    /// Join a Wi-Fi network. `password` is `None` for open networks; when
    /// present it is handed to the OS secret store, never persisted by us.
    fn connect_wifi(&self, ssid: &str, password: Option<&str>) -> Result<(), String>;
}

struct NullBackend;
impl NetworkBackend for NullBackend {
    fn snapshot(&self) -> NetworkSnapshot {
        NetworkSnapshot::default()
    }
    fn apply_config(&self, _config: &NetConfig) -> Result<(), String> {
        Ok(())
    }
    fn scan_wifi(&self) -> Result<Vec<WifiNetwork>, String> {
        Ok(Vec::new())
    }
    fn connect_wifi(&self, _ssid: &str, _password: Option<&str>) -> Result<(), String> {
        Ok(())
    }
}

pub fn backend() -> Box<dyn NetworkBackend> {
    #[cfg(windows)]
    {
        return Box::new(windows::WindowsBackend);
    }
    #[cfg(target_os = "linux")]
    {
        return Box::new(linux::LinuxBackend);
    }
    #[allow(unreachable_code)]
    {
        Box::new(NullBackend)
    }
}

/// Reassert every saved interface configuration to the OS — the app is the
/// source of truth, so this runs at boot. Best-effort and **non-fatal**: a
/// failed apply (no privilege, adapter error) is collected and returned as
/// `(iface, message)` rather than aborting startup.
pub fn reassert(conn: &Connection, backend: &dyn NetworkBackend) -> Vec<(String, String)> {
    let configs = match repo::all(conn) {
        Ok(c) => c,
        Err(e) => return vec![("<db>".to_string(), e.to_string())],
    };
    let mut errors = Vec::new();
    for c in configs {
        if let Err(e) = backend.apply_config(&c) {
            errors.push((c.iface, e));
        }
    }
    errors
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::cell::RefCell;

    #[test]
    fn null_backend_is_all_disconnected() {
        let snap = NullBackend.snapshot();
        assert!(!snap.ethernet.connected);
        assert!(!snap.wifi.connected);
        assert_eq!(snap.ethernet.ip, "");
    }

    /// Records which interfaces had `apply_config` called, and optionally fails
    /// one of them, so reassert's orchestration can be asserted off-device.
    struct FakeBackend {
        applied: RefCell<Vec<String>>,
        fail_iface: Option<String>,
    }
    impl FakeBackend {
        fn new() -> Self {
            FakeBackend { applied: RefCell::new(Vec::new()), fail_iface: None }
        }
        fn failing(iface: &str) -> Self {
            FakeBackend { applied: RefCell::new(Vec::new()), fail_iface: Some(iface.into()) }
        }
    }
    impl NetworkBackend for FakeBackend {
        fn snapshot(&self) -> NetworkSnapshot {
            NetworkSnapshot::default()
        }
        fn apply_config(&self, config: &NetConfig) -> Result<(), String> {
            self.applied.borrow_mut().push(config.iface.clone());
            if self.fail_iface.as_deref() == Some(config.iface.as_str()) {
                Err("boom".into())
            } else {
                Ok(())
            }
        }
        fn scan_wifi(&self) -> Result<Vec<WifiNetwork>, String> {
            Ok(Vec::new())
        }
        fn connect_wifi(&self, _ssid: &str, _password: Option<&str>) -> Result<(), String> {
            Ok(())
        }
    }

    fn db() -> Connection {
        let c = Connection::open_in_memory().unwrap();
        crate::db::run_migrations(&c).unwrap();
        c
    }

    fn cfg(iface: &str) -> NetConfig {
        NetConfig {
            iface: iface.into(),
            mode: "dhcp".into(),
            ip: None,
            prefix: None,
            gateway: None,
            dns1: None,
            dns2: None,
            ssid: None,
            security: None,
        }
    }

    #[test]
    fn reassert_applies_each_saved_config() {
        let c = db();
        repo::save(&c, &cfg("ethernet")).unwrap();
        repo::save(&c, &cfg("wifi")).unwrap();

        let backend = FakeBackend::new();
        let errors = reassert(&c, &backend);

        assert!(errors.is_empty());
        assert_eq!(*backend.applied.borrow(), vec!["ethernet", "wifi"]);
    }

    #[test]
    fn reassert_is_noop_with_no_saved_config() {
        let c = db();
        let backend = FakeBackend::new();
        let errors = reassert(&c, &backend);
        assert!(errors.is_empty());
        assert!(backend.applied.borrow().is_empty());
    }

    #[test]
    fn reassert_collects_errors_and_keeps_going() {
        let c = db();
        repo::save(&c, &cfg("ethernet")).unwrap();
        repo::save(&c, &cfg("wifi")).unwrap();

        let backend = FakeBackend::failing("ethernet");
        let errors = reassert(&c, &backend);

        // ethernet failed, but wifi was still attempted (non-fatal continue).
        assert_eq!(errors, vec![("ethernet".to_string(), "boom".to_string())]);
        assert_eq!(*backend.applied.borrow(), vec!["ethernet", "wifi"]);
    }
}
