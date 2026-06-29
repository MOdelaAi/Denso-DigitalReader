use super::{InterfaceStatus, NetConfig, NetworkBackend, NetworkSnapshot, WifiNetwork};

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

/// Map our logical interface id to the Windows adapter (connection) name.
/// TODO(device): detect the real names instead of assuming the defaults.
fn adapter_name(iface: &str) -> &'static str {
    match iface {
        "wifi" => "Wi-Fi",
        _ => "Ethernet",
    }
}

/// Convert a CIDR prefix length (0..=32) to a dotted IPv4 subnet mask.
fn prefix_to_mask(prefix: u32) -> String {
    let bits: u32 = match prefix {
        0 => 0,
        p if p >= 32 => u32::MAX,
        p => u32::MAX << (32 - p),
    };
    format!(
        "{}.{}.{}.{}",
        (bits >> 24) & 0xff,
        (bits >> 16) & 0xff,
        (bits >> 8) & 0xff,
        bits & 0xff
    )
}

/// Build the ordered `netsh` argument lists that apply `c` to its adapter.
/// Pure (no execution) so it can be unit-tested off a live network.
fn build_netsh_commands(c: &NetConfig) -> Result<Vec<Vec<String>>, String> {
    let name = format!("name={}", adapter_name(&c.iface));
    let ip4 = |verb: &str, noun: &str| {
        vec!["interface".to_string(), "ip".to_string(), verb.to_string(), noun.to_string()]
    };
    let nonempty = |o: &Option<String>| o.as_deref().filter(|s| !s.is_empty()).map(str::to_string);

    let mut cmds = Vec::new();
    match c.mode.as_str() {
        "dhcp" => {
            let mut addr = ip4("set", "address");
            addr.extend([name.clone(), "dhcp".to_string()]);
            cmds.push(addr);
            let mut dns = ip4("set", "dns");
            dns.extend([name, "dhcp".to_string()]);
            cmds.push(dns);
        }
        "static" => {
            let ip = nonempty(&c.ip).ok_or("static mode requires an IP address")?;
            let prefix = c.prefix.ok_or("static mode requires a prefix length")?;
            let mut addr = ip4("set", "address");
            addr.extend([name.clone(), "static".to_string(), ip, prefix_to_mask(prefix)]);
            if let Some(gw) = nonempty(&c.gateway) {
                addr.push(gw);
            }
            cmds.push(addr);

            if let Some(d1) = nonempty(&c.dns1) {
                let mut dns = ip4("set", "dns");
                dns.extend([name.clone(), "static".to_string(), d1]);
                cmds.push(dns);
                if let Some(d2) = nonempty(&c.dns2) {
                    let mut add = ip4("add", "dns");
                    add.extend([name, d2, "index=2".to_string()]);
                    cmds.push(add);
                }
            }
        }
        other => return Err(format!("unknown mode: {other}")),
    }
    Ok(cmds)
}

/// Parse `netsh wlan show networks mode=bssid` into the visible networks.
/// Pure, so it can be unit-tested off real hardware.
fn parse_wifi_networks(out: &str) -> Vec<WifiNetwork> {
    let mut nets = Vec::new();
    let mut cur: Option<WifiNetwork> = None;
    let flush = |cur: &mut Option<WifiNetwork>, nets: &mut Vec<WifiNetwork>| {
        if let Some(n) = cur.take() {
            if !n.ssid.is_empty() {
                nets.push(n);
            }
        }
    };
    for line in out.lines() {
        let l = line.trim();
        if l.starts_with("SSID ") && !l.starts_with("BSSID") {
            flush(&mut cur, &mut nets);
            cur = Some(WifiNetwork {
                ssid: value_after_colon(line),
                signal: String::new(),
                secured: false,
            });
        } else if let Some(n) = cur.as_mut() {
            if l.starts_with("Authentication") {
                n.secured = !value_after_colon(line).eq_ignore_ascii_case("Open");
            } else if l.starts_with("Signal") && n.signal.is_empty() {
                n.signal = value_after_colon(line);
            }
        }
    }
    flush(&mut cur, &mut nets);
    nets
}

/// Escape XML text content / attribute special characters.
fn xml_escape(s: &str) -> String {
    s.replace('&', "&amp;")
        .replace('<', "&lt;")
        .replace('>', "&gt;")
        .replace('"', "&quot;")
        .replace('\'', "&apos;")
}

/// Build a WLAN profile XML for `ssid`. `Some(pw)` produces a WPA2-PSK profile
/// carrying the key; `None` produces an open-network profile with no key.
fn build_profile_xml(ssid: &str, password: Option<&str>) -> String {
    let s = xml_escape(ssid);
    let (auth, enc, shared_key) = match password {
        Some(pw) => (
            "WPA2PSK",
            "AES",
            format!(
                "<sharedKey><keyType>passPhrase</keyType><protected>false</protected>\
                 <keyMaterial>{}</keyMaterial></sharedKey>",
                xml_escape(pw)
            ),
        ),
        None => ("open", "none", String::new()),
    };
    format!(
        "<?xml version=\"1.0\"?>\
         <WLANProfile xmlns=\"http://www.microsoft.com/networking/WLAN/profile/v1\">\
         <name>{s}</name>\
         <SSIDConfig><SSID><name>{s}</name></SSID></SSIDConfig>\
         <connectionType>ESS</connectionType><connectionMode>auto</connectionMode>\
         <MSM><security>\
         <authEncryption><authentication>{auth}</authentication>\
         <encryption>{enc}</encryption><useOneX>false</useOneX></authEncryption>\
         {shared_key}\
         </security></MSM></WLANProfile>"
    )
}

