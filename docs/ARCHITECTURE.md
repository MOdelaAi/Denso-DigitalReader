# Architecture

Depth reference for Denso-DigitalReader (C++ / Qt Widgets / CMake). For the
quick map and commands, see the root `CLAUDE.md`.

## Project layout

Two targets, split by concern, wired by a thin top-level `CMakeLists.txt` via
`add_subdirectory`:

| Path | Target | Depends on |
|---|---|---|
| `src/core/` | `denso_core` (static lib) | `Qt6::Core`, `Qt6::Sql` only |
| `src/app/` | `denso` (Qt Widgets GUI exe) | `denso_core`, `Qt6::Widgets` |
| `tests/` | `denso_tests` (Catch2) | `denso_core` |

`denso_core` holds the ported logic + SQLite persistence **and** the Qt-free
domain↔view boundary (`core/ui/convert` + `viewmodel`). It never links
`Qt6::Widgets`, so the GUI cannot leak into the testable core. Each target's
directory is its own include root: core headers read `network/model.h` /
`ui/convert.h`, the app's widget headers read `ui/theme.h`, and the app reaches
core headers through `denso_core`'s public include dir.

## Boot sequence (`src/app/main.cpp`)

A thin orchestrator:

1. `QApplication` is constructed.
2. `db::Db::open(db::default_path())` opens `denso.db` (next to the exe) in WAL
   mode; `db::run_migrations` applies the `user_version`-gated chain.
3. `settings::import_legacy` does a one-time import of any pre-SQLite
   `settings.json` sitting beside the DB.
4. `network::reassert` re-applies every saved interface config to the OS — the
   app is the source of truth. Best-effort and non-fatal: failures are logged
   via `qWarning`, never block startup.
5. `settings::load` seeds an in-memory `std::shared_ptr<Settings>`.
6. `MainWindow::apply_startup` populates read-only fields + applies settings;
   the window/dialog ctors install the callbacks. `QApplication::exec()`.

`Db` (an `optional<Db>` in `main`) outlives the window, so the connection it
hands the UI stays valid for the whole run.

## UI ↔ domain boundary (`src/core/ui/`)

Feature modules never reference UI view types. `ui/convert.{h,cpp}` is the
single crossing point: `to_*` build view models (`viewmodel.h`:
`NetStatus`/`NetConfigUi`/`WifiRow`) from domain types, `from_ui_config` parses
an editable view model back to a domain `NetConfig` (blank/unparseable fields
become unset). It is `std::string`-only and unit-tested (`test_convert.cpp`);
the widgets convert to/from `QString` at their edge.

A config change travels: UI edit → `SettingsDialog::apply_net_config` →
`from_ui_config` → `network::save` (persist; app owns truth) →
`backend().apply_config` (push to OS) → status string back to the card.

## GUI (`src/app/ui/`)

A faithful port of the former Slint screens to Qt Widgets:

- `theme.{h,cpp}` — the dark/light palette + a stylesheet builder applied to
  the whole app (the Slint `Theme` global / `Palette.color-scheme` analog).
- `mainwindow.{h,cpp}` — root window: top button bar (Camera / Settings) over
  the content area. Hosts the settings-persistence handlers (resolution / theme
  / fullscreen / reset), since those resize the window and restyle the app.
- `settings_dialog.{h,cpp}` — modal: a left nav over five panels (Appearance,
  Display, System, Network, About). Owns the DB-backed network apply and the
  threaded scan/connect/refresh.
- `netcard.{h,cpp}` — one interface's live status + editable IP/DNS config +
  (Wi-Fi) scan list with per-row connect.
- `camera_dialog.{h,cpp}` — placeholder camera modal (preview "coming soon").

### Threading

`apply_net_config` runs **synchronously** on the GUI thread (as the Rust
original did). The blocking OS calls — `scan_wifi`, `connect_wifi`,
`refresh_network` — run on a worker `QThread` (`QThread::create`, so QProcess in
the backends has an event dispatcher) and post results back with
`QMetaObject::invokeMethod(this, …, Qt::QueuedConnection)`. This is the Qt
analog of the Rust `std::thread` + `upgrade_in_event_loop`. A fresh
`network::backend()` is created per operation. The settings dialog is created
once and reused, so worker callbacks always have a valid target.

## Persistence model (`src/core/db/`)

One file, `denso.db`, WAL mode so the UI reads while a worker writes. The schema
is an ordered, `user_version`-gated chain inside `db::run_migrations`
(`db/db.cpp`) — add a migration, never edit a shipped one. Each feature's `repo`
exposes only the operations its data policy allows (e.g. `hardware` is not
stored at all). The `settings` table is a typed key/value store; `net_config`
is typed columns, one row per interface.

## Network feature (`src/core/network/`)

Two distinct datasets share the Network tab:
- **Live status** — `snapshot()` reads the OS (`ipconfig`/`netsh` on Windows,
  `nmcli` on Linux) via `QProcess`. Read-only, transient.
- **Config** — `NetConfig` is user-owned, persisted, and reasserted to the OS
  at boot via `reassert`.

OS work sits behind the `NetworkBackend` base class. `backend()` returns the
platform impl (`WindowsBackend`, `LinuxBackend`, or a `NullBackend` fallback);
exactly one platform `.cpp` under `network/backends/` is compiled per OS (the
other `make_*_backend()` declaration is never odr-used). The pure helpers are
unit-tested off-device: Windows `netsh`/`parse`/`wifi`, Linux `nmcli`. Errors
mirror the Rust `Result::Err(String)` as a thrown `std::runtime_error`;
`reassert` catches them into non-fatal `(iface, message)` pairs.

## Gotchas

- **No Qt6/CMake on the porting dev host.** Only the Qt-free core subset is
  g++-checkable here; the real `cmake` build, `ctest`, and a GUI smoke must run
  on the build machine. Treat "builds" as unverified until then.
- MSVC needs `/utf-8` (set in the top-level CMake) so the UI's non-ASCII
  literals (`✕ … — 🔒`) reach the binary byte-for-byte (the sources are UTF-8
  without BOM).
- Linux disk sum over-counts loop/tmpfs/overlay mounts; sub-GB renders "0 GB"
  (embedded MB-range accepted). Verify on a real Linux device.
- `nmcli -t` SSID escaping (`\:`) and VLAN device names (`eth0:0`) are not yet
  handled — deferred to on-device validation.
- Platform backend tests are compiled per-OS, so the passing test count differs
  between Windows and Linux.
- Deferred UI parity nits from the port: the Network cards don't dim while
  loading (only the Refresh label changes); re-clicking the already-active
  Network nav item doesn't re-trigger a refresh (the Refresh button does).
- `denso.db` (+ `-wal`/`-shm`) is created next to the executable at runtime and
  is git-ignored.
