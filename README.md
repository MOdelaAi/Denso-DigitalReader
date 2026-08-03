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
denso --check-migrations <db-path>   # APPLIES the migration chain to that path.
                                     # Writes — see the warning below.
```

`--check` exit **10** is a non-blocking condition, not a failure — `denso-setup`
interprets these codes.

> **`--check-migrations` is not read-only.** Despite the name it opens the
> database you name and migrates it, creating tables and converting legacy rows.
> Aimed at a copy it validates the chain; aimed at the live database it upgrades
> the live database — which is exactly how a deployment applies a pending
> migration (see *Build and deploy the Debian package*, step 10). It also creates
> the file if it does not exist, so a mistyped path yields a new empty database
> and still reports `ok`.

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

## Build and deploy the Debian package

The appliance ships as a `.deb` for JetPack 6.2 / L4T R36.5.0. Everything below is
copy-pasteable in order; nothing an operator needs is left undocumented.

**The build must run on an aarch64 Jetson.** There is no cross-toolchain, and the
shipped TensorRT engines are pinned to `sm_87` / TRT 10.3. The script hard-refuses
anything else — it checks `dpkg --print-architecture` is `arm64`, that L4T is
**exactly** `36.5.0`, and that TensorRT 10.3 and CUDA 12.6 are the installed
baseline. A warning would not be enough: a mismatched box can otherwise produce a
package whose control file swears JP6.2 while the engines inside fail to
deserialize on the target.

**The build needs no `sudo`.** Only the install steps do.

**On notation:** `<angle-bracketed>` words are placeholders — substitute the real
value before running the line. They are not valid shell, so a block containing one
will fail loudly rather than do something unintended. The two multi-command blocks
where that would be dangerous (the backup and the restore) use a real example path
you overwrite instead.

### 1. Pre-build checks

```bash
cd ~/project/Denso-DigitalReader

git switch main
git pull --ff-only

git status --short          # MUST be empty — see below
git rev-parse HEAD
git rev-parse origin/main   # should equal HEAD
```

The tree **must be clean**. The build refuses a dirty tree unless you pass
`--allow-dirty`, and "dirty" is judged by `git status --porcelain`, so an
*untracked* file counts — otherwise the package's MANIFEST would lie about what
was built.

Confirm the platform and that the engine you intend to ship is approved:

```bash
uname -m                                    # aarch64
sed -n 's/^# R\([0-9]*\).*REVISION: \([0-9.]*\).*/\1.\2/p' /etc/nv_tegra_release | head -1   # 36.5.0
cat packaging/models.approved               # stem, engine sha256, sidecar sha256
sha256sum models/digitv3.engine models/digitv3.names.json
```

### 2. Build the package

```bash
# Release A — the digit reader alone
tools/build_package.sh --model models/digitv3.engine

# Release B — all three generations, in EXACTLY this order
tools/build_package.sh \
    --model models/digitv3.engine \
    --model models/float-small.engine \
    --model models/float-big.engine
