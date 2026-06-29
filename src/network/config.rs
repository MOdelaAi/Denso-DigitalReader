//! Persistence for [`NetConfig`] in the SQLite `net_config` table. User-
//! editable (full CRUD), one row per interface. Mirrors the `settings`
//! repo: typed struct in Rust, rows in the DB, errors surfaced as `Result`
//! so callers can report a failed write.

use super::NetConfig;
use rusqlite::Connection;

// `save`/`load` await the network-config editor UI; `all` is already used by
// boot reassert. Tested now, wired when the editor lands.
/// Upsert one interface's configuration, keyed by `iface`.
#[allow(dead_code)]
pub fn save(conn: &Connection, c: &NetConfig) -> rusqlite::Result<()> {
    conn.execute(
        "INSERT INTO net_config (iface, mode, ip, prefix, gateway, dns1, dns2, ssid, security) \
         VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9) \
         ON CONFLICT(iface) DO UPDATE SET \
             mode=excluded.mode, ip=excluded.ip, prefix=excluded.prefix, \
             gateway=excluded.gateway, dns1=excluded.dns1, dns2=excluded.dns2, \
             ssid=excluded.ssid, security=excluded.security",
        rusqlite::params![
            c.iface, c.mode, c.ip, c.prefix, c.gateway, c.dns1, c.dns2, c.ssid, c.security
        ],
    )?;
    Ok(())
}

fn from_row(row: &rusqlite::Row) -> rusqlite::Result<NetConfig> {
    Ok(NetConfig {
        iface: row.get(0)?,
        mode: row.get(1)?,
        ip: row.get(2)?,
        prefix: row.get(3)?,
        gateway: row.get(4)?,
        dns1: row.get(5)?,
        dns2: row.get(6)?,
        ssid: row.get(7)?,
        security: row.get(8)?,
    })
}

const COLUMNS: &str = "iface, mode, ip, prefix, gateway, dns1, dns2, ssid, security";

/// Load one interface's saved configuration, or `None` if unset.
#[allow(dead_code)]
pub fn load(conn: &Connection, iface: &str) -> rusqlite::Result<Option<NetConfig>> {
    conn.query_row(
        &format!("SELECT {COLUMNS} FROM net_config WHERE iface = ?1"),
        [iface],
        from_row,
    )
    .map(Some)
    .or_else(|e| match e {
        rusqlite::Error::QueryReturnedNoRows => Ok(None),
        other => Err(other),
    })
}

/// Every saved interface configuration, ordered by interface name.
pub fn all(conn: &Connection) -> rusqlite::Result<Vec<NetConfig>> {
    let mut stmt = conn.prepare(&format!("SELECT {COLUMNS} FROM net_config ORDER BY iface"))?;
    let rows = stmt.query_map([], from_row)?;
    rows.collect()
}

#[cfg(test)]
mod tests {
    use super::*;

    fn db() -> Connection {
        let c = Connection::open_in_memory().unwrap();
        crate::db::run_migrations(&c).unwrap();
        c
    }

    fn static_eth() -> NetConfig {
        NetConfig {
            iface: "ethernet".into(),
            mode: "static".into(),
            ip: Some("192.168.1.50".into()),
            prefix: Some(24),
            gateway: Some("192.168.1.1".into()),
            dns1: Some("8.8.8.8".into()),
            dns2: Some("1.1.1.1".into()),
            ssid: None,
            security: None,
        }
    }

    #[test]
    fn save_then_load_roundtrips_a_static_config() {
        let c = db();
        save(&c, &static_eth()).unwrap();
        let back = load(&c, "ethernet").unwrap().unwrap();
        assert_eq!(back, static_eth());
    }

    #[test]
    fn load_returns_none_when_unset() {
        let c = db();
        assert_eq!(load(&c, "wifi").unwrap(), None);
    }

    #[test]
    fn dhcp_config_roundtrips_with_empty_fields() {
        let c = db();
        let dhcp = NetConfig {
            iface: "wifi".into(),
            mode: "dhcp".into(),
            ip: None,
            prefix: None,
            gateway: None,
            dns1: None,
            dns2: None,
            ssid: Some("denso-net".into()),
            security: Some("wpa2".into()),
        };
        save(&c, &dhcp).unwrap();
        assert_eq!(load(&c, "wifi").unwrap().unwrap(), dhcp);
    }

    #[test]
    fn save_overwrites_existing_interface() {
        let c = db();
        save(&c, &static_eth()).unwrap();
        let mut changed = static_eth();
        changed.ip = Some("10.0.0.9".into());
        save(&c, &changed).unwrap();
        assert_eq!(load(&c, "ethernet").unwrap().unwrap().ip, Some("10.0.0.9".into()));
        // still one row, not two
        assert_eq!(all(&c).unwrap().len(), 1);
    }

    #[test]
    fn all_returns_every_interface_ordered() {
        let c = db();
        let mut wifi = static_eth();
        wifi.iface = "wifi".into();
        save(&c, &wifi).unwrap();
        save(&c, &static_eth()).unwrap();
        let ifaces: Vec<String> = all(&c).unwrap().into_iter().map(|c| c.iface).collect();
        assert_eq!(ifaces, vec!["ethernet", "wifi"]);
    }
}
