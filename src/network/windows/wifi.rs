//! Wi-Fi support: parse `netsh wlan show networks` into [`WifiNetwork`]s and
//! build the WLAN profile XML used to join a network.

use super::parse::value_after_colon;
use crate::network::WifiNetwork;

/// Parse `netsh wlan show networks mode=bssid` into the visible networks.
/// Pure, so it can be unit-tested off real hardware.
pub(super) fn parse_wifi_networks(out: &str) -> Vec<WifiNetwork> {
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
pub(super) fn build_profile_xml(ssid: &str, password: Option<&str>) -> String {
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

#[cfg(test)]
mod tests {
    use super::*;

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
                WifiNetwork {
                    ssid: "DENSO-FACTORY".into(),
                    signal: "72%".into(),
                    secured: true
                },
                WifiNetwork {
                    ssid: "OpenCafe".into(),
                    signal: "41%".into(),
                    secured: false
                },
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
        assert!(
            xml.contains("<keyMaterial>s3cret&amp;pw</keyMaterial>"),
            "got: {xml}"
        );
    }

    #[test]
    fn profile_xml_open_has_no_shared_key() {
        let xml = build_profile_xml("OpenCafe", None);
        assert!(
            xml.contains("<authentication>open</authentication>"),
            "got: {xml}"
        );
        assert!(!xml.contains("sharedKey"), "got: {xml}");
    }
}