```

Run it from the repository root. Model paths are resolved relative to the repo
root regardless of where you invoke it from.

What the command does and does **not** do:

| | |
|---|---|
| Needs `sudo` | **No** |
| Cleans first | **No** — it builds into `build-pkg/` incrementally. For a from-scratch build run `rm -rf build-pkg` first. (`build-pkg/` is separate from the `build/` you use for tests, so packaging never disturbs your dev build.) |
| Copies `models/` wholesale | **No** — never a glob. Each engine is named explicitly with `--model`. |
| Permitted artifacts | A `.engine` **and** its matching `<stem>.names.json` sidecar, **both** listed in `packaging/models.approved` with a matching SHA-256. The pair is approved, never the engine alone: `TrtEngine` reads class names from the sidecar, so a swapped sidecar changes what the app reads as much as a swapped engine would. |
| Duplicate `--model` stems | Rejected before staging |
| Output directory | `dist/` (git-ignored) |

**Model-set gate.** Two model sets have reviewed manifest identities, and the
build asserts the generated manifest hashes to the reviewed value:

- `digitv3` alone (Release A);
- `digitv3 float-small float-big`, **in that order** (Release B).

Generation order is part of the manifest bytes, so the Release-B set passed in any
other order is **refused outright** rather than quietly built unpinned — an
unpinned build still cuts a valid, installable package, but one whose manifest
nobody reviewed.

**Version derivation** — `<APP_VERSION>+r<commit-count>.g<short-sha>[+dirty]`,
e.g. `0.1.0+r217.gcf810d1`:

- `APP_VERSION` is read from `src/app/CMakeLists.txt`;
- `r<count>` is `git rev-list --count HEAD`. It leads because a git SHA is **not**
  monotonic and dpkg compares non-digits by ASCII — `0.1.0+gda30437` sorts above a
  newer `0.1.0+g7a2d661`, so apt would call a newer build a downgrade and refuse
  it. The count always increases; the sha stays for provenance.

**Clean builds are reproducible.** The build date is the commit timestamp, and a
clean build refuses a `SOURCE_DATE_EPOCH` that differs from it — a name carrying
no content hash can only be honest if the bytes follow from the commit alone. A
`--allow-dirty` build may override it freely, because its bundle name carries the
`.deb`'s own hash. Verify with `tests/manual/repro_build.sh models/digitv3.engine`
(Jetson-only). Reproducibility is scoped **per machine** by design: the MANIFEST
records the local toolchain and JetPack stack.

### 3. Locate the artifacts

Four files land in `dist/`:

```bash
ls -l dist/
```

| File | Purpose |
|---|---|
| `denso-digitalreader_<version>_arm64.deb` | the package |
| `denso-digitalreader_<version>_arm64.deb.sha256` | its checksum |
| `preflight-denso-<version>.sh` | JetPack-stack guard, bound to **that exact** `.deb` by SHA-256 |
| `denso-digitalreader_<version>_arm64.tar.gz` | transport bundle: the `.deb`, its guard, `SHA256SUMS` and a generated `INSTALL.txt` |

A `--allow-dirty` build appends the `.deb`'s hash to the bundle name
(`..._arm64.<12-hex>.tar.gz`), because every dirty build at one commit produces
the identical version string — without the suffix two materially different
archives would share a name and silently overwrite each other. The exact names are
printed at the end of the build.

### 4. Inspect and checksum

Stay in the repository root — later steps use `./dist/...` paths:

```bash
sha256sum -c dist/denso-digitalreader_<version>_arm64.deb.sha256

dpkg-deb --info     dist/denso-digitalreader_<version>_arm64.deb   # control, deps, version
dpkg-deb --contents dist/denso-digitalreader_<version>_arm64.deb   # file map
```

The package declares its runtime dependencies (Qt 6 platform plugins, the SQLite
driver, GStreamer + `nvidia-l4t-gstreamer`, NetworkManager, `procps`, `python3`);
CUDA, TensorRT and OpenCV come from the JetPack image and are deliberately not
vendored.

### 5. Stop the application

The app must be closed **before** the backup, not after it. A running instance can
write between the copy of `denso.db` and the copy of `denso.db-wal`, leaving a
mismatched pair that looks restorable and is not — which is exactly why
`denso-setup verify` refuses to run at all while an instance is live.

Quit it from the GUI, then confirm:

```bash
sudo -u modela env DENSO_DATA_DIR=/opt/denso/data /opt/denso/bin/denso --check-running
echo $?
```

| Exit | Meaning |
|---|---|
| `0` | still running — close it |
| `1` | not running — proceed |
| `4` | cannot determine — **do not proceed** |

An exit of `4` usually means a root-owned lock file in an operator-owned data dir,
left by running a check under plain `sudo`:

```bash
ls -l /opt/denso/data/denso.lock
sudo chown -R modela:modela /opt/denso/data
```

The package enforces this too: `prerm` **refuses** an upgrade or removal while an
instance is running, and reads `--check-running` as the tri-state it is — `4` is
treated as unsafe, never as "probably fine". A refused upgrade leaves the **old**
package installed, which is the correct outcome.

### 6. Create a verified pre-upgrade database backup

**Take your own backup before the upgrade touches anything.** `denso-setup verify`
does create one, but it runs *after* the migration step below — so it is not the
backup that protects you from the migration.

Create the directory with `mktemp -d`, and capture the path from the command that
created it. Do not name a directory by timestamp and then rediscover it with `ls`:
second-resolution names collide, and picking up the wrong directory pairs a
`denso.db` with a stranger's `-wal`, which is worse than no backup because it
looks restorable. (`denso-setup verify` uses `mktemp -d` for the same reason.)

Copy the **WAL set** (`denso.db` **and** `denso.db-wal`), **as the target user** —
a root-owned file left in an operator-owned data dir is one the app can no longer
write:

```bash
BK=$(sudo -u modela mktemp -d "/opt/denso/data/pre-upgrade-$(date +%Y%m%d-%H%M%S)-XXXXXX")
echo "backup dir: $BK"

