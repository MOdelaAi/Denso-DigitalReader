slint::include_modules!();

mod db;
mod hardware;
mod network;
mod reader;
mod settings;
mod wiring;

use std::cell::RefCell;
use std::rc::Rc;

fn main() -> Result<(), slint::PlatformError> {
    let app = AppWindow::new()?;

    let db_path = db::default_path();
    let db_conn = Rc::new(db::open(&db_path).expect("open database"));
    db::run_migrations(&db_conn).expect("run migrations");

    // One-time migration of any pre-SQLite settings.json sitting beside the DB.
    if let Some(dir) = db_path.parent() {
        settings::import_legacy(&db_conn, &dir.join("settings.json"));
    }

    // The app owns network config: reassert it to the OS at boot. Non-fatal —
    // a failed apply is logged, never blocks startup.
    for (iface, err) in network::reassert(&db_conn, network::backend().as_ref()) {
        eprintln!("network: failed to apply {iface} config: {err}");
    }

    let state = Rc::new(RefCell::new(settings::load(&db_conn)));

    wiring::apply_startup(&app, &state);
    wiring::install_handlers(&app, &db_conn, &state);

    app.run()
}
