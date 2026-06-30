# Denso-DigitalReader

Desktop app (C++ / Qt Widgets / CMake) for reading a 4-digit 7-segment display,
with a settings UI for display resolution, theme, hardware spec, and network
configuration. Single SQLite store (`denso.db`) next to the executable.

Ported 1:1 from a Rust + Slint original; the port history lives on branch
`port/cpp-qt`.

## Commands

Out-of-source build with CMake (needs Qt6: Core, Gui, Sql, Widgets).

| Action | Command |
|---|---|
| Configure | `cmake -S . -B build` |
| Build | `cmake --build build` |
| Test | `ctest --test-dir build` |
| Run | `./build/src/app/denso` (path varies by generator) |

Tests are Catch2 v3, fetched via `FetchContent` at configure time (needs net on
first configure). Platform backend tests are compiled per-OS, so the passing
count differs between Windows and Linux.

## Layout

Two targets wired by a thin top-level `CMakeLists.txt` (`add_subdirectory`):

| Path | Target | Role |
|---|---|---|
| `src/core/` | `denso_core` (lib) | Ported logic + SQLite persistence + the Qt-free domain↔view boundary. `Qt6::Core`/`Sql` only. |
| `src/app/` | `denso` (GUI exe) | Qt Widgets UI + entry-point orchestrator. `Qt6::Widgets`. |
| `tests/` | `denso_tests` | Catch2 unit tests over `denso_core`. |

Each target dir is its own include root, so includes read `network/model.h`,
`ui/convert.h`, `ui/theme.h`, etc.

### `src/core/` (library)

| Path | Responsibility |
|---|---|
| `db/db.{h,cpp}` | SQLite base (`denso.db`, WAL) + the `user_version`-gated migration chain in `run_migrations`. |
| `settings/` | Persisted app settings (window size, theme, fullscreen) + resolution presets. `settings`=type/presets, `repo`=persistence + legacy import. |
| `hardware/` | Read-only host spec via `QSysInfo`/`QStorageInfo` (collected fresh, never stored). `format`=byte formatting, `collect`=the spec. |
| `network/` | `NetworkBackend` base + `reassert` + `NetConfig`/status types + config persistence (`repo`). |
| `network/windows/{netsh,wifi,parse}.*`, `network/linux/nmcli.*` | Pure, unit-tested OS-command helpers (compiled on every OS for off-device testing). |
| `network/windows/windows_backend.cpp`, `network/linux/linux_backend.cpp` | OS backends (`QProcess`); one compiled per platform. |
| `ui/convert.{h,cpp}`, `ui/viewmodel.h` | The **only** domain↔view boundary (Qt-free, testable). |
| `camera/model.h`, `strutil.h` | Camera domain struct (placeholder, not wired); small string helpers. |

### `src/app/` (GUI)

| Path | Responsibility |
|---|---|
| `main.cpp` | Thin orchestrator: open DB → migrate → import legacy → reassert network → load settings → apply startup → run. |
| `ui/theme.{h,cpp}` | Palette + theme-driven app stylesheet. |
| `ui/mainwindow.{h,cpp}` | Root window (top bar + content) + settings-persistence handlers. |
| `ui/settings_dialog.{h,cpp}` | Settings modal: nav + 5 panels; owns DB-backed network apply + threaded scan/connect/refresh. |
| `ui/netcard.{h,cpp}` | Per-interface status + editable config + Wi-Fi scan/connect. |
| `ui/camera_dialog.{h,cpp}` | Placeholder camera modal. |

## Hard rules

- Domain/feature types **never** see UI view types. The only boundary is
  `src/core/ui/convert.{h,cpp}`.
- `main.cpp` stays a thin orchestrator — no business logic.
- Each feature is split header/source by responsibility (type / persistence /
  OS access). Access policy is the `repo`'s API surface, not SQL grants.
- Persistence is one SQLite file with version-gated migrations in
  `db::run_migrations` — add a migration, never edit a shipped one.
- OS-specific work sits behind `NetworkBackend` (`network/backends/`). Keep both
  platforms in sync.
- `denso_core` must not link `Qt6::Widgets` — the GUI cannot leak into the
  testable core.
- `build/` and `*.png` are git-ignored (see `.gitignore`); `assets/icon.png`
  predates the `*.png` rule and stays tracked as a committed source asset.

## Workflow

Superpowers SDD: specs in `docs/superpowers/specs/`, plans in
`docs/superpowers/plans/`, progress ledger in `.superpowers/sdd/progress.md`.

See `docs/ARCHITECTURE.md` for the boot sequence, data flow, threading model,
persistence model, and gotchas (including: no Qt6/CMake on the porting dev
host — real build/ctest/GUI smoke happen on the build machine).
