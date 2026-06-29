slint::include_modules!();

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
        .unwrap_or(Settings { width: 1600, height: 900 })
}

fn save_settings(width: u32, height: u32) {
    if let Some(path) = settings_path() {
        let s = Settings { width, height };
        if let Ok(json) = serde_json::to_string_pretty(&s) {
            let _ = std::fs::write(path, json);
        }
    }
}

fn main() -> Result<(), slint::PlatformError> {
    let settings = load_settings();

    let app = AppWindow::new()?;

    app.window().set_size(slint::LogicalSize::new(
        settings.width as f32,
        settings.height as f32,
    ));

    let index = PRESETS
        .iter()
        .position(|&(w, h)| w == settings.width && h == settings.height)
        .unwrap_or(2) as i32;
    app.set_resolution_index(index);

    let app_weak = app.as_weak();
    app.on_apply_resolution(move |index| {
        let (w, h) = PRESETS[index as usize];
        save_settings(w, h);
        if let Some(app) = app_weak.upgrade() {
            app.window().set_size(slint::LogicalSize::new(w as f32, h as f32));
        }
    });

    app.run()
}
