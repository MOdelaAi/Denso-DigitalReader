//! Settings domain type. Mirrors the `settings` key/value table; the typed
//! struct is the in-Rust view. `Serialize`/`Deserialize` are retained only to
//! parse a legacy `settings.json` during one-time import (see [`super::repo`]).

use serde::{Deserialize, Serialize};

const DEFAULT_WIDTH: u32 = 1600;
const DEFAULT_HEIGHT: u32 = 900;

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
}
