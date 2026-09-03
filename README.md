# Denso Digital Reader

A Jetson-based desktop application for camera-based industrial reading. It pulls
live camera streams, runs TensorRT inference on them, draws the result back into
the picture, and reports the values to a backend over HTTP. The UI is Qt Widgets;
all state lives in one SQLite store (`denso.db`) in the data dir.

The appliance runs in exactly **one of two operating modes** at a time:

| Mode | Reads | Model family | Models |
|---|---|---|---|
| **Digital Number Reader** (`digit_reader`) | up to four numeric digits per configured area | `digit_numeric` | `digitv3` |
| **Floating Ball Leveler** (`ball_leveler`) | a floating ball's height, as a 0–100 % level | `float_ball` | `float-small`, `float-big` |

The modes are **mutually exclusive**. No model is permitted in both; the mode
matrix lives in exactly one place (`src/core/models/compatibility.cpp`) and fails
closed on anything it does not recognise. Switching between them is destructive —
see [Mode switching](#mode-switching).

> Built with **C++20 / Qt 6 Widgets / CMake**.

**Where the detail lives.** This README is the entry point. The canonical
references are:

| Document | Covers |
|---|---|
| [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) | boot sequence, threading, persistence, headless CLI contract, install/upgrade/packaging architecture, gotchas |
| [`docs/MODEL_COMPATIBILITY.md`](docs/MODEL_COMPATIBILITY.md) | the model/mode matrix, manifest schema 2, artifact integrity, enforcement points, adding a model |
| [`docs/GPU_SETUP.md`](docs/GPU_SETUP.md) | Windows GPU execution providers |
| [`packaging/manifest/README.md`](packaging/manifest/README.md) | the reviewed manifest descriptors and the engine-only artifact policy |
| [`CLAUDE.md`](CLAUDE.md) / [`AGENTS.md`](AGENTS.md) | source map and working agreements |

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
the frame is processed once and each zone selects from the same detections. While
one zone is being edited the camera's other zones stay visible as faint context,
so overlap and relative placement are apparent before saving.

**Precision differs between the frame and the wire, by design:** the overlay shows
one decimal (`ZONE 3   LEVEL 42.7%`) because that is the precision the measurement
was taken at, while the backend receives a **whole percentage** (`42`) because
that is what the reporting contract carries.

## Global zone numbering

Digital areas and Ball zones draw from **one machine-wide namespace**: the whole
numbers **1–99** inclusive (`camera::kMinZone` … `camera::kMaxZone`, tested with
`camera::zone_in_range`). A zone number identifies a reading across the whole
appliance, not within one camera — so they are allocated across cameras, e.g.:

```
Camera 1 → zones 1, 2, 3, 4
Camera 2 → zones 40, 41
```

The number is an **identifier, not an index**: it is typed directly on the Areas
and Ball calibration steps, which list the numbers already in use and name the
holder of one that is entered twice. Values outside the range, decimals, text and
an empty required field all block Save, and attempting Save on a conflicting set
raises an explicit refusal naming the zone. Ball zones are auto-allocated the
next free number ascending from 1.

**`0` is not a zone.** There is exactly one representation of "this area is not
reported": NULL, a disengaged `std::optional<int>`, reached from the UI by
leaving "Report this area to a zone" unchecked. Up to schema v16, `0` was a
_second_ spelling of the same thing; **v17** normalises every legacy
`camera_area.zone = 0` to NULL once, at migration time. After that the runtime
invariant is simply NULL = unassigned, `1`–`99` = assigned, and a `0` reaching a
write chokepoint is refused as out of range exactly as `100` is.

A zone number is claimed by at most one area or Ball zone machine-wide; saves
reject duplicates, and a zone number the runtime finds claimed twice is rendered
as `Conflict` rather than attributed to a guess. The range is enforced
authoritatively in both write chokepoints (`camera::replace_areas` and
`level::save_level_configuration`), not only in the UI.

The range is **not** a per-camera cap. A digit camera has no limit on how many
zones it owns; a Ball camera is capped at four zones by a Ball-specific rule.

> **Backend compatibility.** The payload is sparse
> (`{"zone1":…,"zone45":…,"zone99":…}`). The client targets **1–99**; the known
> local simulator enforces **1–12**; the production backend's real range is
> **unconfirmed**. The unresolved deployment surface is therefore **zones
> 13–99** — confirm the backend accepts those keys before configuring one on a
> live line. Zones 1–12 are unaffected, and `zone0` can no longer be produced by
> any valid configuration.

## Mode switching

Settings → **Mode** → pick the target → **Switch**, then confirm.

A confirmed switch is **destructive** and runs as one checked transaction. It:

- stops the previous mode's runtime (capture and inference threads are joined);
- stops Backend reporting and sets `brazing.enabled = 0`;
- clears the configured processing setup of **both** modes (`setup_complete`,
  `areas_need_review`);
- writes the new `mode.target` and starts the destination mode **unconfigured**;
- **preserves** `brazing.base_url` and `brazing.api_path`, camera rows and their
  connection settings;
- closes the Settings dialog once the switch has committed.

**Switching back does not restore anything.** Camera model assignments, areas,
zone numbers, number formats and Ball calibration are cleared by the switch and
must be set up again through the Camera wizard. Camera *connections* survive, so
the operator does not have to retype addresses or credentials.

## Settings

The dialog has one primary action, **Save changes**, and one secondary,
**Cancel**. There is no competing global Apply button.

- **Save changes** validates every edited page, persists everything in a single
  transaction, and only then applies the runtime effects. If any part fails, the
  dialog stays open with the reason next to the field and **nothing** is applied.
- **Cancel** (and `Esc`, and the window's close box) discards unsaved edits and
  applies nothing. The dark-mode toggle previews live and is restored on cancel.
- **Save changes stays disabled until something actually differs** from what is
  stored — returning a field to its saved value disarms it again.
- **Backend settings apply immediately**, with no restart and without disturbing
  camera capture or inference.

Page-scoped controls remain where they belong — the Network page's per-adapter
*Apply* / *Connect*, and the Mode page's *Switch* — because those act on one
adapter or one transaction, not on the form.

## Main toolbar

### Backend status

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
configuration**. It writes nothing, so mode, model assignment, areas, zone
numbers, number formats, Ball calibration and the Backend configuration all
survive by construction.

It is a **runtime refresh, not a reset**: it cannot bring back configuration that
a mode switch destructively cleared.

## Backend reporting

Configure it at Settings → **Server**.

| Field | Value |
|---|---|
| *Send zone readings to server* | on / off |
| *Protocol* | HTTP or HTTPS |
| *Server address* | an IPv4/IPv6 address or a host name — the address only |
| *Port* | 1–65535, or blank for the protocol's default |
| *Reporting API path* | the path the server exposes; defaults to `/api/brazing/update` |

The page shows the **effective endpoint** — the URL the application will really
post to — and recomputes it as any of those fields is changed:

```
POST {base_url}{api_path}
Content-Type: application/json

{"zone1":3.00,"zone5":42}
```

The path setting is named generically on purpose: the same endpoint carries
**Digital Number Reader** and **Floating Ball Leveler** readings.

**Upgrades need no action.** An installation that has never configured a path
keeps posting to `/api/brazing/update`: the setting is a row in the `settings`
key/value table, and an absent row means the shipped default. There was no schema
migration.

Every known zone across every camera is sent as one combined body whenever any
zone's reading settles on a new value. Digital zones carry their configured
decimal format; Ball zones carry a whole percentage.

**Normalization.** One rule serves the settings form, its endpoint preview, the
runtime gate and the HTTP client (`src/core/brazing/url.h`); there is no second,
more lenient copy anywhere.

*Protocol / Server address / Port* — these three are a **view** of one stored
value. There is no `brazing.protocol` or `brazing.port` row: they are split out of
`brazing.base_url` on load and composed back into it on save, through the same
authority (`split_base_url` / `compose_base_url` in `src/core/brazing/url.h`), so
every address already in the field round-trips byte-for-byte and nothing needed a
migration.

Surrounding whitespace is removed and a host name is lower-cased. A blank port
means the protocol's default and is stored as an address with no port
(`https://server.example.com`) — that shape must keep working, so the port is
validated only when you supply one. An IPv6 address is typed without brackets and
gets them back in the URL. **Rejected with a message**: a port outside 1–65535 or
that is not a plain number; a port typed with no address; and anything but an
address in the address box — `http://…`, `host:8080`, `host/path` — because the
protocol, the port and the path each have their own control now.

A value already stored that holds the whole endpoint still opens correctly:
`http://10.0.0.5:8080/api/brazing/update` splits to `10.0.0.5` + `8080`, and what
is stripped is the **configured** path, whatever it is.

*Reporting API path* — a missing leading slash is added (`api/denso/update` →
`/api/denso/update`) and surrounding whitespace removed; a single trailing slash
is **kept**, because `/x/` and `/x` are different resources to many servers. A
blank field resolves to `/api/brazing/update`, and the field is re-seeded on Save
so you can see that. Rejected with a message: a whole server URL
(`http://another-server/api/update` — scheme and host belong in the base URL), a
protocol-relative or `scheme:` value, a query or a fragment, embedded spaces, an
empty `//` segment, `.`/`..` segments, a bare `/`, characters URLs do not allow,
and a percent-escape of `/`, `.` or `\` (`%2F`, `%2E`, `%5C`) — those carry path
structure past the segment rules, and a server or proxy that decodes before
routing would land somewhere other than the endpoint shown in the preview.
Escapes of ordinary characters (`%20`) are fine. A stored path the rule refuses
starts **no** sender at all rather than
falling back to the default — reporting stops loudly instead of posting somewhere
nobody chose.

**Delivery is reliable and latest-value-wins.** If the server is unreachable the
app keeps retrying the latest snapshot (exponential backoff, capped at 30 s) and
delivers it when the server returns; newer readings merge into the pending
snapshot while it waits. Only one request is in flight at a time, each bounded by
a 5 s timeout, so an unreachable server cannot hang the UI. Retry state is
in-memory only — a restart begins again from live detection.

A mode switch turns reporting **off** and keeps the address and the path.

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
| `<data>/denso.db` | the single SQLite store (schema **v17**, version-gated migrations) |
| `<data>/models/` | TensorRT engines + their `.names.json` sidecars + `manifest.json` |
| `<data>/models/trt_cache/` | TensorRT cache |
| `<data>/status.json` | machine-readable health for SSH inspection |
| `<data>/denso.log` | bounded rotating log (`.1` … `.4` siblings) |
| `<data>/denso.lock` | single-instance guard |

## Requirements and build

Every `find_package` is `REQUIRED` — configure fails without it.

- A C++20 compiler (MSVC, GCC or Clang) and CMake ≥ 3.21
- Qt 6 — `Core`, `Gui`, `Sql`, `Widgets`, `Multimedia`, `Network`
- OpenCV
- An inference backend, **platform-split**:
  - **Linux / Jetson** — CUDA Toolkit + TensorRT (`nvinfer` + `NvInfer.h`)
  - **Windows** — ONNX Runtime in `third_party/onnxruntime/` (git-ignored; see
    [`docs/GPU_SETUP.md`](docs/GPU_SETUP.md))
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
| Jetson (dev + appliance) | `models/<name>.engine` + `models/<name>.names.json` | built on the aarch64 build host with `trtexec` for TRT 10.3 / `sm_87` and shipped inside the `.deb` — the app never builds one at runtime |

The engine and its sidecar are a **pair** — `TrtEngine` reads class names from the
sidecar, so a mismatched one silently changes what the app reads.

On Windows models are discovered by a **configure-time** glob, so **re-run `cmake`
after adding or replacing an ONNX**. On Linux nothing is copied at build time; the
packaged engines are seeded into the data dir's `models/` at install time.

`packaging/models.approved` approves deployable `.engine` + `.names.json` pairs by
SHA-256, and `tools/build_package.sh` refuses anything not listed.

See [`docs/MODEL_COMPATIBILITY.md`](docs/MODEL_COMPATIBILITY.md) for the manifest
format, the enforcement points and the add-a-model checklist, and
[`packaging/manifest/README.md`](packaging/manifest/README.md) for the reviewed
descriptors.

## Run from source

```sh
./build/src/app/denso          # exact path varies by generator
```

Only one instance may run at a time; a second exits **3**.

### Disposable test data (`build/test-data`)

Point a development run at a scratch data dir rather than a production one. Keep
it inside the build tree, which is already git-ignored (`/build`), so the runtime
state a test run leaves behind can never dirty the working tree:

```sh
DENSO_DATA_DIR=/path/to/repo/build/test-data ./build/src/app/denso
```

To drive the GUI on a machine with a graphical session — the `DISPLAY` and
`XAUTHORITY` values below are examples from one validated Jetson session, so
**substitute your own active session's values**:

```sh
cd /path/to/repo/build/src/app

DISPLAY=:1 \
XAUTHORITY=/run/user/1000/gdm/Xauthority \
DENSO_DATA_DIR=/path/to/repo/build/test-data \
./denso
```

`build/test-data` is **disposable development and test runtime state**. It is not
production data and has nothing to do with `/opt/denso/data` — deleting it costs
nothing and the next run recreates what it needs. It holds the isolated
`denso.db`, `denso.log`, `status.json`, `denso.lock`, and a `models/` directory
the application reads.

**Do not copy production data into it.** Reproduce the state you need instead.

That `models/` needs a `manifest.json` alongside the engines, or every artifact
reads back as `model_undeclared`, `--check` returns **10**, and the camera wizard
offers no model to select. The engines and sidecars themselves may be **symlinks**
to a shared copy, so a scratch dir costs no disk. See
[`packaging/manifest/README.md`](packaging/manifest/README.md) for the reviewed
descriptors the manifest is generated from.

Headless modes, used by the installer — no display required:

| Mode | Contract |
|---|---|
| `denso --version` | prints the version; no lock, no mutation |
| `denso --check [--engine <file>]…` | validates data dir + engines. **0** Ready, **10** Degraded (serviceable), **78** Blocked |
| `denso --check-running` | **0** running, **1** not running, **4** cannot determine |
| `denso --check-migrations <db-path>` | applies the chain to a **throwaway copy you supply**. Not read-only, and it *creates* a missing file — point it only at a copy |
| `denso --apply-migrations` | migrates the **live** database under `DENSO_DATA_DIR`, in place. Takes no path on purpose. **0** at the supported schema, **78** blocked |

`--check` exit **10** is a non-blocking condition, not a failure. The full
contract, including why the two migration commands are split, is in
[`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) under *Headless modes*.

## Automated tests

Catch2 v3, in two binaries plus a packaging harness, all behind `ctest`:

```sh
QT_QPA_PLATFORM=offscreen ctest --test-dir build --output-on-failure
bash tests/packaging/run.sh                    # the POSIX-sh packaging harness
```

| Target | Scope |
|---|---|
| `denso_tests` | pure logic — parsers, formatters, persistence, the domain↔view converter, the zone aggregator and retry policy. Backend-free and fast. |
| `denso_integration_tests` | the real Qt Widgets objects offscreen — `MainWindow`, `SettingsDialog`, `CameraView`, `CameraGrid`, the real wizard pages. |
| `packaging_policy` | the packaging harness (apt-plan guard, GDM editing, model seeding, preflight drift). |

Tests are hermetic: in-memory or scratch databases, model-less cameras pointed at
a closed loopback port, fake transports. **No test contacts a camera, a backend or
the network**, and none loads a real engine.

> Catch2 test names are passed back to the binary as CLI arguments by
> `catch_discover_tests`, so keep them **ASCII** and never start one with `--`.

Connection details and credentials for the real target live in the shared device
registry outside this repo.

## Install and upgrade

The appliance ships as a `.deb` for JetPack 6.2 / L4T R36.5.0. The architecture
behind everything below — operator resolution, the two ownership domains, the
upgrade gate and its invariants — is in
[`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) under *Install and runtime
architecture* and *Upgrade architecture*.

### Fresh install

```bash
sudo apt install ./denso-digitalreader_<version>_arm64.deb
```

That is the whole procedure — there is **no** separate `denso-setup configure`
step. The install resolves the operator account (recorded user, else `SUDO_USER`,
else exactly one acceptable local active non-remote session user; root, system
accounts and ambiguity all **fail the install rather than guess**), creates and
hands over `/opt/denso/data`, seeds the packaged models and manifest, enables
autostart, and fails the installation if verification is incomplete.

```
denso: fresh installation configured.
denso: operator user: <resolved-user>
denso: autostart: enabled
denso: autologin: unchanged
```

Autologin is never enabled — the box still stops at the greeter on power-on.

### Upgrade

```bash
# 1. Stop Denso, then confirm — TRI-STATE, only rc 1 is safe to continue:
denso-digitalreader --check-running     # 1 = stopped (0 = running, 4 = cannot tell)

# 2. Run the release preflight guard against the .deb it was generated for:
sudo ./preflight-denso-<version>.sh ./denso-digitalreader_<version>_arm64.deb

# 3. Upgrade in place:
sudo apt install ./denso-digitalreader_<version>_arm64.deb

# 4. Verify:
denso-digitalreader --check             # 0 Ready, 10 Degraded-serviceable

# 5. Launch through the menu entry or /usr/bin/denso-digitalreader.
```

- **No uninstall first.** Upgrade in place; do not `apt remove` the old package.
- **Your database is preserved.** dpkg never touches `/opt/denso/data`.
- **The pre-migration backup is automatic** whenever a migration is required:
  `/opt/denso/data/denso.db.pre-v<schema>`, created *and verified* before
  anything writes to the live database. Backups accumulate on purpose and are
  never pruned automatically.
- **If migration or integrity verification fails, package configuration fails and
  Denso stays stopped.** The message names the backup path. Fix the cause, then
  `sudo dpkg --configure -a` — it reuses the same backup rather than overwriting
  it.
- **Recovery is manual.** Nothing is ever restored or rolled back automatically.
- **Never `apt purge`** if `/opt/denso/data` must be retained — purge deletes the
  database, the operator's engines and every `denso.db.pre-v*` backup.
- **Use `apt install`, never a bare `dpkg -i`** — `dpkg -i` does not resolve
  dependencies.

### Service lifecycle

systemd `--user` is the **only** thing that starts Denso, so
`systemctl --user disable` genuinely stops it starting and a `.deb` upgrade will
not re-enable it.

```bash
systemctl --user status|start|stop|restart denso-digitalreader
systemctl --user enable --now denso-digitalreader     # start at every graphical login
systemctl --user disable --now denso-digitalreader

sudo journalctl _SYSTEMD_USER_UNIT=denso-digitalreader.service -f
```

> This appliance runs a **volatile** journal with no per-user journal files, so
> that is the supported `journalctl` invocation.

**Service status is not application health:**

| Question | Command |
|---|---|
| Is the service running? | `systemctl --user status denso-digitalreader` |
| Is an instance holding the lock? | `denso-digitalreader --check-running` |
| Is the app, database and model set healthy? | `denso-digitalreader --check` |

If the journal says *"no graphical session is available"*, that is the guard doing
its job: Denso is a GUI and there is no display for it. Log in graphically. **No
`DISPLAY` value is ever invented.**

## Building the Debian package

**The build must run on an aarch64 Jetson.** There is no cross-toolchain, and the
shipped engines are pinned to `sm_87` / TRT 10.3. The script hard-refuses anything
else: `arm64`, L4T **exactly** `36.5.0`, TensorRT 10.3 and CUDA 12.6.

The build needs no `sudo`, and refuses a dirty tree unless you pass
`--allow-dirty` (dirtiness is judged by `git status --porcelain`, so an untracked
file counts — otherwise the package's MANIFEST would lie about what was built).

```bash
git switch main && git pull --ff-only
git status --short                          # MUST be empty

tools/build_package.sh --models-dir models  # THE RELEASE CUT — both product modes
```

`--models-dir` is the canonical release input. It requires **exactly** the six
canonical files — `digitv3`, `float-small`, `float-big`, each with its
`.names.json` — and refuses a missing engine, a missing sidecar, an unexpected
fourth engine, and any `.pt` or `.onnx` (production packaging is TensorRT-engine
only). It hands the engines downstream in reviewed manifest order, which is part
of the pinned Release-B manifest identity the build asserts.

`--model` remains for one-off diagnostic builds and is mutually exclusive with
`--models-dir`. A package built from `--model models/digitv3.engine` alone is
valid, installable, and **silently ships without the entire Floating Ball Leveler
mode** — which is why `--models-dir` exists.

Four files land in `dist/` (git-ignored):

| File | Purpose |
|---|---|
| `denso-digitalreader_<version>_arm64.deb` | the package |
| `…deb.sha256` | its checksum |
| `preflight-denso-<version>.sh` | JetPack-stack guard, bound to **that exact** `.deb` by SHA-256 |
| `…_arm64.tar.gz` | transport bundle: the `.deb`, its guard, `SHA256SUMS`, `INSTALL.txt` |

Version strings are `<APP_VERSION>+r<commit-count>.g<short-sha>[+dirty]`. The
commit count leads because a git SHA is not monotonic and dpkg compares
non-digits by ASCII, so apt would otherwise call a newer build a downgrade.

Inspect before shipping:

```bash
sha256sum -c dist/denso-digitalreader_<version>_arm64.deb.sha256
dpkg-deb --info     dist/denso-digitalreader_<version>_arm64.deb
dpkg-deb --contents dist/denso-digitalreader_<version>_arm64.deb
```

To move it to another **compatible, validated** appliance, send the bundle:

```bash
scp dist/denso-digitalreader_<version>_arm64.tar.gz <user>@<host>:~/
# on that appliance: tar xzf …, sha256sum -c SHA256SUMS, then follow INSTALL.txt
```

"Compatible" is load-bearing: the `.deb` carries a prebuilt TensorRT plan, so a
bundle is qualified only for Jetson Orin Nano / L4T R36.5.0 / TRT 10.3 / CUDA
12.6 / `sm_87`.

The full pipeline — staging, manifest generation and the reviewed-identity
assertion — is documented in [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) under
*Packaging & ship pipeline*.

## Operating an installed appliance

`<operator>` below is the account recorded at install time, readable with
`cat /opt/denso/install-state/user`. No username is hardcoded anywhere.

### Health and verification

```bash
cat /opt/denso/data/status.json                 # machine-readable health
tail -n 200 /opt/denso/data/denso.log           # rotated siblings: .1 … .4
dpkg -l denso-digitalreader                     # state iU means unconfigured
sudo denso-setup verify                         # expect: verify: PASS
```

`verify` confirms the app is not running, backs up the WAL set to a unique
`/opt/denso/data/backup-<timestamp>-XXXXXX/`, runs the migration chain against a
**throwaway copy**, then runs `denso --check` as the target user. It is a test on
a copy — it does **not** migrate the live database.

Read the live schema and integrity with the shipped helper (there is no `sqlite3`
CLI on the Jetson image):

```bash
sudo -u <operator> python3 /opt/denso/lib/denso-db-helper user-version    /opt/denso/data/denso.db
sudo -u <operator> python3 /opt/denso/lib/denso-db-helper integrity-check /opt/denso/data/denso.db
sudo -u <operator> python3 /opt/denso/lib/denso-db-helper backup          /opt/denso/data/denso.db /path/to/snapshot.db
```

Expect `integrity: ok` and a schema equal to `SCHEMA_VERSION` in
`src/core/db/db.cpp` — check it there rather than trusting a number written here.
`backup` uses SQLite's online backup API, which is consistent under WAL; it
refuses an existing destination and errors on a mistyped source rather than
creating an empty database. Run it as the data-dir owner, never as root.

A `[trt] Using an engine plan file across different models of devices` warning is
expected — it appears on the build host too — and does not indicate a problem.

### Recovery

**The upgrade was refused.** `prerm` reports the app is running, or cannot tell.
Close the app and retry. A refused upgrade is safe: the **old** package is still
installed and configured. Exit `4` usually means a root-owned lock file in an
operator-owned data dir:

```bash
ls -l /opt/denso/data/denso.lock
sudo chown -R <operator>:<operator> /opt/denso/data
```

**The migration failed.** The live database is mid-upgrade and `postinst` halted
rather than reporting success. Its message names the pre-migration backup at
`/opt/denso/data/denso.db.pre-v<schema>`, which was **not** restored
automatically. Restore it before doing anything else, then capture
`/opt/denso/data/denso.log` — a failed migration is a defect, not an operator
error.

**`apply-migrations: BLOCKED` naming a newer schema.** The database was written by
a newer build than the one installed. Downgrades are not supported; install the
newer package again. The database is untouched, because the guard runs before the
chain opens it for writing.

**Restore a database backup.** Restore the **whole set** — the `-wal` matters,
because a live WAL beside a restored older `denso.db` pairs the database with an
unrelated write-ahead log that SQLite will happily replay over your restore. With
the app closed, as `<operator>`:

```bash
# 1. List candidates, newest first, and choose one.
ls -dt /opt/denso/data/denso.db.pre-v* /opt/denso/data/backup-*/ 2>/dev/null

# 2. Set BK to the directory you chose — paste the REAL path.
BK=/opt/denso/data/backup-20260803-101500-Ab3xYz

# 3. Restore, fail-fast, in a separate shell process. `set -eu` matters: a failed
#    WAL removal or copy must stop the sequence, not be followed by commands that
#    pair unrelated database and WAL files. It runs in its own process because
#    bash ignores errexit inside a compound command whose status is tested.
sudo -u <operator> bash -c '
    set -eu
    dst=/opt/denso/data
    src=$1
    [ -d "$src" ]
    [ -f "$src/denso.db" ]
    rm -f "$dst/denso.db-wal" "$dst/denso.db-shm"
    cp -p "$src/denso.db" "$dst/denso.db"
    if [ -f "$src/denso.db-wal" ]; then
        cp -p "$src/denso.db-wal" "$dst/denso.db-wal"
    fi
' _ "$BK" && echo "restored from $BK"
```

No output from the final `echo` means the restore aborted part-way — re-read the
error and do **not** launch until the schema/integrity check above is sound.
Restoring an older database under a **newer** binary leaves the migration
pending, so roll the package back too, or re-run the gate deliberately.

**Roll the package back.** Versions sort by `r<count>`, so going back is a
downgrade as far as apt is concerned:

```bash
sudo apt install --no-install-recommends --allow-downgrades \
     ./dist/denso-digitalreader_<older-version>_arm64.deb
sudo denso-setup verify
```

**A model looks wrong.** `configure` deliberately keeps an operator's differing
model rather than overwriting it, so take the packaged one back explicitly:

```bash
sudo denso-setup replace-model <stem>
```

**Removing the package.**

```bash
sudo apt remove denso-digitalreader      # keeps /opt/denso/data (database, engines)
sudo apt purge  denso-digitalreader      # also removes it
```

> **Never `purge` when operator data must be preserved.** `purge` runs
> `rm -rf /opt/denso/data`, deleting the database, the operator's engines **and
> every `denso.db.pre-v*` pre-migration backup**. There is no undo. Use
> `apt remove` to replace or reinstall; an upgrade never purges. If you genuinely
> intend to discard operator data, copy anything needed outside `/opt/denso`
> first.

## Repository structure

Eight CMake targets, split by concern:

```
src/
├─ core/   → denso_core   (Qt Core/Sql + std; never links Qt6::Widgets)
│  ├─ db/         SQLite base + version-gated migrations (currently v17)
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
docs/      → ARCHITECTURE.md · MODEL_COMPATIBILITY.md · GPU_SETUP.md
```

`denso_app` is an OBJECT library so the shipped executable and the integration
tests link the **same objects**. The three static subsystem libs are linked by both
`denso` and `denso_tests`, so the tests exercise shipped code rather than a second
compile.

## Operational notes and limitations

- **One mode at a time, and switching is destructive.** Plan mode changes; they
  are not a toggle.
- **Zones above 12 are not yet qualified against the production backend.** The
  client supports **1–99**; only **1–12** is within the previously known backend
  range. Do not configure a zone above 12 on a live line until the backend has
  been qualified for `zone13`…`zone99` — see
  [Global zone numbering](#global-zone-numbering).
- **Autostart is enabled by a fresh install, autologin is not.** An upgrade never
  re-enables a service the operator disabled.
- **Engines are not portable.** A serialized TensorRT plan is tied to its GPU
  architecture, TRT version and OS, so each target needs its own build.
  Production packaging is **engine-only**: `tools/build_package.sh` refuses any
  `.onnx` or `.pt`, and `packaging/models.approved` approves the `.engine` +
  `.names.json` pair by SHA-256. The Windows development build is the one place an
  ONNX is still loaded directly.
- **No model is bundled in git.** A fresh clone starts with an empty catalog.
- **Reporting has no queue on disk.** Retry state is in-memory; a restart begins
  from live detection.
- **`Backend: ON` is not a connectivity claim.** There is no health endpoint.
- Reporting pauses for a camera whose source changed until its areas are
  re-verified.

## Development safety

- Work on a branch; `main` is the deployable line.
- Never `git add .` or `git add -A` here. Everything under `models/` is ignored
  by pattern (`*.engine`, `*.names.json`, `*.onnx`, `*.pt`, `trt_cache/`), but a
  multi-megabyte artifact was swept in that way before those patterns existed,
  and a new artifact type would not be covered either. Stage by path.
- Do not point a development build at a production data dir. Use
  `DENSO_DATA_DIR`.
- `/opt/denso/data` belongs to the installed package — leave it to `denso-setup`.
- Engines and their `.names.json` sidecars travel together.
