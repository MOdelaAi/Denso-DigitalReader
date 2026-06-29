slint::include_modules!();

mod settings;
mod hardware;
mod network;

use settings::Settings;
use std::cell::RefCell;
use std::rc::Rc;

fn to_net_status(s: &network::InterfaceStatus) -> NetStatus {
    NetStatus {
        connected: s.connected,
        ip: s.ip.clone().into(),
        gateway: s.gateway.clone().into(),
        ssid: s.ssid.clone().into(),
        signal: s.signal.clone().into(),
    }
}

fn main() -> Result<(), slint::PlatformError> {
    let app = AppWindow::new()?;
    let state = Rc::new(RefCell::new(settings::load()));

    app.set_app_version(env!("CARGO_PKG_VERSION").into());

    let hw = hardware::collect();
    app.set_hw_os(hw.os.into());
    app.set_hw_device(hw.device.into());
    app.set_hw_ram(hw.ram.into());
    app.set_hw_storage(hw.storage.into());

    // Apply persisted state at startup.
    {
        let s = state.borrow();
        app.window()
            .set_size(slint::LogicalSize::new(s.width as f32, s.height as f32));
        app.window().set_fullscreen(s.fullscreen);
        app.set_resolution_index(settings::preset_index(s.width, s.height));
        app.set_fullscreen(s.fullscreen);
        app.global::<Theme>().set_dark(s.dark);
    }

    // Resolution Apply — resize + persist (preserves other fields).
    let weak = app.as_weak();
    let st = state.clone();
    app.on_apply_resolution(move |index| {
        let (w, h) = settings::PRESETS[index as usize];
        {
            let mut s = st.borrow_mut();
            s.width = w;
            s.height = h;
            settings::save(&s);
        }
        if let Some(app) = weak.upgrade() {
            app.window()
                .set_size(slint::LogicalSize::new(w as f32, h as f32));
        }
    });

    // Theme toggle — instant persist.
    let st = state.clone();
    app.on_theme_changed(move |dark| {
        let mut s = st.borrow_mut();
        s.dark = dark;
        settings::save(&s);
    });

    // Fullscreen toggle — apply + persist.
    let weak = app.as_weak();
    let st = state.clone();
    app.on_toggle_fullscreen(move |fullscreen| {
        {
            let mut s = st.borrow_mut();
            s.fullscreen = fullscreen;
            settings::save(&s);
        }
        if let Some(app) = weak.upgrade() {
            app.window().set_fullscreen(fullscreen);
        }
    });

    // Reset to defaults — persist defaults and reflect them in the UI.
    let weak = app.as_weak();
    let st = state.clone();
    app.on_reset_defaults(move || {
        let d = Settings::default();
        settings::save(&d);
        if let Some(app) = weak.upgrade() {
            app.window().set_fullscreen(d.fullscreen);
            app.window()
                .set_size(slint::LogicalSize::new(d.width as f32, d.height as f32));
            app.set_resolution_index(settings::preset_index(d.width, d.height));
            app.set_fullscreen(d.fullscreen);
            app.global::<Theme>().set_dark(d.dark);
        }
        *st.borrow_mut() = d;
    });

    let weak = app.as_weak();
    app.on_refresh_network(move || {
        let Some(app) = weak.upgrade() else { return; };
        app.set_net_loading(true);
        let weak2 = app.as_weak();
        std::thread::spawn(move || {
            let snap = network::backend().snapshot();
            let _ = weak2.upgrade_in_event_loop(move |app| {
                app.set_eth(to_net_status(&snap.ethernet));
                app.set_wifi(to_net_status(&snap.wifi));
                app.set_net_loading(false);
            });
        });
    });

    app.run()
}
