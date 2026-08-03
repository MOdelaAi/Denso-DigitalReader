# Denso Digital Reader

A Jetson-based desktop application for camera-based industrial reading. It pulls
live camera streams, runs TensorRT inference on them, draws the result back into
the picture, and reports the values to a backend over HTTP. The UI is Qt Widgets;
all state lives in one SQLite store (`denso.db`) in the data dir.

The appliance runs in exactly **one of two operating modes** at a time:

| Mode | Reads | Model family |
|---|---|---|
| **Digital Number Reader** (`digit_reader`) | up to four numeric digits per configured area | `digit_numeric` (`digitv3`) |
| **Floating Ball Leveler** (`ball_leveler`) | a floating ball's height, as a 0–100 % level | `float_ball` (`float-small`, `float-big`) |

The modes are **mutually exclusive**. No model is permitted in both; the mode
matrix lives in exactly one place (`src/core/models/compatibility.cpp`) and fails
closed on anything it does not recognise. Switching between them is destructive —
see [Mode switching](#mode-switching).

> Built with **C++20 / Qt 6 Widgets / CMake**. Ported 1:1 from a Rust + Slint
> original; `main` **is** the C++/Qt app. See `docs/ARCHITECTURE.md` for the boot
> sequence, threading model, persistence model and known gotchas, and
> `docs/MODEL_COMPATIBILITY.md` for the model/mode rules.

---

## Digital Number Reader

Each camera carries one or more **areas** — closed ROI polygons drawn in the
Camera wizard's *Areas* step. Inside an area the detected digits are read
left-to-right and assembled into a number of **up to four digit positions**.

An area that should be reported is given a **global zone number**; an area
without one is ROI-only (it constrains detection but is never reported).

Each area independently selects one **number format**, which is where the decimal
point goes. The four entries are the only legal formats:

| Format | Raw digits `1234` renders as |
|---|---|
| `0000` | `1234` |
| `000.0` | `123.4` |
| `00.00` | `12.34` |
| `0.000` | `1.234` |

**The model does not detect a decimal point.** It reads digits; the configured
format alone decides where the point sits. Changing an area's format changes what
is displayed *and* what is reported, without retraining anything.

On screen the reader shows a fixed four-position face, so **leading zeros are
preserved**: raw `12` with format `00.00` is drawn as `00.12`. The backend payload
carries the number only — `0.12` — never the padding.

## Floating Ball Leveler

Each camera binds **one** Float model and carries **one to four Ball zones**
(`level::kMaxBallZones`). A zone is:

- a rectangular measurement area;
- a **lower reference line** that reads **0 %**;
- an **upper reference line** that reads **100 %**.

The selected ball's vertical centre is mapped linearly between those two lines
into a 0–100 % level. A ball genuinely above the 100 % line or below the 0 % line
is clamped rather than extrapolated.

All Ball zones on one camera share **a single inference result** for that camera —
the frame is processed once and each zone selects from the same detections.

Ball zones draw their numbers from the **same machine-wide zone namespace** as
Digital areas.

**Precision differs between the frame and the wire, by design:** the overlay shows
one decimal (`ZONE 3   LEVEL 42.7%`) because that is the precision the measurement
was taken at, while the backend receives a **whole percentage** (`42`) because
that is what the reporting contract carries.

## Global zone numbering

Digital areas and Ball zones draw from **one machine-wide namespace**, currently
**1–12** (`camera::kMaxZone`). A zone number identifies a reading across the whole
appliance, not within one camera — so they are allocated across cameras, e.g.:

```
Camera 1 → zones 1, 2, 3, 4
Camera 2 → zones 5, 6
```

A zone number is claimed by at most one area or Ball zone machine-wide; saves
reject duplicates, and a zone number the runtime finds claimed twice is rendered
as `Conflict` rather than attributed to a guess.

`kMaxZone` is **not** a per-camera cap. A digit camera has no limit on how many
zones it owns; a Ball camera is capped at four zones by a Ball-specific rule.

## Mode switching

Settings → **Mode** → pick the target → **Switch**, then confirm.

A confirmed switch is **destructive** and runs as one checked transaction. It:

- stops the previous mode's runtime (capture and inference threads are joined);
- stops Backend reporting and sets `brazing.enabled = 0`;
- clears the configured processing setup of **both** modes (`setup_complete`,
  `areas_need_review`);
- writes the new `mode.target`;
- starts the destination mode **unconfigured**;
- **preserves** `brazing.base_url`, camera rows and their connection/capture
  settings;
- closes the Settings dialog once the switch has committed.

**Switching back does not restore anything.** Camera model assignments, areas,
zone numbers, number formats and Ball calibration are cleared by the switch and
must be set up again through the Camera wizard. Camera *connections* survive, so
the operator does not have to retype addresses or credentials.

Re-enabling reporting afterwards is one tick and a Save — the base URL is still
there, and the sender starts immediately with no restart.

## Settings

The dialog has one primary action, **Save changes**, and one secondary,
**Cancel**. There is no competing global Apply button.

- **Save changes** validates every edited page, persists everything in a single
  transaction, and only then applies the runtime effects. If any part fails, the
  dialog stays open with the reason next to the field and **nothing** is applied.
- **Cancel** (and `Esc`, and the window's close box) discards unsaved edits and
  applies nothing. The dark-mode toggle previews live for immediate feedback and
  is restored on cancel; only Save commits it.
- **Save changes stays disabled until something actually differs** from what is
  stored — returning a field to its saved value disarms it again.
- **Backend settings apply immediately**, with no restart and without disturbing
  camera capture or inference.
- The Settings dialog **re-synchronises its Backend checkbox** after a mode
  switch, so it can never show reporting as enabled once the switch has turned it
  off.

Page-scoped controls remain where they belong — the Network page's per-adapter
*Apply* / *Connect*, and the Mode page's *Switch* — because those act on one
adapter or one transaction, not on the form.

## Main toolbar

### Backend status

The top bar shows one of:

| State | Meaning |
|---|---|
| `Backend: OFF` | reporting is disabled, or there is no active reporting pipeline (no usable base URL, or nothing streaming) |
| `Backend: ON` | reporting is enabled and the sender is active |
| `Backend: ERROR` | reporting is enabled, but the most recent delivery failed or is being retried |

`ON` is deliberately **not** labelled "Connected". The backend is a plain HTTP
endpoint with no persistent connection and no health endpoint, so "enabled and
running" is the strongest true claim — `ERROR` is the only state that reflects the
server at all, and it comes from a real delivery attempt.

Clicking the indicator **opens Settings → Server**. It never toggles reporting, so
a stray touch on a production panel cannot stop delivery.

### Refresh Cameras

Tears down and rebuilds every camera runtime from the **existing persisted
configuration**: workers are stopped and joined, stale callbacks from the retired
generation are rejected, and the grid is rebuilt from the same database rows.

It writes nothing, so mode, model assignment, areas, zone numbers, number formats,
Ball calibration and the Backend configuration all survive by construction. An
un-acked Backend snapshot survives too — a camera rebuild is not a mode switch.

It is a **runtime refresh, not a reset**: it cannot bring back configuration that
a mode switch destructively cleared.

## Backend reporting

Configure it at Settings → **Server**.

| Field | Value |
|---|---|
| *Send zone readings to server* | on / off |
| *Server base URL* | `http://SERVER_IP:PORT` — the base only |

The endpoint path is fixed and appended by the application:

```
POST {base_url}/api/brazing/update
Content-Type: application/json

{"zone1":3.00,"zone5":42}
```

Every known zone across every camera is sent as one combined body whenever any
zone's reading settles on a new value. Digital zones carry their configured
decimal format; Ball zones carry a whole percentage.

**URL normalization.** Operators are usually given the full endpoint, so pasting
it is expected and handled: `http://10.0.0.5:8080/api/brazing/update` is stored as
`http://10.0.0.5:8080`. Surrounding whitespace and one trailing slash are also
removed. Anything else — a different path, a missing or non-`http(s)` scheme, a
missing host, embedded credentials, a query or a fragment — is **rejected with a
message**, never silently rewritten. One rule serves the settings form, the
runtime gate and the HTTP client (`src/core/brazing/url.h`), so the UI can never
accept an address the transport would treat differently.

**Delivery is reliable and latest-value-wins.** If the server is unreachable the
app keeps retrying the latest snapshot (exponential backoff, capped at 30 s) and
delivers it when the server returns; newer readings merge into the pending
snapshot while it waits. Only one request is in flight at a time, each bounded by
a 5 s timeout, so an unreachable server cannot hang the UI. Retry state is
in-memory only — a restart begins again from live detection.

A mode switch turns reporting **off** and keeps the address.

## Operator workflow

1. **Settings → Mode** — confirm the appliance is in the mode you want. Switching
   later destroys the processing setup.
2. **Camera → Add / Edit** — the wizard runs *Source → Configure → Models*, then
   *Areas* (digit_reader) or *Level calibration* (ball_leveler).
3. Assign each reported area or Ball zone a **global zone number**, and — for
   Digital areas — a **number format**.
4. **Settings → Server** — tick *Send zone readings to server*, enter the base
   URL, press **Save changes**. The top bar should turn `Backend: ON`.
5. Watch the live grid. Values are drawn into the camera frames themselves.

Up to **four cameras** are displayed at once (`kMaxTiles`). Changing a camera's
source re-verifies its areas: reporting for that camera pauses until they are
re-checked against the new view.

## Configuration and data

Everything lives in the **data dir** — beside the executable by default, or
wherever `$DENSO_DATA_DIR` points (which is how a packaged install keeps state off
the root-owned, upgrade-replaced program directory).

| Path | Contents |
|---|---|
| `<data>/denso.db` | the single SQLite store (schema **v16**, version-gated migrations) |
| `<data>/models/` | TensorRT engines + their `.names.json` sidecars |
| `<data>/models/trt_cache/` | TensorRT cache |
| `<data>/status.json` | machine-readable health for SSH inspection |
| `<data>/denso.log` | bounded rotating log (`.1` … `.4` siblings) |
| `<data>/denso.lock` | single-instance guard |

## Build from source

Every `find_package` is `REQUIRED` — configure fails without it.

- A C++20 compiler (MSVC, GCC or Clang) and CMake ≥ 3.21
- Qt 6 — `Core`, `Gui`, `Sql`, `Widgets`, `Multimedia`, `Network`
- OpenCV
- An inference backend, **platform-split**:
  - **Linux / Jetson** — CUDA Toolkit + TensorRT (`nvinfer` + `NvInfer.h`)
  - **Windows** — ONNX Runtime in `third_party/onnxruntime/` (git-ignored; see
    `docs/GPU_SETUP.md`)
- Network access on first configure (Catch2 is fetched for the tests)

```sh
cmake -S . -B build -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build -- -j6
```

The Jetson has no `ninja` — use `Unix Makefiles`. It has 8 GB of RAM, so do not
over-parallelize.

## Provisioning a model

**No model is tracked in git.** Everything under `models/` is a device- and
version-specific build artifact, so a fresh clone has an empty catalog.

| Machine | Needs | Source |
|---|---|---|
| Windows/MSYS2 (dev) | `models/<name>.onnx` | exported from the `.pt` checkpoint in the training venv |
| Jetson (dev + appliance) | `models/<name>.engine` + `models/<name>.names.json` | built **on-device** from that ONNX with the `trtexec` recipe in `packaging/models.approved` (TRT 10.3 / `sm_87`; the app never builds one at runtime) |

The engine and its sidecar are a **pair** — `TrtEngine` reads class names from the
sidecar, so a mismatched one silently changes what the app reads.

On Windows models are discovered by a **configure-time** glob, so **re-run `cmake`
after adding or replacing an ONNX**. On Linux nothing is copied at build time; the
operator places the engine + sidecar in the data dir's `models/`.

`packaging/models.approved` approves deployable `.engine` + `.names.json` pairs by
SHA-256, and `tools/build_package.sh --model` refuses anything not listed.

## Run from source

```sh
./build/src/app/denso          # exact path varies by generator
```

Point it at a scratch data dir rather than a production one:

```sh
DENSO_DATA_DIR=/tmp/denso-dev ./build/src/app/denso
```

Only one instance may run at a time; a second exits **3**.

Headless modes — used by the installer, no display required:

```sh
denso --version
denso --check [--engine <file>]...   # validate data dir + engines; no primary-DB mutation.
                                     # Run as the target user. Readiness contract:
                                     #   0  Ready
                                     #   10 Degraded (serviceable)
                                     #   78 Blocked
denso --check-running                # 0 running, 1 not running, 4 cannot determine
denso --check-migrations <db-path>   # run the migration chain against that path ONLY
```

`--check` exit **10** is a non-blocking condition, not a failure — `denso-setup`
interprets these codes.

## Automated tests

Catch2 v3, in two binaries plus a packaging harness, all behind `ctest`:

```sh
QT_QPA_PLATFORM=offscreen ctest --test-dir build --output-on-failure
```

| Target | Scope |
|---|---|
| `denso_tests` | pure logic — parsers, formatters, persistence, the domain↔view converter, the zone aggregator and retry policy. Backend-free and fast. |
| `denso_integration_tests` | the real Qt Widgets objects offscreen — `MainWindow`, `SettingsDialog`, `CameraView`, `CameraGrid`, the real wizard pages. |
| `packaging_policy` | the POSIX-sh packaging harness (apt-plan guard, GDM editing, model seeding, preflight drift). |

Tests are hermetic: in-memory or scratch databases, model-less cameras pointed at
a closed loopback port, fake transports. **No test contacts a camera, a backend or
the network**, and none loads a real engine.

At `d3b916d` the suite is **1055 tests**. Platform network backends compile per-OS,
so the count differs between Windows and Linux.

> Catch2 test names are passed back to the binary as CLI arguments by
> `catch_discover_tests`, so keep them **ASCII** and never start one with `--`.

Testing on the real target: connection details, credentials and the
platform/toolchain versions live in `d:\workspace\devices.md`, the shared device
registry outside this repo.

## Package and installation

Ships as a `.deb` for the Jetson (JetPack 6.2 / L4T R36.5.0). Build it **on an
aarch64 Jetson** — there is no cross-toolchain, and engines are `sm_87`/TRT 10.3
pinned:

```sh
tools/build_package.sh --model models/digitv3.engine     # -> dist/
```

That writes four artifacts to `dist/`: the `.deb`, its checksum, a
`preflight-denso-<version>.sh` guard bound to that exact `.deb` by SHA-256, and a
`.tar.gz` transport bundle carrying all of them plus a generated `INSTALL.txt`.

```sh
sudo ./dist/preflight-denso-<version>.sh ./dist/denso-digitalreader_<version>_arm64.deb
sudo apt install --no-install-recommends ./dist/denso-digitalreader_<version>_arm64.deb
sudo denso-setup configure --user <username>
sudo denso-setup verify                                   # expect: verify: PASS
```

Never `dpkg -i` — it does not resolve dependencies. `apt remove` keeps
`/opt/denso/data` (database, engines); `apt purge` removes it.

**Clean builds are reproducible.** Rebuilding one commit on one machine produces a
byte-identical `.deb` and bundle; the build date is the commit timestamp, and file
modes, `tar`/`gzip` metadata and the MANIFEST's `ldd` report are all pinned.
Reproducibility is scoped **per machine** by design — the MANIFEST records the
local toolchain and JetPack stack. Verify with
`tests/manual/repro_build.sh models/digitv3.engine` (Jetson-only).

A bundle is qualified only for the supported configuration (Jetson Orin Nano,
L4T R36.5.0, TensorRT 10.3, CUDA 12.6, `sm_87`), because the `.deb` ships a
prebuilt TensorRT plan — see *Engine compatibility* in `AGENTS.md`.

## Repository structure

Eight CMake targets, split by concern:

```
src/
├─ core/   → denso_core   (Qt Core/Sql + std; never links Qt6::Widgets)
│  ├─ db/         SQLite base + version-gated migrations (currently v16)
│  ├─ mode/       operating mode + the destructive switch-and-reset transaction
│  ├─ camera/     camera + ROI-area domain, zone namespace, source-change logic
│  ├─ level/      Ball Leveler calibration, measurement and persistence
│  ├─ detection/  per-camera model / class config + persistence
│  ├─ models/     manifest, model identity and the ONE model/mode matrix
│  ├─ brazing/    reporting config + the ONE base-URL/endpoint authority
│  ├─ health/     integrity verdict + status.json
│  ├─ network/    domain + persistence + OS backends (windows/ · linux/)
│  ├─ settings/   persisted app settings
│  └─ ui/         Qt-free domain↔view boundary
└─ app/
   ├─ ui/         theme, main window, settings + camera widgets   → denso_app
   ├─ camera/     capture + frame processing                      → denso_camera
   ├─ detection/  inference helpers + backend engines             → denso_detection
   ├─ brazing/    zone aggregation, retry policy, HTTP transport   → denso_brazing
   └─ logging/    bounded 24/7 rotating file log

tests/     → denso_tests + denso_integration_tests
packaging/ → .deb metadata, denso-setup, models.approved, desktop entry
docs/      → ARCHITECTURE.md · MODEL_COMPATIBILITY.md · GPU_SETUP.md · superpowers/
```

`denso_app` is an OBJECT library so the shipped executable and the integration
tests link the **same objects**. The three static subsystem libs are linked by both
`denso` and `denso_tests`, so the tests exercise shipped code rather than a second
compile.

See `CLAUDE.md` for the source map and `AGENTS.md` for the working agreements.

## Operational notes and limitations

- **One mode at a time, and switching is destructive.** Plan mode changes; they
  are not a toggle.
- **Autostart is not configured by default.** `denso-setup configure --autostart
  --enable-autologin` exists and is designed, but its GDM/XDG path has **not**
  been verified on hardware.
- **Engines are not portable.** A serialized TensorRT plan is tied to its GPU
  architecture, TRT version and OS. Build each on its own target; the `.onnx` is
  the portable source of truth.
- **No model is bundled in git.** A fresh clone starts with an empty catalog.
- **Reporting has no queue on disk.** Retry state is in-memory; a restart begins
  from live detection.
- **`Backend: ON` is not a connectivity claim.** There is no health endpoint.
- Reporting pauses for a camera whose source changed until its areas are
  re-verified.

## Development safety

- Work on a branch; `main` is the deployable line.
- Never `git add .` or `git add -A` here — `models/` is not ignored, and a
  multi-megabyte artifact has been swept in that way before. Stage by path.
- Do not point a development build at a production data dir. Use
  `DENSO_DATA_DIR`.
- `/opt/denso/data` belongs to the installed package — leave it to `denso-setup`.
- Engines and their `.names.json` sidecars travel together.
- Re-run `cmake` (not just a rebuild) after adding or replacing a Windows ONNX.
