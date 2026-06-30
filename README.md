# Denso Digital Reader

A desktop application for reading a 4-digit 7-segment display, with a settings
UI for display resolution, theme, host hardware spec, and network configuration.
Built with **C++ / Qt Widgets / CMake**, backed by a single SQLite store
(`denso.db`) kept next to the executable.

> Ported 1:1 from a Rust + Slint original. The port lives on branch
> `port/cpp-qt`; see `docs/ARCHITECTURE.md` for the design and the port notes.

## Features

- **Display** — pick a window resolution from presets; toggle fullscreen.
- **Appearance** — dark / light theme, applied live and persisted.
- **System** — read-only host hardware spec (OS, device, RAM, storage).
- **Network** — live Ethernet/Wi-Fi status, editable DHCP/static IP config
  (reasserted to the OS at boot), and Wi-Fi scan / connect. Windows uses
  `netsh`/`ipconfig`; Linux uses `nmcli`.
- **Camera** — placeholder modal (preview to come).

## Requirements

- A C++20 compiler (MSVC, GCC, or Clang)
- CMake ≥ 3.21
- Qt 6 (components: `Core`, `Gui`, `Sql`, `Widgets`)
- Network access on first configure (Catch2 is fetched for the tests)

## Build

```sh
cmake -S . -B build
cmake --build build
```

## Run

```sh
./build/src/app/denso     # exact path varies by generator
```

The app creates / migrates `denso.db` next to the executable on first run.

## Test

Unit tests are Catch2 v3:

```sh
ctest --test-dir build
```

The pure logic (parsers, formatters, the domain↔view converter, persistence) is
covered off-device. Platform network backend tests are compiled per-OS, so the
passing count differs between Windows and Linux.

## Project layout

Two CMake targets, split by concern and tied together by a thin top-level
`CMakeLists.txt`:

```
src/
├─ core/   → denso_core  (library; Qt Core/Sql + std)
│  ├─ db/        SQLite base + version-gated migrations
│  ├─ hardware/  host spec (QSysInfo / QStorageInfo)
│  ├─ network/   domain + persistence + OS backends
│  │  ├─ windows/  netsh / parse / wifi + Windows backend
│  │  └─ linux/    nmcli + Linux backend
│  ├─ settings/  persisted app settings
│  ├─ ui/        Qt-free domain↔view boundary (convert + view models)
│  ├─ camera/    camera domain struct (placeholder)
│  └─ util/      shared string helpers
└─ app/    → denso  (Qt Widgets GUI + entry-point orchestrator)
   └─ ui/   theme, main window, settings/camera dialogs, network card

tests/     → denso_tests  (Catch2 over denso_core)
```

`denso_core` never links `Qt6::Widgets`, so the GUI cannot leak into the
testable core. See `CLAUDE.md` for the source map and `docs/ARCHITECTURE.md` for
the boot sequence, threading model, persistence model, and known gotchas.
