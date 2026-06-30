# Plan: Rust → C++/Qt/CMake port

Spec: `docs/superpowers/specs/2026-06-30-rust-to-cpp-qt-port-design.md`
Branch: `port/cpp-qt`  ·  Rust baseline on `main`: 47 tests passing

Faithful 1:1 port — **no new functions**. Each slice compiles and its tests pass
before moving on: `cmake -S . -B build && cmake --build build && ctest --test-dir build`.

---

## Slice 1 — Pure logic core + tests ✅ DONE (commit 6b41643)

Qt-free `denso_core` (std only) + Catch2 tests.
- `src/strutil.h`, `src/network/{model.h,netsh,parse,wifi}`,
  `src/hardware/format`, `src/settings/settings`, `src/camera/model.h`
- `tests/test_{netsh,parse,wifi,format,settings}.cpp`, `CMakeLists.txt`
- Verified with g++ 15.2 `-Wall -Wextra`: clean compile + smoke assertions pass.

---

## Slice 2 — SQLite layer (DB + migrations + repos)

Add `Qt6::Sql`. Wrap `QSqlDatabase` in a small `Db` type (unique connection
names so in-memory test DBs don't collide).

- `src/db/db.{h,cpp}` — `default_path()` (`denso.db` next to exe), `open()` (WAL),
  `open_in_memory()` (tests), `run_migrations()` with the v1→v2→v3 chain
  (v3 drops `readings` + its index). Mirror `migrations.rs` exactly.
- `src/settings/repo.{h,cpp}` — `get/set`, `load` (defaults for missing keys),
  `save`, `import_legacy` (parse a legacy `settings.json`, then delete it).
  Legacy JSON parsing via `Qt6::Core` `QJsonDocument`.
- `src/network/repo.{h,cpp}` — `save` (upsert by `iface`), `load(iface)`,
  `all()` ordered by iface.
- Tests: port `db::migrations`, `settings::repo`, and `network::repo` tests to
  Catch2 against an in-memory DB.

Done when: migrations create settings/net_config, drop readings; settings/net
round-trip; all repo tests pass.

## Slice 3 — OS network backends + hardware collect

- `src/network/backend.{h,cpp}` — `NetworkBackend` abstract base; `NullBackend`;
  `backend()` factory (`#ifdef` Windows/Linux); `reassert(db, backend)` returning
  `vector<pair<iface,error>>` (non-fatal). Port the `reassert` tests with a fake
  backend.
- `src/network/windows_backend.cpp` — `QProcess` runners (`run`, `run_checked`)
  + `snapshot` (ipconfig + netsh wlan → `parse::build_snapshot`), `apply_config`
  (`netsh::build_netsh_commands` → run), `scan_wifi`, `connect_wifi` (write WLAN
  profile XML to temp, `netsh wlan add profile`/`connect`).
- `src/network/linux_backend.cpp` — `nmcli` snapshot; apply/scan/connect return
  "not yet implemented" errors, exactly as the Rust stub does.
- `src/hardware/collect.{h,cpp}` — `HardwareSpec` + `collect()`: OS name/version,
  hostname, RAM, total storage, formatted via the existing `format_bytes`. Use
  Qt (`QSysInfo`, `QStorageInfo`) + platform calls for RAM. Same output shape.

Done when: backends compile per-platform; reassert tests pass; manual snapshot
smoke on Windows matches the Rust app.

## Slice 4 — Qt Widgets UI + wiring + main

Reproduce the Slint screens (same fields, same callbacks — nothing new).
- `src/ui/theme.{h,cpp}` — palette + dark/light, from `theme.slint`.
- `src/ui/mainwindow.{h,cpp}` — root window: display/preview area + open-settings.
- `src/ui/settings_dialog.{h,cpp}` — modal with nav + 5 panels reproducing
  `settings/*.slint`: **about** (app version), **appearance** (theme toggle),
  **display** (resolution preset apply + fullscreen), **network** (eth/wifi
  status refresh, config editors + apply, wifi scan/connect), **system**
  (hardware spec rows). Reuse small widget styles for gold-button / nav-item /
  spec-row.
- `src/ui/convert.{h,cpp}` — `NetConfig`⇄UI strings, `InterfaceStatus`→view,
  wifi rows (float connected SSID to top) — port `wiring/convert.rs`.
- Threading: scan/connect/refresh on a `QThread`/worker, results posted via
  queued signals (replaces `std::thread` + `upgrade_in_event_loop`).
- `src/main.cpp` — orchestrator: build window → open DB → migrate →
  import_legacy → reassert (log failures) → load settings → apply startup →
  install handlers → exec.
- Port `wiring/convert.rs` tests to Catch2.

Done when: app builds and launches on Windows; screens and behaviors match.

## Slice 5 — Cutover & cleanup

- Remove the Rust tree (`Cargo.toml`, `Cargo.lock`, `build.rs`, `src/**/*.rs`,
  `ui/*.slint`) once parity is confirmed.
- Update `CLAUDE.md` + `docs/ARCHITECTURE.md` to describe the C++/Qt/CMake app.
- Update `.gitignore` (drop `target/`, add `build/`).
- Decide branch integration (merge to `main` / PR).

Done when: clean `cmake` build + `ctest` green from a fresh checkout; docs match.