/// Run one `netsh` invocation, treating a non-zero exit — or netsh's stdout
/// error text — as failure.
fn run_checked(args: &[String]) -> Result<(), String> {
    let out = std::process::Command::new("netsh")
        .args(args)
        .output()
        .map_err(|e| format!("failed to spawn netsh: {e}"))?;
    if out.status.success() {
        Ok(())
    } else {
        let stderr = String::from_utf8_lossy(&out.stderr);
        let detail = if stderr.trim().is_empty() {
            String::from_utf8_lossy(&out.stdout).into_owned()
        } else {
            stderr.into_owned()
        };
        Err(format!("netsh {}: {}", args.join(" "), detail.trim()))
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

    fn cfg(iface: &str, mode: &str) -> NetConfig {
        NetConfig {
            iface: iface.into(),
            mode: mode.into(),
            ip: None,
            prefix: None,
            gateway: None,
            dns1: None,
            dns2: None,
            ssid: None,
            security: None,
        }
    }

    /// Each built command joined back to a string, for readable assertions.
    fn lines(c: &NetConfig) -> Vec<String> {
        build_netsh_commands(c)
            .unwrap()
            .iter()
            .map(|a| a.join(" "))
            .collect()
    }

    #[test]
    fn prefix_to_mask_common_values() {
        assert_eq!(prefix_to_mask(24), "255.255.255.0");
        assert_eq!(prefix_to_mask(16), "255.255.0.0");
        assert_eq!(prefix_to_mask(8), "255.0.0.0");
        assert_eq!(prefix_to_mask(25), "255.255.255.128");
        assert_eq!(prefix_to_mask(32), "255.255.255.255");
        assert_eq!(prefix_to_mask(0), "0.0.0.0");
    }

    #[test]
    fn dhcp_sets_address_and_dns_to_dhcp() {
        assert_eq!(
            lines(&cfg("ethernet", "dhcp")),
            vec![
                "interface ip set address name=Ethernet dhcp",
                "interface ip set dns name=Ethernet dhcp",
            ]
        );
    }

    #[test]
    fn static_builds_address_then_dns_with_mask_and_gateway() {
        let mut c = cfg("wifi", "static");
        c.ip = Some("192.168.1.50".into());
        c.prefix = Some(24);
        c.gateway = Some("192.168.1.1".into());
        c.dns1 = Some("8.8.8.8".into());
        c.dns2 = Some("1.1.1.1".into());
        assert_eq!(
            lines(&c),
            vec![
                "interface ip set address name=Wi-Fi static 192.168.1.50 255.255.255.0 192.168.1.1",
                "interface ip set dns name=Wi-Fi static 8.8.8.8",
                "interface ip add dns name=Wi-Fi 1.1.1.1 index=2",
            ]
        );
    }

    #[test]
    fn static_without_gateway_or_dns_emits_only_address() {
        let mut c = cfg("ethernet", "static");
        c.ip = Some("10.0.0.2".into());
        c.prefix = Some(8);
        assert_eq!(
            lines(&c),
            vec!["interface ip set address name=Ethernet static 10.0.0.2 255.0.0.0"]
        );
    }

    #[test]
    fn static_without_ip_is_an_error() {
        let err = build_netsh_commands(&cfg("ethernet", "static")).unwrap_err();
        assert!(err.to_lowercase().contains("ip"), "got: {err}");
    }

    const NETWORKS: &str = "\
Interface name : Wi-Fi
There are 2 networks currently visible.

SSID 1 : DENSO-FACTORY
    Network type            : Infrastructure
    Authentication          : WPA2-Personal
    Encryption              : CCMP
    BSSID 1                 : aa:bb:cc:dd:ee:ff
         Signal             : 72%
         Radio type         : 802.11ac

SSID 2 : OpenCafe
    Network type            : Infrastructure
    Authentication          : Open
    Encryption              : None
    BSSID 1                 : 11:22:33:44:55:66
         Signal             : 41%
";

    #[test]
    fn parse_networks_extracts_ssid_signal_secured() {
        let nets = parse_wifi_networks(NETWORKS);
        assert_eq!(
            nets,
            vec![
                WifiNetwork { ssid: "DENSO-FACTORY".into(), signal: "72%".into(), secured: true },
                WifiNetwork { ssid: "OpenCafe".into(), signal: "41%".into(), secured: false },
            ]
        );
    }

    #[test]
    fn parse_networks_skips_hidden_empty_ssid() {
        let out = "SSID 1 : \n    Authentication : Open\n         Signal : 30%\n";
        assert!(parse_wifi_networks(out).is_empty());
    }

    #[test]
    fn xml_escape_encodes_specials() {
        assert_eq!(xml_escape("a&b<c>\"d'"), "a&amp;b&lt;c&gt;&quot;d&apos;");
    }

    #[test]
    fn profile_xml_secured_carries_escaped_psk() {
        let xml = build_profile_xml("MyNet", Some("s3cret&pw"));
        assert!(xml.contains("<name>MyNet</name>"), "got: {xml}");
        assert!(xml.contains("WPA2PSK"), "got: {xml}");
        assert!(xml.contains("<keyMaterial>s3cret&amp;pw</keyMaterial>"), "got: {xml}");
    }

    #[test]
    fn profile_xml_open_has_no_shared_key() {
        let xml = build_profile_xml("OpenCafe", None);
        assert!(xml.contains("<authentication>open</authentication>"), "got: {xml}");
        assert!(!xml.contains("sharedKey"), "got: {xml}");
    }
}
