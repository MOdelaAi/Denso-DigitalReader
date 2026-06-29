//! SQLite persistence base. One file, `denso.db`, next to the executable —
//! the single durable store for settings (user-editable) and readings
//! (system-written, append-only).
//!
//! Access control is by API surface, not SQL grants: each feature module
//! exposes only the operations its data policy allows.

use rusqlite::Connection;
use std::path::{Path, PathBuf};

/// Current schema version. Bump when adding a migration step below.
const SCHEMA_VERSION: i64 = 2;

/// Location of the database file: `denso.db` next to the executable,
/// falling back to the current directory if the exe path is unavailable.
pub fn default_path() -> PathBuf {
    std::env::current_exe()
        .ok()
        .and_then(|p| p.parent().map(|d| d.join("denso.db")))
        .unwrap_or_else(|| PathBuf::from("denso.db"))
}

/// Open (creating if absent) the database at `path` in WAL mode, so the UI
/// can read while a background thread (network refresh, inference) writes.
/// Does not run migrations — call [`run_migrations`] after.
pub fn open(path: &Path) -> rusqlite::Result<Connection> {
    let conn = Connection::open(path)?;
    conn.pragma_update(None, "journal_mode", "WAL")?;
    Ok(conn)
}

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
    fn migrations_create_settings_and_readings_tables() {
        let c = mem();
        run_migrations(&c).unwrap();
        let count: i64 = c
            .query_row(
                "SELECT count(*) FROM sqlite_master \
                 WHERE type='table' AND name IN ('settings','readings')",
                [],
                |r| r.get(0),
            )
            .unwrap();
        assert_eq!(count, 2);
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
    fn open_enables_wal_mode() {
        let path = std::env::temp_dir().join("denso_open_enables_wal.db");
        let _ = std::fs::remove_file(&path);
        let c = open(&path).unwrap();
        let mode: String = c.query_row("PRAGMA journal_mode", [], |r| r.get(0)).unwrap();
        assert_eq!(mode.to_lowercase(), "wal");
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
