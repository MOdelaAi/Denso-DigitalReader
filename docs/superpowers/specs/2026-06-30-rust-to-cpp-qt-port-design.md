# Spec: Port Denso-DigitalReader from Rust+Slint to C++/Qt/CMake

Date: 2026-06-30
Status: Approved — implementation in progress (slice 1 landed)

## Context

Denso-DigitalReader is a desktop app (currently Rust + Slint) that reads a
4-digit 7-segment display, with a settings UI for resolution, theme, hardware
spec, and network configuration, persisted in a single SQLite store
(`denso.db`). The team is moving the project to **C/C++** to align with the
C/C++ ecosystem for camera capture and vision/detection work planned later.

This spec covers a **faithful 1:1 port** of the *existing* application — same
modules, same behavior, same tests. It deliberately adds **no new functionality**.

### Why a full rewrite (not FFI)

A Rust shell calling a C++ vision core via FFI was considered and is technically
the lighter option, but the decision is a full language switch for the whole app
(team/maintenance direction). This spec follows that decision.

## Decisions

- **Language:** C++20 (not C). Uses `std::string` / `std::optional` /
  `std::vector` / RAII.
- **Build:** CMake (≥ 3.21).
- **UI:** **Qt Widgets** (Qt6). Chosen over the Slint C++ binding and Dear ImGui
  for the strongest, best-documented C++ desktop fit. The Slint `.slint` files
  are not reused; the screens are reproduced in Qt.
- **Database:** SQLite via **`Qt6::Sql`** (the `QSQLITE` driver) — SQLite ships
  with Qt, so no extra dependency. Same single `denso.db`, same WAL mode, same
  `user_version`-gated migration chain.
- **No OpenCV / capture / detection.** The current app has none (the `camera`
  module is structs only and unwired; `processor` is empty). A faithful port
  therefore needs **Qt only**. The camera→processor / `Frame` / `regions()` /
  `Detector` design discussed earlier is explicitly **out of scope**.
- **Core is Qt-free.** The parsing/formatting/builder logic uses only the
  standard library (namespaces `denso::network`, `denso::hardware`,
  `denso::settings`, `denso::camera`). Qt types appear only in the DB layer, OS
  backends, and UI. Conversion between `std::string` and `QString` happens at
  the Qt boundary (mirroring the Rust `wiring/convert.rs` role).

## Architecture (maps 1:1 to the Rust tree)

| Rust | C++ / Qt |
|---|---|
| `main.rs` (orchestrator) | `src/main.cpp` |
| `db/{mod,migrations}.rs` (rusqlite, WAL, version-gated) | `src/db/` over `QSqlDatabase` |
| `settings/{model,repo}.rs` | `src/settings/` (struct + presets done; repo pending) |
| `hardware/{model,repo}.rs` (`sysinfo`) | `src/hardware/` (format done; `collect()` via Qt/OS pending) |
| `network/model.rs` | `src/network/model.h` |
| `network/windows/{netsh,parse,wifi}.rs` (pure) | `src/network/{netsh,parse,wifi}` |
| `network/{mod,repo,windows/mod,linux}.rs` | `NetworkBackend` iface + backends + repo (pending) |
| `wiring/{mod,convert}.rs` | folded into the Qt UI + a small convert layer |
| `camera/model.rs` (structs, unwired) | `src/camera/model.h` |
| `processor/` (empty) | stays empty |
| `ui/*.slint` | `src/ui/` Qt Widgets |

### Type mapping

- `String` → `std::string` (core) / `QString` (UI edge)
- `Option<T>` → `std::optional<T>`
- `Vec<T>` → `std::vector<T>`
- `Result<T, String>` → return value + thrown `std::runtime_error` for the error
  path (e.g. `build_netsh_commands` on an invalid static config); backends catch
  and surface the message, as the Rust caller mapped `Err` to a status string.
- trait `NetworkBackend` → abstract base class with pure virtuals; `backend()`
  factory returns the platform impl or a `NullBackend`.
- Off-thread work (`std::thread` + `upgrade_in_event_loop`) → `QThread` / Qt
  queued signals back to the UI thread.

## Persistence model (unchanged)

One `denso.db`, WAL. Migration chain by `PRAGMA user_version`: v1 creates
`settings` + `readings` (+ index), v2 creates `net_config`, v3 drops `readings`
(the digit-reader log was removed). Repos expose only the operations each data
policy allows.

## Testing

Rust `#[cfg(test)]` unit tests port to **Catch2 v3** (CMake `FetchContent`),
one executable, `catch_discover_tests`. The DB/repo tests use an in-memory
SQLite connection, mirroring the Rust pattern. Target: behavioral parity with
the Rust baseline (47 tests on `main`).

## Out of scope

- Any new feature (camera capture, detection, processor logic, ROI pipeline).
- Reusing the Slint UI files.
- OpenCV or any vision dependency.
- Deleting the Rust tree mid-port (removed in the final slice once parity holds).

## Verification

Per slice: `cmake -S . -B build && cmake --build build && ctest --test-dir build`.
Final: the Qt app launches, shows the same screens, and the settings/network
behaviors match the Rust app on a real Windows host.