sudo -u modela bash -c '
    set -eu
    src=/opt/denso/data
    dst=$1
    [ -d "$dst" ]
    cp -p "$src/denso.db" "$dst/denso.db"
    # Test for the WAL, then copy it UNGUARDED — never `2>/dev/null || true`.
    if [ -f "$src/denso.db-wal" ]; then
        cp -p "$src/denso.db-wal" "$dst/denso.db-wal"
    fi
' _ "$BK" && sudo -u modela python3 - "$BK/denso.db" <<'PY'
import sqlite3, sys
db = sqlite3.connect(sys.argv[1])
print("integrity:", db.execute("PRAGMA integrity_check").fetchone()[0])
print("schema   :", db.execute("PRAGMA user_version").fetchone()[0])
PY
```

Expect `integrity: ok` and the schema number the appliance is currently on — that
is the version you would be rolling back to. **If the copy failed you will see the
`cp` error and no integrity output at all**, because the check is chained with
`&&` and never runs.

Do not proceed until you have seen `integrity: ok`.

The check runs through `sudo -u modela` too: `mktemp -d` creates the directory
mode 0700 owned by `modela`, so reading it as anyone else fails on permissions.
That keeps the procedure identical whichever sudo-capable account you are using.

The `-wal` is optional only in the sense that the file may not exist. If it does
exist, copying it is **not** optional — a crash can leave committed transactions
there, so `denso.db` alone is a stale snapshot.

That is why the copy is guarded by an existence **test** rather than by
`2>/dev/null || true`: suppressing errors would treat a WAL that failed to copy —
I/O error, permissions, no space — exactly like a WAL that was never there, and
you would carry on with a backup that is silently missing committed data.

And it is why the copies run under `set -eu` **in a separate `bash -c` process**.
An interactive shell happily continues after a failed `cp`, and a failed main
copy would otherwise be followed by the integrity check — which would open, and
CREATE, an empty `"$BK/denso.db"` and report `integrity: ok` on nothing.

The separate process matters: bash **ignores `errexit` inside a compound command
whose status is being tested**, and that suppression applies even to a `set -e`
written inside it. So `if ( set -eu; … ); then` does *not* abort on the first
failure. A fresh shell process is not in that context, which is exactly why
`denso-setup verify` runs its own backup as `runuser … sh -c 'set -eu; …' || die`.

The names inside the directory must stay canonical (`denso.db`, `denso.db-wal`) so
SQLite associates them.

There is **no `sqlite3` CLI** on the Jetson image; `python3` is a package
dependency and is the supported way to read these pragmas.

### 7. Install or upgrade the package

On the build box, install straight out of `dist/` — **both steps, in this order**:

```bash
sudo ./dist/preflight-denso-<version>.sh ./dist/denso-digitalreader_<version>_arm64.deb
sudo apt install --no-install-recommends ./dist/denso-digitalreader_<version>_arm64.deb
```

The preflight simulates the apt transaction and refuses if installing would touch
a protected JetPack package. It is bound to that one `.deb` by SHA-256 and rejects
any other, which is why the two travel together.

**Never `dpkg -i`** — it does not resolve dependencies.

On **another compatible, validated appliance**, move the single bundle instead:

```bash
scp dist/denso-digitalreader_<version>_arm64.tar.gz <user>@<host>:~/
# then on that appliance:
ssh <user>@<host>
tar xzf denso-digitalreader_<version>_arm64.tar.gz
cd denso-digitalreader_<version>_arm64
sha256sum -c SHA256SUMS
cat INSTALL.txt          # then follow it
```

"Compatible" is load-bearing: the `.deb` carries a prebuilt TensorRT plan, so a
bundle is qualified only for Jetson Orin Nano / L4T R36.5.0 / TensorRT 10.3 /
CUDA 12.6 / `sm_87`.

An upgrade keeps `/opt/denso/data` — dpkg never touches it, which is why all
mutable state lives there and not beside the root-owned, upgrade-replaced binary.
**That is also why the database arrives at the new binary still on the old
schema**, and why step 10 exists.

### 8. Configure the installation

`postinst` is structural only (it creates directories); everything requiring
judgement is an explicit operator step:

```bash
sudo denso-setup configure --user modela
```

`--user` is **required** and is not inferred from `$SUDO_USER` (absent under
automation, wrong from a root shell). It records the target user, sets data-dir
ownership, and seeds the packaged models into `/opt/denso/data/models` **as that
user**. A model already present and identical is left alone; one that differs is
**kept**, not overwritten.

Autostart is **opt-in and not verified on hardware**:

```bash
sudo denso-setup configure --user modela --autostart --enable-autologin
```

XDG autostart only fires after a graphical login, so `--autostart` without
`--enable-autologin` still stops at the GDM greeter on power-on. This path is
designed but **has not been validated on real hardware** — treat it as untested.

### 9. Seed the model manifest

```bash
sudo denso-setup seed-manifest
```

Writes the packaged manifest to `/opt/denso/data/models/manifest.json` (where the
app reads it) atomically. Usually a no-op — `configure` already seeds it — and it
prints `manifest already current` when so. It **refuses** rather than overwriting
if the target differs, is a symlink, is not a regular file, or is unreadable;
remove or correct it and re-run.

### 10. Apply pending migrations to the live database

> **This step modifies `/opt/denso/data/denso.db` in place. Do not run it until
> step 6's backup exists and reported `integrity: ok`.**

It must run as the **target user**, never as root — a root-created WAL or journal
file in an operator-owned data dir is one the app can no longer write.

If you are logged in as `modela`, that is simply:

```bash
denso-digitalreader --check-migrations /opt/denso/data/denso.db
```

From any other shell, name the user explicitly rather than assuming whose it is:

```bash
sudo -u modela denso-digitalreader --check-migrations /opt/denso/data/denso.db
```

`sudo -u modela` is fine — what must not happen is running it as **root**.

Expect `check-migrations: ok (/opt/denso/data/denso.db)`.

**`--check-migrations` is not read-only, despite its name.** It opens the database
you name and runs the migration chain against it: new tables are created and
legacy configuration is migrated. Point it at the live database and the live
database is migrated. Point it at a copy and only the copy is.

That dual use is exactly why the name misleads. `denso-setup verify` calls it
against a **throwaway copy** to prove the chain *would* apply; this step calls it
against the **live database** to actually apply it. Both are the same command with
different targets, and only the second changes the appliance.

Two hazards worth knowing before you run it:

- **A mistyped path is silently created.** The database is opened with SQLite's
  default create-if-missing behaviour, so a wrong path produces a brand-new empty
  database, migrates it to the current schema, and reports `ok`. That looks
  identical to success. Check the path.
- **It does not take the single-instance lock.** Nothing stops it running against
  a database a live app is using. Step 5 is a prerequisite, not a formality.

Why this step is not optional, and why `verify` alone is not enough: an upgrade
replaces the binary but never touches `/opt/denso/data`, so the new binary meets
the old schema. This was observed on a real upgrade — the live database was at
schema 14; `denso-setup verify` migration-tested a throwaway copy successfully,
yet the runtime check still failed because the *live* database was still at 14.
Running `--check-migrations` against `/opt/denso/data/denso.db` moved it to 15,
created the new tables, migrated the legacy Ball configuration, and `verify` then
passed.

A normal GUI launch will also open and migrate its configured database. **The
deployment workflow does not rely on that.** Migrate and verify explicitly first,
so a migration failure surfaces at a controlled moment with a fresh backup beside
it — not on an operator's first launch.

### 11. Verify the live schema and integrity

Confirm the live database actually moved, and is sound:

```bash
sudo -u modela python3 - /opt/denso/data/denso.db <<'PY'
import sqlite3, sys
db = sqlite3.connect(sys.argv[1])
print("integrity:", db.execute("PRAGMA integrity_check").fetchone()[0])
print("schema   :", db.execute("PRAGMA user_version").fetchone()[0])
PY
```

Expect `integrity: ok` and a schema equal to the version the shipped source
declares. **At this commit that is `16`** (`SCHEMA_VERSION` in
`src/core/db/db.cpp` — check it there rather than trusting this number after a
future release).

If the schema is still the old value, step 10 did not take effect: check you ran
it against `/opt/denso/data/denso.db` and not a typo'd path, and re-read its
output.

### 12. Run the installation gate

```bash
sudo denso-setup verify         # expect: verify: PASS
```

`verify` is the final gate. It does five things in order:

1. confirms the app is **not running** (a live WAL database cannot be validated);
2. **backs up** `denso.db` + `denso.db-wal` to a unique
   `/opt/denso/data/backup-<timestamp>-XXXXXX/` directory;
3. runs the migration chain against a **throwaway copy** of the live database —
   proving the chain applies, without changing the live database;
4. runs `denso --check` against every packaged engine, as the target user;
5. reports `verify: PASS` or the specific failure.

Read item 3 above for what it is: a **test on a copy**. It does not migrate the live
database, which is why step 10 exists and must come first. If you skip step 10,
`verify` can pass its migration test and still fail its runtime check, because the
live schema is untouched.

The `--check` in item 4 speaks the readiness contract:

| Exit | Meaning |
|---|---|
| `0` | Ready |
| `10` | **Degraded but serviceable** — not a blocker. `EnginesUnmanifested` on a fresh install is the common cause, and `verify` continues past it by design. |
| `78` | Blocked — a configuration fault. **Do not launch.** |

To run that check alone:

```bash
sudo -u modela env DENSO_DATA_DIR=/opt/denso/data /opt/denso/bin/denso --check
echo $?
```

A `[trt] Using an engine plan file across different models of devices` warning is
expected — it appears on the build host too — and does not indicate a problem.

### 13. Launch the installed application

```bash
denso-digitalreader
```

Run it as `modela`, from a graphical session. That launcher is the **one entry
point** — the menu entry, the XDG autostart entry and an operator shell all go
through it, so the environment is identical however the app starts. It sets
`DENSO_DATA_DIR=/opt/denso/data` and executes `/opt/denso/bin/denso`.

Do not invoke `/opt/denso/bin/denso` directly for normal use: without
`DENSO_DATA_DIR` it would look for its data beside the binary, in the root-owned
directory dpkg replaces on every upgrade.

Only one instance may run at a time; a second exits `3`. After launch, check the
top bar reads the mode you expect and that Backend status is what you configured.

### 14. Diagnose and roll back

**Where to look first**

```bash
cat /opt/denso/data/status.json          # machine-readable health
tail -n 200 /opt/denso/data/denso.log    # rotated siblings: .1 … .4
sudo denso-setup verify                  # re-runs every gate and says which failed
dpkg -l denso-digitalreader              # which version is actually installed
```

**The upgrade was refused**

`prerm` reports the app is running, or that it cannot tell. Close the app and
retry; for exit `4`, see step 5. A refused upgrade is safe — the **old** package is
still installed and configured.

**`verify` fails its runtime check right after an upgrade**

The most likely cause is a skipped step 10: the live database is still on the old
schema. Confirm with step 11, run step 10, then re-run `verify`.

**The migration itself failed**

`check-migrations: migration chain FAILED` leaves the live database mid-upgrade.
Restore step 6's backup (below) before doing anything else, then capture
`/opt/denso/data/denso.log` — the failure is a defect, not an operator error.

**Restore the database backup**

Restore the **whole set** (the `-wal` matters), as `modela`, with the app closed:

```bash
# 1. List the candidates, newest first, and choose one.
ls -dt /opt/denso/data/pre-upgrade-*/ /opt/denso/data/backup-*/ 2>/dev/null

