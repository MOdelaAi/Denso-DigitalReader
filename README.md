# Denso Digital Reader

A desktop application for reading a 4-digit 7-segment display, with a settings
UI for display resolution, theme, host hardware spec, and network configuration.
Built with **C++ / Qt Widgets / CMake**, backed by a single SQLite store
(`denso.db`) kept in the data dir — beside the executable by default, or
wherever `$DENSO_DATA_DIR` points.

> Ported 1:1 from a Rust + Slint original. The port has landed — `main` **is**
> the C++/Qt app (the `port/cpp-qt` branch is gone). See `docs/ARCHITECTURE.md`
> for the design and the port notes.

## Features

- **Display** — pick a window resolution from presets; toggle fullscreen.
- **Appearance** — dark / light theme, applied live and persisted.
- **System** — read-only host hardware spec (OS, device, RAM, storage).
- **Network** — live Ethernet/Wi-Fi status, editable DHCP/static IP config
  (reasserted to the OS at boot), and Wi-Fi scan / connect. Windows uses
  `netsh`/`ipconfig`; Linux uses `nmcli`.
- **Camera** — live 1–4 camera grid with per-camera digit detection (ONNX on
  Windows, TensorRT on the Jetson — see *Requirements*) and
  named ROI polygons. Each camera's source is editable; changing it re-verifies
  the ROIs (reporting pauses until they're re-checked against the new view).
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

Delivery is **reliable, latest-value-wins**: if the server is unreachable, the app
keeps retrying the latest zone values (exponential backoff, up to 30 s between
tries) and delivers them once the server returns — the newest reading always wins,
and new readings merge into the pending snapshot while it waits. Only one POST is
in flight at a time; retry state is in-memory (nothing is queued or persisted, so
a restart starts fresh from live detection). A stuck/unreachable server can't hang
the app (bounded 5 s per-request timeout).

**Enable it:** Settings → **Server** → tick *Send zone readings to server* and set
the base URL (e.g. `http://192.168.1.50:8098`). Then, in the Camera wizard →
*Areas*, give each reporting ROI a zone number (0 = ROI-only, not reported).
Config changes take effect when the camera grid next reloads.

**Test it locally** against the stand-in server, which lives **outside this repo**
at `d:\workspace\test-server`:

```sh
cd /d/workspace/test-server && python server.py --host 0.0.0.0 --port 8098
# point the app's base URL at http://<this-pc-ip>:8098, then watch the posts:
curl http://localhost:8098/api/state
```

## Requirements

Every `find_package` below is `REQUIRED` — configure fails without it.

- A C++20 compiler (MSVC, GCC, or Clang)
- CMake ≥ 3.21
- Qt 6 — components `Core`, `Gui`, `Sql`, `Widgets`, `Multimedia`, `Network`
- OpenCV
- A detection backend, **platform-split** (see `docs/ARCHITECTURE.md`):
  - **Windows** — ONNX Runtime, provisioned into `third_party/onnxruntime/`
    (git-ignored; see `docs/GPU_SETUP.md`)
  - **Linux / Jetson** — CUDA Toolkit + TensorRT (`nvinfer` + `NvInfer.h`)
- Network access on first configure (Catch2 is fetched for the tests)

## Build

```sh
cmake -S . -B build
cmake --build build
```

## Provisioning a model

**No model is tracked in git** — everything under `models/` (`*.onnx`, `*.pt`,
`*.engine`, `*.names.json`, `trt_cache/`) is a build/training artifact, device-
and version-specific. A fresh clone has an **empty `models/`**, and the app then
has an empty catalog. The current model family is **digitv3**; `denso.onnx` and
the digitv2 family were removed.

| Machine | Needs | Where it comes from |
|---|---|---|
| Windows/MSYS2 (dev) | `models/digitv3.onnx` | exported from `digitv3.pt` in the training venv at `d:\workspace\train_venv` — the ONNX is what ORT loads |
| Jetson (dev + appliance) | `models/digitv3.engine` + `models/digitv3.names.json` | built **on-device** from that ONNX with the `trtexec` recipe recorded in `packaging/models.approved` (TRT 10.3 / `sm_87`; the app never builds one at runtime) |

The `.pt` checkpoint is a training input, not a runtime asset — the Jetson has no
torch/ultralytics, so the `.pt` → `.onnx` export happens on the training machine.
The engine and its `.names.json` sidecar are a **pair**: `TrtEngine` reads class
names from the sidecar, so a mismatched one silently changes what the app reads.

Windows discovers models with a **configure-time** glob
(`file(GLOB … models/*.onnx)` in `src/app/CMakeLists.txt`), so **re-run `cmake`
after adding or replacing an ONNX** — a plain rebuild won't pick it up. Note the
glob copies present inputs but does not delete stale ones already beside the exe;
remove those by hand (or wipe `build/`). On Linux nothing is copied at build
time — the operator places the engine + sidecar in the data dir's `models/`.

`packaging/models.approved` is a **production-package** contract: it approves only
the deployable `.engine` + `.names.json` pair by SHA-256, and
`tools/build_package.sh --model` refuses anything not listed. It deliberately does
not record an ONNX hash, so the package can be proven to carry an approved engine
but the exact ONNX that produced it is not reconstructable from the manifest.

## Run

```sh
./build/src/app/denso     # exact path varies by generator
```

The app creates / migrates `denso.db` on first run, in the data dir: beside the
executable by default, or `$DENSO_DATA_DIR` when set (which is how a packaged
install keeps state off the root-owned, upgrade-replaced program dir).

Headless modes — used by the installer, no display required:

```sh
denso --version
denso --check [--engine <file>]...   # validate data dir + engines; no primary-DB mutation,
                                      # run as the target user (see docs/ARCHITECTURE.md)
denso --check-running                # exit 0 running, 1 not running, 4 cannot determine
denso --check-migrations <db-path>   # run the migration chain against that path ONLY
```

Only one instance may run at a time; a second exits 3.

## Deploy

Ships as a `.deb` for the Jetson (JetPack 6.2 / L4T R36.5.0). Build it **on an
aarch64 Jetson** — there is no cross-toolchain, and the engines are `sm_87`/TRT
10.3 pinned:

```sh
tools/build_package.sh --model models/digitv3.engine     # -> dist/
```

Then, on the target (the preflight is bound to that exact `.deb` by SHA-256 and
guards the JetPack stack — run it first):

```sh
sudo ./dist/preflight-denso-<version>.sh ./dist/denso-digitalreader_<version>_arm64.deb
sudo apt install --no-install-recommends ./dist/denso-digitalreader_<version>_arm64.deb
sudo denso-setup configure --user <username>
sudo denso-setup verify                                   # expect: verify: PASS
```

Never `dpkg -i` — it does not resolve dependencies. `apt remove` keeps
`/opt/denso/data` (database, engines); `apt purge` removes it.

**Autostart is not configured by default.** `denso-setup configure --autostart
--enable-autologin` exists and is designed, but its GDM/XDG path has **not** been
verified on hardware — see `docs/superpowers/specs/2026-07-17-build-package-deployment-design.md`.

## Test

Unit tests are Catch2 v3:

```sh
ctest --test-dir build
```

Testing on the real target (Jetson Orin Nano): connection details, credentials
and the platform/toolchain versions live in **`d:\workspace\devices.md`**, the
shared device registry outside this repo.

> Catch2 test names are passed back to the binary by `catch_discover_tests` as
> CLI arguments, so keep them **ASCII** and never start one with `--` — a name
> like `--engine only applies to --check`, or one containing `→`, arrives
> mangled and the case reports Failed while its logic is fine.

The pure logic (parsers, formatters, the domain↔view converter, persistence) is
covered off-device. Platform network backend tests are compiled per-OS, so the
passing count differs between Windows and Linux.

## Project layout

Six CMake targets, split by concern and tied together by a thin top-level
`CMakeLists.txt` — the `denso` exe, the `denso_core` library, three static
subsystem libs (`denso_detection` / `denso_brazing` / `denso_camera`) linked by
both the exe and the tests, and `denso_tests`:

```
src/
├─ core/   → denso_core  (library; Qt Core/Sql + std)
│  ├─ db/        SQLite base + version-gated migrations (currently v11)
│  ├─ hardware/  host spec (QSysInfo / QStorageInfo)
│  ├─ network/   domain + persistence + OS backends
│  │  ├─ windows/  netsh / parse / wifi + Windows backend
│  │  └─ linux/    nmcli + Linux backend
│  ├─ settings/  persisted app settings
│  ├─ ui/        Qt-free domain↔view boundary (convert + view models)
│  ├─ camera/    camera + ROI-area domain + source-change (ROI-quarantine) logic
│  ├─ detection/ per-camera model / class config + persistence
│  ├─ reading/   append-only reading log
│  ├─ brazing/   zone-reporter config
│  └─ util/      shared string helpers
└─ app/    → denso  (Qt Widgets GUI) + 3 static subsystem libs
   ├─ ui/         theme, main window, settings + camera widgets
   ├─ camera/     capture + frame-processing runtime      → denso_camera
   ├─ detection/  inference helpers + backend engines      → denso_detection
   ├─ brazing/    zone reporting logic + HTTP transport     → denso_brazing
   └─ logging/    bounded 24/7 rotating file log

tests/     → denso_tests  (Catch2 over denso_core + the subsystem libs)
```

`denso_core` never links `Qt6::Widgets`, so the GUI cannot leak into the
testable core. The three `src/app/` subsystem libs (`denso_camera`,
`denso_detection`, `denso_brazing`) are linked by **both** `denso` and
`denso_tests`, so the tests exercise the shipped objects rather than a second
compile. See `CLAUDE.md` for the source map and `docs/ARCHITECTURE.md` for the
boot sequence, threading model, persistence model, and known gotchas.
