//! Schema definition and migrations. The single ordered, `user_version`-gated
//! view of how the database has evolved — read top to bottom to see the
//! current shape. Each feature's Rust types mirror the tables defined here.

use rusqlite::Connection;

/// Current schema version. Bump and add a `version < N` block when changing
/// the schema.
const SCHEMA_VERSION: i64 = 3;

/// Apply any pending schema migrations, gated by `PRAGMA user_version` so
/// repeated runs are no-ops. Safe to call on every startup.
pub fn run_migrations(conn: &Connection) -> rusqlite::Result<()> {
    let version: i64 = conn.query_row("PRAGMA user_version", [], |r| r.get(0))?;

    if version < 1 {
        conn.execute_batch(
            "CREATE TABLE IF NOT EXISTS settings (
                 key   TEXT PRIMARY KEY,
                 value TEXT NOT NULL
             );
             CREATE TABLE IF NOT EXISTS readings (
                 id          INTEGER PRIMARY KEY,
                 ts          INTEGER NOT NULL,
                 value       TEXT    NOT NULL,
                 confidence  REAL,
                 image_path  TEXT
             );
             CREATE INDEX IF NOT EXISTS idx_readings_ts ON readings(ts);",
        )?;
    }

    if version < 2 {
        // User-editable network configuration; the app is the source of
        // truth and reasserts these to the OS on boot. The WiFi PSK is NOT
        // stored here — it lives in the OS secret store.
        conn.execute_batch(
            "CREATE TABLE IF NOT EXISTS net_config (
                 iface    TEXT PRIMARY KEY,
                 mode     TEXT NOT NULL,
                 ip       TEXT,
                 prefix   INTEGER,
                 gateway  TEXT,
                 dns1     TEXT,
                 dns2     TEXT,
                 ssid     TEXT,
                 security TEXT
             );",
        )?;
    }

    if version < 3 {
        // The digit-reader log feature was removed; drop its now-unused table.
        conn.execute_batch(
            "DROP INDEX IF EXISTS idx_readings_ts;
             DROP TABLE IF EXISTS readings;",
        )?;
    }

    // PRAGMA can't be parameterized; SCHEMA_VERSION is a trusted constant.
    conn.execute_batch(&format!("PRAGMA user_version = {SCHEMA_VERSION};"))?;
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    fn mem() -> Connection {
        Connection::open_in_memory().unwrap()
    }

    #[test]
    fn migrations_create_settings_table() {
        let c = mem();
        run_migrations(&c).unwrap();
        let count: i64 = c
            .query_row(
                "SELECT count(*) FROM sqlite_master \
                 WHERE type='table' AND name='settings'",
                [],
                |r| r.get(0),
            )
            .unwrap();
        assert_eq!(count, 1);
    }

    #[test]
    fn migrations_drop_readings_table() {
        // `readings` is created at v1 and dropped at v3 — a fully migrated DB
        // must not have it (the digit-reader log feature was removed).
        let c = mem();
        run_migrations(&c).unwrap();
        let count: i64 = c
            .query_row(
                "SELECT count(*) FROM sqlite_master \
                 WHERE type='table' AND name='readings'",
                [],
                |r| r.get(0),
            )
            .unwrap();
        assert_eq!(count, 0);
    }

    #[test]
    fn migrations_create_net_config_table() {
        let c = mem();
        run_migrations(&c).unwrap();
        let count: i64 = c
            .query_row(
                "SELECT count(*) FROM sqlite_master \
                 WHERE type='table' AND name='net_config'",
                [],
                |r| r.get(0),
            )
            .unwrap();
        assert_eq!(count, 1);
    }

    #[test]
    fn migrations_set_user_version() {
        let c = mem();
        run_migrations(&c).unwrap();
        let v: i64 = c.query_row("PRAGMA user_version", [], |r| r.get(0)).unwrap();
        assert_eq!(v, SCHEMA_VERSION);
    }

    #[test]
    fn migrations_are_idempotent() {
        let c = mem();
        run_migrations(&c).unwrap();
        run_migrations(&c).unwrap();
        let v: i64 = c.query_row("PRAGMA user_version", [], |r| r.get(0)).unwrap();
        assert_eq!(v, SCHEMA_VERSION);
    }
}
