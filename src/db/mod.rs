//! SQLite persistence base. One file, `denso.db`, next to the executable —
//! the single durable store. Connection setup lives here; the schema and its
//! version-gated migrations live in [`migrations`].
//!
//! Access control is by API surface, not SQL grants: each feature's `repo`
//! exposes only the operations its data policy allows.

mod migrations;
pub use migrations::run_migrations;

use rusqlite::Connection;
use std::path::{Path, PathBuf};

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

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn open_enables_wal_mode() {
        let path = std::env::temp_dir().join("denso_open_enables_wal.db");
        let _ = std::fs::remove_file(&path);
        let c = open(&path).unwrap();
        let mode: String = c.query_row("PRAGMA journal_mode", [], |r| r.get(0)).unwrap();
        assert_eq!(mode.to_lowercase(), "wal");
    }
}
