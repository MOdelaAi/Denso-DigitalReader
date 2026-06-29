//! Apply a [`NetConfig`] via `netsh interface ip`: build the ordered command
//! argument lists (pure, unit-tested) and run them. Also the raw command
//! runners shared with the backend.

use crate::network::NetConfig;

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
pub(super) fn build_netsh_commands(c: &NetConfig) -> Result<Vec<Vec<String>>, String> {
    let name = format!("name={}", adapter_name(&c.iface));
    let ip4 = |verb: &str, noun: &str| {
        vec![
            "interface".to_string(),
            "ip".to_string(),
            verb.to_string(),
            noun.to_string(),
        ]
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
            addr.extend([
                name.clone(),
                "static".to_string(),
                ip,
                prefix_to_mask(prefix),
            ]);
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

/// Run one `netsh` invocation, treating a non-zero exit — or netsh's stdout
/// error text — as failure.
pub(super) fn run_checked(args: &[String]) -> Result<(), String> {
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

pub(super) fn run(cmd: &str, args: &[&str]) -> String {
    std::process::Command::new(cmd)
        .args(args)
        .output()
        .map(|o| String::from_utf8_lossy(&o.stdout).into_owned())
        .unwrap_or_default()
}

#[cfg(test)]
mod tests {
    use super::*;

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
}
