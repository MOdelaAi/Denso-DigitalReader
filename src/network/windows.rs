use super::{InterfaceStatus, NetConfig, NetworkBackend, NetworkSnapshot};

pub struct WindowsBackend;

impl NetworkBackend for WindowsBackend {
    fn snapshot(&self) -> NetworkSnapshot {
        let ipcfg = run("ipconfig", &[]);
        let wlan = run("netsh", &["wlan", "show", "interfaces"]);
        build_snapshot(&ipcfg, &wlan)
    }

    fn apply_config(&self, _config: &NetConfig) -> Result<(), String> {
        // TODO(device): `netsh interface ip set address/dns`. The privileged
        // apply path is verified on the Windows target, not this dev/CI path.
        // Until then, report unimplemented so boot-reassert surfaces it
        // non-fatally rather than silently claiming success.
        Err("network apply not yet implemented for Windows".into())
    }
}

fn run(cmd: &str, args: &[&str]) -> String {
    std::process::Command::new(cmd)
        .args(args)
        .output()
        .map(|o| String::from_utf8_lossy(&o.stdout).into_owned())
        .unwrap_or_default()
}

/// Value after the FIRST ':' on a "key : value" line, trimmed; strips a
/// trailing "(Preferred)". Splitting on the first colon (not the last) keeps
/// values that themselves contain ':' intact — e.g. an SSID like "Corp:Net"
/// or an IPv6 gateway "fe80::1".
fn value_after_colon(line: &str) -> String {
    match line.split_once(':') {
        Some((_, v)) => v.trim().replace("(Preferred)", "").trim().to_string(),
        None => String::new(),
    }
}

/// Parse `ipconfig` into (ethernet, wifi) IP/gateway. Adapter headers start at
/// column 0; their fields are indented. Wi-Fi headers contain "Wireless" or
/// "Wi-Fi"; wired headers contain "Ethernet".
fn parse_ipconfig(out: &str) -> (InterfaceStatus, InterfaceStatus) {
    let mut eth = InterfaceStatus::default();
    let mut wifi = InterfaceStatus::default();
    let mut target: Option<bool> = None; // Some(true)=wifi, Some(false)=eth
    for line in out.lines() {
        if !line.starts_with([' ', '\t']) && line.contains("adapter") {
            let h = line.to_lowercase();
            target = if h.contains("wireless") || h.contains("wi-fi") {
                Some(true)
            } else if h.contains("ethernet") {
                Some(false)
            } else {
                None
            };
            continue;
        }
        let Some(is_wifi) = target else { continue };
        let slot = if is_wifi { &mut wifi } else { &mut eth };
        let l = line.trim();
        if l.starts_with("IPv4 Address") {
            slot.ip = value_after_colon(line);
            slot.connected = !slot.ip.is_empty();
        } else if l.starts_with("Default Gateway") {
            let g = value_after_colon(line);
            if !g.is_empty() {
                slot.gateway = g;
            }
        }
    }
    (eth, wifi)
}

/// Parse `netsh wlan show interfaces` for (connected, ssid, signal).
fn parse_netsh_wlan(out: &str) -> (bool, String, String) {
    let mut connected = false;
    let mut ssid = String::new();
    let mut signal = String::new();
    for line in out.lines() {
        let l = line.trim();
        if l.starts_with("State") {
            connected = value_after_colon(line).eq_ignore_ascii_case("connected");
        } else if l.starts_with("SSID") && !l.starts_with("BSSID") {
            ssid = value_after_colon(line);
        } else if l.starts_with("Signal") {
            signal = value_after_colon(line);
        }
    }
    (connected, ssid, signal)
}

fn build_snapshot(ipcfg: &str, wlan: &str) -> NetworkSnapshot {
    let (eth, mut wifi) = parse_ipconfig(ipcfg);
    let (wconn, ssid, signal) = parse_netsh_wlan(wlan);
    wifi.connected = wifi.connected || wconn;
    wifi.ssid = ssid;
    wifi.signal = signal;
    NetworkSnapshot { ethernet: eth, wifi }
}

#[cfg(test)]
mod tests {
    use super::*;

    const IPCONFIG: &str = "\
Windows IP Configuration

Ethernet adapter Ethernet:

   IPv4 Address. . . . . . . . . . . : 192.168.1.50(Preferred)
   Subnet Mask . . . . . . . . . . . : 255.255.255.0
   Default Gateway . . . . . . . . . : 192.168.1.1

Wireless LAN adapter Wi-Fi:

   IPv4 Address. . . . . . . . . . . : 192.168.1.77(Preferred)
   Default Gateway . . . . . . . . . : 192.168.1.1
";

    const NETSH: &str = "\
There is 1 interface on the system:

    Name                   : Wi-Fi
    State                  : connected
    SSID                   : MyNetwork
    Signal                 : 72%
";

    #[test]
    fn ipconfig_extracts_eth_and_wifi() {
        let (eth, wifi) = parse_ipconfig(IPCONFIG);
        assert_eq!(eth.ip, "192.168.1.50");
        assert_eq!(eth.gateway, "192.168.1.1");
        assert!(eth.connected);
        assert_eq!(wifi.ip, "192.168.1.77");
    }

    #[test]
    fn netsh_extracts_ssid_signal_state() {
        let (connected, ssid, signal) = parse_netsh_wlan(NETSH);
        assert!(connected);
        assert_eq!(ssid, "MyNetwork");
        assert_eq!(signal, "72%");
    }

    #[test]
    fn value_with_colon_is_kept_intact() {
        // SSID containing a colon, and an IPv6-style gateway, must survive.
        assert_eq!(value_after_colon("    SSID                   : Corp:Net"), "Corp:Net");
        assert_eq!(value_after_colon("   Default Gateway . . . : fe80::1"), "fe80::1");
    }

    #[test]
    fn build_snapshot_merges_wifi() {
        let snap = build_snapshot(IPCONFIG, NETSH);
        assert_eq!(snap.ethernet.ip, "192.168.1.50");
        assert_eq!(snap.wifi.ssid, "MyNetwork");
        assert_eq!(snap.wifi.signal, "72%");
        assert!(snap.wifi.connected);
    }
}
