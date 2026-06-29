//! Persisted application settings (window size, theme, fullscreen). User-
//! editable: the typed [`Settings`] is read/written whole over the `settings`
//! key/value table. See [`repo`] for persistence and the legacy import.

mod model;
mod repo;

pub use model::Settings;
pub use repo::{import_legacy, load, preset_index, save, PRESETS};
