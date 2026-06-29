slint::include_modules!();

use std::cell::RefCell;
use std::rc::Rc;

const PRESETS: [(u32, u32); 4] = [
    (800, 600),
    (1280, 720),
    (1600, 900),
    (1920, 1080),
];

#[derive(serde::Serialize, serde::Deserialize)]
struct Settings {
    width: u32,
    height: u32,
    #[serde(default = "default_dark")]
    dark: bool,
}

fn default_dark() -> bool {
    true
}

fn settings_path() -> Option<std::path::PathBuf> {
    std::env::current_exe()
        .ok()?
        .parent()
        .map(|p| p.join("settings.json"))
}

fn load_settings() -> Settings {
    settings_path()
        .and_then(|path| std::fs::read_to_string(path).ok())
        .and_then(|s| serde_json::from_str(&s).ok())
        .unwrap_or(Settings { width: 1600, height: 900, dark: true })
}

fn save_settings(settings: &Settings) {
    if let Some(path) = settings_path() {
        if let Ok(json) = serde_json::to_string_pretty(settings) {
            let _ = std::fs::write(path, json);
        }
    }
}

fn main() -> Result<(), slint::PlatformError> {
    let app = AppWindow::new()?;

    let settings = Rc::new(RefCell::new(load_settings()));

    {
        let s = settings.borrow();
        app.window().set_size(slint::LogicalSize::new(
            s.width as f32,
            s.height as f32,
        ));
        let index = PRESETS
            .iter()
            .position(|&(w, h)| w == s.width && h == s.height)
            .unwrap_or(2) as i32;
        app.set_resolution_index(index);
        app.global::<Theme>().set_dark(s.dark);
    }

    let app_weak = app.as_weak();
    let settings_res = settings.clone();
    app.on_apply_resolution(move |index| {
        let (w, h) = PRESETS[index as usize];
        {
            let mut s = settings_res.borrow_mut();
            s.width = w;
            s.height = h;
            save_settings(&s);
        }
        if let Some(app) = app_weak.upgrade() {
            app.window().set_size(slint::LogicalSize::new(w as f32, h as f32));
        }
    });

    let settings_theme = settings.clone();
    app.on_theme_changed(move |dark| {
        let mut s = settings_theme.borrow_mut();
        s.dark = dark;
        save_settings(&s);
    });

    app.run()
}
