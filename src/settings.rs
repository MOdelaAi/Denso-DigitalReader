//! Persisted application settings (window size, theme, fullscreen).
//!
//! Stored as `settings.json` next to the executable. All read/write paths
//! fail silently to a sensible default — losing the file must never crash
//! or block the app.

use serde::{Deserialize, Serialize};

/// Selectable window resolutions, in display order. Index 2 (1600×900) is
/// the default.
pub const PRESETS: [(u32, u32); 4] = [(800, 600), (1280, 720), (1600, 900), (1920, 1080)];

const DEFAULT_WIDTH: u32 = 1600;
const DEFAULT_HEIGHT: u32 = 900;
const DEFAULT_INDEX: i32 = 2;

#[derive(Serialize, Deserialize, Clone)]
pub struct Settings {
    pub width: u32,
    pub height: u32,
    #[serde(default = "default_dark")]
    pub dark: bool,
    #[serde(default)]
    pub fullscreen: bool,
}

fn default_dark() -> bool {
    true
}

impl Default for Settings {
    fn default() -> Self {
        Settings {
            width: DEFAULT_WIDTH,
            height: DEFAULT_HEIGHT,
            dark: true,
            fullscreen: false,
        }
    }
}

fn path() -> Option<std::path::PathBuf> {
    std::env::current_exe()
        .ok()?
        .parent()
        .map(|p| p.join("settings.json"))
}

/// Load settings from disk, falling back to defaults on any error
/// (missing file, unreadable path, malformed JSON).
pub fn load() -> Settings {
    path()
        .and_then(|p| std::fs::read_to_string(p).ok())
        .and_then(|s| serde_json::from_str(&s).ok())
        .unwrap_or_default()
}

/// Persist settings to disk. Write errors are silently ignored.
pub fn save(settings: &Settings) {
    if let Some(p) = path() {
        if let Ok(json) = serde_json::to_string_pretty(settings) {
            let _ = std::fs::write(p, json);
        }
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

    #[test]
    fn default_is_dark_1600x900_windowed() {
        let s = Settings::default();
        assert_eq!((s.width, s.height), (1600, 900));
        assert!(s.dark);
        assert!(!s.fullscreen);
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
    fn missing_optional_fields_use_defaults() {
        // An older settings.json with only width/height must still load.
        let s: Settings = serde_json::from_str(r#"{"width":800,"height":600}"#).unwrap();
        assert_eq!((s.width, s.height), (800, 600));
        assert!(s.dark); // default_dark
        assert!(!s.fullscreen); // serde default
    }

    #[test]
    fn roundtrip_preserves_all_fields() {
        let s = Settings {
            width: 1280,
            height: 720,
            dark: false,
            fullscreen: true,
        };
        let json = serde_json::to_string(&s).unwrap();
        let back: Settings = serde_json::from_str(&json).unwrap();
        assert_eq!((back.width, back.height), (1280, 720));
        assert!(!back.dark);
        assert!(back.fullscreen);
    }
}