# 2. Set BK to the directory you chose. Paste the REAL path — an angle-bracket
#    placeholder here would be read by the shell as a redirection, not as text.
BK=/opt/denso/data/pre-upgrade-20260803-101500-Ab3xYz

# 3. Restore, fail-fast, in a separate shell process (see the backup step for
#    why a subshell would NOT abort).
sudo -u modela bash -c '
    set -eu
    dst=/opt/denso/data
    src=$1
    [ -d "$src" ]
    [ -f "$src/denso.db" ]
    # Clear the CURRENT WAL set FIRST. Leaving a live denso.db-wal beside a
    # restored older denso.db pairs the database with a newer, unrelated
    # write-ahead log — the same mismatched pair the backup procedure exists to
    # avoid, and SQLite will happily replay it over your restore.
    rm -f "$dst/denso.db-wal" "$dst/denso.db-shm"
    cp -p "$src/denso.db" "$dst/denso.db"
    if [ -f "$src/denso.db-wal" ]; then
        cp -p "$src/denso.db-wal" "$dst/denso.db-wal"
    fi
' _ "$BK" && echo "restored from $BK"
```

No output from the final `echo` means the restore aborted part-way — re-read the
error, and do **not** launch until step 11 reports a sound database.

`set -eu` matters as much here as in the backup: a failed WAL removal or a failed
main-database copy must stop the sequence, not be followed by commands that pair
unrelated database and WAL files — and for the same reason it runs in its own
shell process rather than a subshell. The `-shm` is transient and SQLite rebuilds it;
it is removed with the WAL rather than left to pair with a different one.

Then re-check with step 11. Restoring an older database under a **newer** binary
means the migration is still pending — so if you are rolling the database back,
roll the package back too, or re-run step 10 deliberately.

**Roll the package back**

Reinstall the previous `.deb`. Because versions sort by `r<count>`, going back is
a downgrade as far as apt is concerned:

```bash
sudo apt install --no-install-recommends --allow-downgrades \
     ./dist/denso-digitalreader_<older-version>_arm64.deb
sudo denso-setup verify
```

Keep the previous `.deb` and its bundle — that is what makes this a one-command
recovery.

**A model looks wrong**

```bash
sudo denso-setup replace-model <stem>    # restore the packaged engine + sidecar
```

`configure` deliberately keeps an operator's differing model rather than
overwriting it, so this is the explicit way to take the packaged one back.

**Removing the package**

```bash
sudo apt remove denso-digitalreader      # keeps /opt/denso/data (database, engines)
sudo apt purge  denso-digitalreader      # also removes it
```

`remove` reverts autostart/autologin first and restores the recorded original GDM
settings; it refuses rather than proceeding if it cannot, so a purge can never
destroy the only record of your prior configuration.

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
