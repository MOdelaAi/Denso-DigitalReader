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
- **Camera** — live 1–4 camera grid with per-camera ONNX digit detection and
  named ROI polygons.
- **Server** — push each ROI's reading to a backend as one combined JSON POST
  (see *Brazing zone reporting* below).

## Brazing zone reporting

The app can report the number shown in each camera ROI to a backend server. A
**zone** is an ROI polygon (drawn in the Camera wizard's *Areas* step) that has a
**zone number** (1–12) assigned to it. The digit detections inside a zone are read
**left-to-right** and assembled into an integer (e.g. boxes `5`,`0`,`0` → `500`).

When any zone's reading **settles on a new value** (debounced), the app POSTs the
current value of *every* known zone across all cameras as one combined body:

```
POST {base_url}/api/brazing/update
Content-Type: application/json

{ "zone1": 500, "zone2": 200 }
```

Delivery is **best-effort, latest-value-wins**: there is no queue or retry — if a
POST is lost, the next change re-sends the full snapshot. A stuck/unreachable
server can't hang the app (bounded 5 s timeout; failures are logged and dropped).

**Enable it:** Settings → **Server** → tick *Send zone readings to server* and set
the base URL (e.g. `http://192.168.1.50:8098`). Then, in the Camera wizard →
*Areas*, give each reporting ROI a zone number (0 = ROI-only, not reported).
Config changes take effect when the camera grid next reloads.

**Test it locally** against the stand-in server in `test-server/`:

```sh
cd test-server && python server.py --host 0.0.0.0 --port 8098
# point the app's base URL at http://<this-pc-ip>:8098, then watch the posts:
curl http://localhost:8098/api/state
```

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
