# Architecture

Depth reference for Denso-DigitalReader. For the quick map and commands, see
the root `CLAUDE.md`.

## Boot sequence (`main.rs`)

1. `AppWindow::new()` builds the Slint window.
2. `db::open(default_path())` opens `denso.db` (next to the exe) in WAL mode;
   `db::run_migrations` applies the `user_version`-gated chain.
3. `settings::import_legacy` does a one-time import of any pre-SQLite
   `settings.json` sitting beside the DB.
4. `network::reassert` re-applies every saved interface config to the OS — the
   app is the source of truth. Best-effort and non-fatal: failures are logged,
   never block startup.
5. `settings::load` seeds the in-memory `Rc<RefCell<Settings>>`.
6. `wiring::apply_startup` populates read-only fields + applies settings;
   `wiring::install_handlers` wires callbacks. `app.run()`.

## UI ↔ domain boundary (`wiring/`)

Feature modules never reference Slint types. `wiring/convert.rs` is the single
crossing point: `to_*` build view models from domain types, `from_ui_config`
parses an editable view model back to a domain `NetConfig` (blank/unparseable
fields become `None`). `wiring/mod.rs` installs callbacks that call the
converters and persist through each feature's `repo`.

A config change travels: UI edit → callback in `wiring/mod.rs` →
`from_ui_config` → `network::repo::save` (persist; app owns truth) →
`backend().apply_config` (push to OS) → status string back to the UI.

## Persistence model (`db/`)

One file, `denso.db`, WAL mode so the UI reads while a background thread writes.
Schema lives entirely in `db/migrations.rs` as a single ordered, `user_version`
-gated chain — add a migration, never edit a shipped one. Each feature's `repo`
exposes only the operations its data policy allows (e.g. `hardware` is not
stored at all). The `settings` table is a typed key/value store; other tables
are typed columns.

## Network feature (`network/`)

Two distinct datasets share the Network tab:
- **Live status** — `snapshot()` reads the OS (`ipconfig`/`netsh` on Windows,
  `nmcli` on Linux). Read-only, transient.
- **Config** — `NetConfig` is user-owned, persisted, and reasserted to the OS
  at boot via `reassert`.

OS work sits behind the `NetworkBackend` trait. `backend()` returns the
platform impl (`WindowsBackend`, `LinuxBackend`, or a `NullBackend` fallback).
The Windows backend is split into `netsh` (build + run config commands),
`wifi` (parse scans, build WLAN profile XML), and `parse` (status parsing);
the pure helpers are unit-tested off-device.

## In progress: camera + processor

The working tree is mid-refactor, adding camera management and a processing
stage. This is the intended direction, not shipped structure:

- `camera/` is new: `model.rs` defines `Camera` / `CameraArea` /
  `CameraWithAreas` (USB + IP cameras with per-camera ROI areas; the IP camera
  password lives in the OS secret store, never the DB). Its `mod`/`repo`,
  migration, and UI wiring are not written yet.
- `processor/` is a new, currently empty placeholder for the
  capture→processing stage.

Neither module is declared in `main.rs`, so neither compiles into the build
yet. Domain `model.rs` files now split their types into `DB (persisted)` and
`Runtime (transient)` sections (see `camera/model.rs`, `network/model.rs`).

## Gotchas

- `edition = "2024"` requires Rust ≥ 1.85.
- Linux disk sum over-counts loop/tmpfs/overlay mounts; sub-GB renders "0 GB"
  (embedded MB-range accepted). Verify on a real Linux device.
- `nmcli -t` SSID escaping (`\:`) and VLAN device names (`eth0:0`) are not yet
  handled — deferred to on-device validation.
- Platform backend tests are `#[cfg(...)]`-gated, so the passing test count
  differs between Windows and Linux.
- `target/` and `*.png` are git-ignored. `assets/icon.png` predates the `*.png`
  rule and stays tracked: it is a committed source asset that `build.rs` converts
  to `icon.ico` under `target/` at build time (the generated `.ico` is the build
  output, not committed).
