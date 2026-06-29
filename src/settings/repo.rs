//! Settings persistence over the `settings` key/value table. User-editable
//! (read + write the whole struct). All paths fail silently to a sensible
//! default — a DB hiccup must never crash or block the UI. Also owns the
//! resolution presets and the one-time `settings.json` import.

use super::Settings;
use rusqlite::Connection;
use std::path::Path;

/// Selectable window resolutions, in display order. Index 2 (1600×900) is
/// the default.
pub const PRESETS: [(u32, u32); 4] = [(800, 600), (1280, 720), (1600, 900), (1920, 1080)];

const DEFAULT_INDEX: i32 = 2;

/// Read a single setting's raw value, or `None` if absent or unreadable.
fn get(conn: &Connection, key: &str) -> Option<String> {
    conn.query_row("SELECT value FROM settings WHERE key = ?1", [key], |r| r.get(0))
        .ok()
}

/// Upsert a single setting. Errors are silently ignored.
fn set(conn: &Connection, key: &str, value: &str) {
    let _ = conn.execute(
        "INSERT INTO settings (key, value) VALUES (?1, ?2) \
         ON CONFLICT(key) DO UPDATE SET value = excluded.value",
        rusqlite::params![key, value],
    );
}

/// Load settings from the DB, falling back to defaults for any missing or
/// unreadable key.
pub fn load(conn: &Connection) -> Settings {
    let d = Settings::default();
    Settings {
        width: get(conn, "width").and_then(|v| v.parse().ok()).unwrap_or(d.width),
        height: get(conn, "height").and_then(|v| v.parse().ok()).unwrap_or(d.height),
        dark: get(conn, "dark").map(|v| v == "1").unwrap_or(d.dark),
        fullscreen: get(conn, "fullscreen").map(|v| v == "1").unwrap_or(d.fullscreen),
    }
}

/// Persist all settings fields to the DB. Write errors are silently ignored.
pub fn save(conn: &Connection, settings: &Settings) {
    set(conn, "width", &settings.width.to_string());
    set(conn, "height", &settings.height.to_string());
    set(conn, "dark", if settings.dark { "1" } else { "0" });
    set(conn, "fullscreen", if settings.fullscreen { "1" } else { "0" });
}

/// One-time migration of a pre-SQLite `settings.json` into the DB. If the
/// file exists and parses, its values are persisted and the file is deleted
/// so this never runs again. A missing or corrupt file is left untouched.
pub fn import_legacy(conn: &Connection, json_path: &Path) {
    if !json_path.exists() {
        return;
    }
    let parsed = std::fs::read_to_string(json_path)
        .ok()
        .and_then(|s| serde_json::from_str::<Settings>(&s).ok());
    if let Some(s) = parsed {
        save(conn, &s);
        let _ = std::fs::remove_file(json_path);
    }
}

/// Index into [`PRESETS`] matching the given size, or the default index
/// when no preset matches.
pub fn preset_index(width: u32, height: u32) -> i32 {
    PRESETS
        .iter()
        .position(|&(w, h)| w == width && h == height)
        .map(|i| i as i32)
        .unwrap_or(DEFAULT_INDEX)
}

#[cfg(test)]
mod tests {
    use super::*;

    /// A migrated, empty in-memory database.
    fn db() -> Connection {
        let c = Connection::open_in_memory().unwrap();
        crate::db::run_migrations(&c).unwrap();
        c
    }

    #[test]
    fn preset_index_matches_known_size() {
        assert_eq!(preset_index(800, 600), 0);
        assert_eq!(preset_index(1920, 1080), 3);
    }

    #[test]
    fn preset_index_falls_back_for_unknown_size() {
        assert_eq!(preset_index(1234, 567), DEFAULT_INDEX);
    }

    #[test]
    fn load_returns_defaults_on_empty_db() {
        let s = load(&db());
        assert_eq!((s.width, s.height), (1600, 900));
        assert!(s.dark);
        assert!(!s.fullscreen);
    }

    #[test]
    fn load_uses_defaults_for_missing_keys() {
        // Only width/height persisted; theme/fullscreen must fall back.
        let c = db();
        set(&c, "width", "800");
        set(&c, "height", "600");
        let s = load(&c);
        assert_eq!((s.width, s.height), (800, 600));
        assert!(s.dark); // default_dark
        assert!(!s.fullscreen);
    }

    #[test]
    fn save_then_load_roundtrips_all_fields() {
        let c = db();
        save(
            &c,
            &Settings { width: 1280, height: 720, dark: false, fullscreen: true },
        );
        let back = load(&c);
        assert_eq!((back.width, back.height), (1280, 720));
        assert!(!back.dark);
        assert!(back.fullscreen);
    }

    #[test]
    fn import_writes_settings_and_deletes_file() {
        let c = db();
        let path = std::env::temp_dir().join("denso_import_ok.json");
        std::fs::write(
            &path,
            r#"{"width":1280,"height":720,"dark":false,"fullscreen":true}"#,
        )
        .unwrap();

        import_legacy(&c, &path);

        let s = load(&c);
        assert_eq!((s.width, s.height), (1280, 720));
        assert!(!s.dark);
        assert!(s.fullscreen);
        assert!(!path.exists(), "legacy file should be deleted after import");
    }

    #[test]
    fn import_is_noop_when_file_absent() {
        let c = db();
        let path = std::env::temp_dir().join("denso_import_absent.json");
        let _ = std::fs::remove_file(&path);

        import_legacy(&c, &path); // must not panic

        let s = load(&c);
        assert_eq!((s.width, s.height), (1600, 900)); // untouched defaults
    }

    #[test]
    fn import_leaves_corrupt_file_intact() {
        let c = db();
        let path = std::env::temp_dir().join("denso_import_corrupt.json");
        std::fs::write(&path, "}{ not json").unwrap();

        import_legacy(&c, &path);

        assert!(path.exists(), "corrupt file kept for inspection");
        let s = load(&c);
        assert_eq!((s.width, s.height), (1600, 900)); // defaults, nothing imported
        let _ = std::fs::remove_file(&path);
    }

    #[test]
    fn save_overwrites_previous_values() {
        let c = db();
        save(&c, &Settings { width: 800, height: 600, dark: true, fullscreen: false });
        save(&c, &Settings { width: 1920, height: 1080, dark: false, fullscreen: true });
        let back = load(&c);
        assert_eq!((back.width, back.height), (1920, 1080));
        assert!(!back.dark);
        assert!(back.fullscreen);
    }
}
