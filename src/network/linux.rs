use super::{InterfaceStatus, NetConfig, NetworkBackend, NetworkSnapshot, WifiNetwork};

pub struct LinuxBackend;

impl NetworkBackend for LinuxBackend {
    fn apply_config(&self, _config: &NetConfig) -> Result<(), String> {
        // TODO(device): nmcli `con mod`/`con up` (or netplan apply). Privileged
        // and verified on the Jetson/Pi target, not this dev path. Report
        // unimplemented so boot-reassert surfaces it non-fatally.
        Err("network apply not yet implemented for Linux".into())
    }

    fn scan_wifi(&self) -> Result<Vec<WifiNetwork>, String> {
        // TODO(device): nmcli -t -f SSID,SIGNAL,SECURITY device wifi list.
        Err("wifi scan not yet implemented for Linux".into())
    }

    fn connect_wifi(&self, _ssid: &str, _password: Option<&str>) -> Result<(), String> {
        // TODO(device): nmcli device wifi connect <ssid> [password <pw>].
        Err("wifi connect not yet implemented for Linux".into())
    }

    fn snapshot(&self) -> NetworkSnapshot {
        let dev = run("nmcli", &["-t", "-f", "DEVICE,TYPE,STATE", "device"]);
        let (eth_dev, wifi_dev) = pick_devices(&dev);

        let mut ethernet = InterfaceStatus::default();
        if let Some(d) = eth_dev {
            ethernet = device_ip(&d);
        }
        let mut wifi = InterfaceStatus::default();
        if let Some(d) = wifi_dev {
            wifi = device_ip(&d);
            let w = run("nmcli", &["-t", "-f", "ACTIVE,SSID,SIGNAL", "device", "wifi"]);
            let (ssid, signal) = parse_wifi(&w);
            wifi.ssid = ssid;
            wifi.signal = signal;
        }
        NetworkSnapshot { ethernet, wifi }
    }
}

fn run(cmd: &str, args: &[&str]) -> String {
    std::process::Command::new(cmd)
        .args(args)
        .output()
        .map(|o| String::from_utf8_lossy(&o.stdout).into_owned())
        .unwrap_or_default()
}

fn device_ip(dev: &str) -> InterfaceStatus {
    let out = run(
        "nmcli",
        &["-t", "-f", "IP4.ADDRESS,IP4.GATEWAY", "device", "show", dev],
    );
    let (ip, gateway) = parse_device_show(&out);
    InterfaceStatus {
        connected: !ip.is_empty(),
        ip,
        gateway,
        ..Default::default()
    }
}

/// From `nmcli -t -f DEVICE,TYPE,STATE device`, return the first connected
/// (device, _) for ethernet and wifi respectively.
fn pick_devices(out: &str) -> (Option<String>, Option<String>) {
    let mut eth = None;
    let mut wifi = None;
    for line in out.lines() {
        let f: Vec<&str> = line.split(':').collect();
        if f.len() < 3 {
            continue;
        }
        let (dev, ty, state) = (f[0], f[1], f[2]);
        let connected = state == "connected";
        if ty == "ethernet" && eth.is_none() && connected {
            eth = Some(dev.to_string());
        } else if ty == "wifi" && wifi.is_none() && connected {
            wifi = Some(dev.to_string());
        }
    }
    (eth, wifi)
}

/// From `nmcli -t -f IP4.ADDRESS,IP4.GATEWAY device show <dev>`, return
/// (ip-without-prefix, gateway).
fn parse_device_show(out: &str) -> (String, String) {
    let mut ip = String::new();
    let mut gateway = String::new();
    for line in out.lines() {
        if let Some((key, val)) = line.split_once(':') {
            if key.starts_with("IP4.ADDRESS") && ip.is_empty() {
                ip = val.split('/').next().unwrap_or("").trim().to_string();
            } else if key.starts_with("IP4.GATEWAY") {
                gateway = val.trim().to_string();
            }
        }
    }
    (ip, gateway)
}

/// From `nmcli -t -f ACTIVE,SSID,SIGNAL device wifi`, return the active
/// (ssid, "NN%").
fn parse_wifi(out: &str) -> (String, String) {
    for line in out.lines() {
        let f: Vec<&str> = line.split(':').collect();
        if f.len() >= 3 && f[0] == "yes" {
            return (f[1].to_string(), format!("{}%", f[2]));
        }
    }
    (String::new(), String::new())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn picks_first_connected_devices() {
        let out = "eth0:ethernet:connected\nwlan0:wifi:connected\nlo:loopback:unmanaged\n";
        let (eth, wifi) = pick_devices(out);
        assert_eq!(eth.as_deref(), Some("eth0"));
        assert_eq!(wifi.as_deref(), Some("wlan0"));
    }

    #[test]
    fn parses_device_show_ip_gateway() {
        let out = "IP4.ADDRESS[1]:192.168.1.50/24\nIP4.GATEWAY:192.168.1.1\n";
        let (ip, gw) = parse_device_show(out);
        assert_eq!(ip, "192.168.1.50");
        assert_eq!(gw, "192.168.1.1");
    }

    #[test]
    fn parses_active_wifi() {
        let out = "no:OtherNet:40\nyes:MyNetwork:72\n";
        let (ssid, signal) = parse_wifi(out);
        assert_eq!(ssid, "MyNetwork");
        assert_eq!(signal, "72%");
    }
}
