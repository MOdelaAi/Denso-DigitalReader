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
SHA-256, and `tools/build_package.sh` refuses anything not listed — whichever
selector you use, `--models-dir` or `--model`.

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
denso --check-migrations <db-path>   # APPLIES the chain to a THROWAWAY COPY.
                                     # Writes — see the warning below.
denso --apply-migrations             # Migrates the LIVE database in place.
                                     # Takes no path, on purpose.
                                     #   0  at the supported schema
                                     #   78 Blocked (unreadable / newer / failed)
```

`--check` exit **10** is a non-blocking condition, not a failure — `denso-setup`
interprets these codes.

There are two migration commands, and the split is deliberate:

| | target | who runs it |
|---|---|---|
| `--check-migrations <path>` | a throwaway **copy** you supply | `denso-setup verify`, to prove the chain *would* apply |
| `--apply-migrations` | the **live** database under `DENSO_DATA_DIR` | the package's `postinst`, to actually apply it |

> **`--check-migrations` is not read-only.** Despite the name it opens the
> database you name and migrates it, creating tables and converting legacy rows.
> It also creates the file if it does not exist, so a mistyped path yields a new
> empty database and still reports `ok`. **Point it only at a copy.** Aiming it
> at `/opt/denso/data/denso.db` migrates the live database with no backup taken
> and no schema-newer guard — which is what `--apply-migrations` exists to
> prevent.

> **`--apply-migrations` refuses to take a path.** It always migrates the primary
> database under `DENSO_DATA_DIR`, so it cannot be aimed somewhere else by a typo
> in a maintainer script, and it cannot conjure an empty database at a mistyped
> path. It runs the read-only schema classifier *first*, so a database written by
> a **newer** build is refused with **78** before the chain opens it for writing —
> the forward-only guard. It never rolls back: recovery is manual and explicit.

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

## Fresh install — one command

```bash
sudo apt install ./denso-digitalreader_<version>_arm64.deb
```

That is the whole procedure. There is **no** `denso-setup configure` step
afterwards. The install:

1. **Resolves the operator user** — the existing recorded user if there is one,
   otherwise `SUDO_USER`, otherwise exactly one acceptable local, active,
   non-remote session user. root, `nobody`, system/service accounts and unknown
   users are refused, and if the answer is ambiguous the **install fails rather
   than guessing**. Any normal local username works; nothing is hardcoded.
2. Creates and initialises `/opt/denso/data` and hands it to that user.
3. Leaves `/opt/denso/{bin,lib,models}` root-owned and package-owned.
4. **Enables autostart** for that user — the systemd user service
   `denso-digitalreader.service`.
5. **Does not touch autologin.** The box still stops at the greeter on power-on.
6. Verifies the setup and **fails the installation** if it is incomplete.

Expected output:

```
denso: fresh installation configured.
denso: operator user: <resolved-user>
denso: autostart: enabled
denso: autologin: unchanged
```

Denso then starts at that operator's next graphical login.

### Running it, and watching it

**systemd `--user` is the only thing that starts Denso.** There is no XDG
autostart entry, so `systemctl --user disable` genuinely stops it starting:

```
graphical login → systemd --user → denso-digitalreader.service
                                     → /usr/bin/denso-digitalreader
                                     → /opt/denso/bin/denso

desktop / menu click → systemctl --user start denso-digitalreader.service
                       (the SAME unit — a no-op if it is already running)
```

Lifecycle, as the operator (not root):

```bash
systemctl --user status  denso-digitalreader
systemctl --user start   denso-digitalreader
systemctl --user stop    denso-digitalreader
systemctl --user restart denso-digitalreader

systemctl --user enable  denso-digitalreader   # start at every graphical login
systemctl --user disable denso-digitalreader   # stop starting automatically
systemctl --user enable  --now denso-digitalreader
systemctl --user disable --now denso-digitalreader
```

`enable`/`disable` are authoritative — nothing else can start Denso at login, so
a disabled service stays disabled. A `.deb` upgrade will **not** re-enable it.

Live and recent output:

```bash
sudo journalctl _SYSTEMD_USER_UNIT=denso-digitalreader.service -f
sudo journalctl _SYSTEMD_USER_UNIT=denso-digitalreader.service -n 100
```

> This appliance runs a **volatile** journal with no per-user journal files, so
> those are the supported commands. `journalctl --user -u denso-digitalreader -f`
> also works *if* a machine has been configured with persistent per-user
> journals — the Denso package neither requires nor configures that.

**Service status is not application health.** They answer different questions:

| Question | Command |
|---|---|
| Is the service running? | `systemctl --user status denso-digitalreader` |
| Is an instance holding the lock? | `denso-digitalreader --check-running` — 0 running, 1 not, 4 cannot tell |
| Is the app, database and model set healthy? | `denso-digitalreader --check` — 0 Ready, 10 Degraded, 78 Blocked |

If the service will not start and the journal says *"no graphical session is
available"*, that is the guard doing its job: Denso is a GUI and there is no
display for it. Log in graphically. **No `DISPLAY` value is ever invented** — a
guessed one either does not exist, or belongs to the greeter or another user.

## Manual upgrade — the short version

Denso is an **embedded appliance updated manually by an administrator**. There is
**no automatic updater**. To upgrade an appliance that already has Denso
installed:

```bash
# 1. Stop Denso.

# 2. Confirm it really stopped — TRI-STATE, only rc 1 is safe to continue:
denso-digitalreader --check-running     # 1 = stopped  (0 = running, 4 = cannot tell)

# 3. Run the release preflight guard against the .deb it was generated for:
sudo ./preflight-denso-<version>.sh ./denso-digitalreader_<version>_arm64.deb

# 4. Install — this performs the upgrade in place:
sudo apt install ./denso-digitalreader_<version>_arm64.deb

# 5. The installer automatically takes a verified pre-migration backup and runs
#    the forward migration if the schema needs one. Nothing to do by hand.

# 6. Verify:
denso-digitalreader --check             # 0 Ready, 10 Degraded-serviceable

# 7. Start Denso through the existing launch mechanism (the menu entry, or
#    /usr/bin/denso-digitalreader).
```

What to know before you run it:

- **No uninstall is needed.** Do not `apt remove` the old package first — upgrade
  in place.
- **Your database is preserved.** dpkg never touches `/opt/denso/data`.
- **The migration backup is automatic** whenever a schema migration is required:
  `/opt/denso/data/denso.db.pre-v<schema>`, created *and verified* before
  anything writes to the live database.
- **If migration or integrity verification fails, package configuration fails and
  Denso stays stopped.** That is deliberate. The message names the backup path.
  Fix the cause, then `sudo dpkg --configure -a` — it reuses the same backup
  rather than overwriting it.
- **Recovery is manual.** Nothing is ever restored or rolled back automatically,
  and no backup is ever deleted automatically.
- **Never `apt purge`** if `/opt/denso/data` must be retained — purge deletes the
  database, the operator's engines and every `denso.db.pre-v*` backup.
- **Use `apt install`, never a bare `dpkg -i`** — `dpkg -i` does not resolve
  dependencies.

The rest of this section covers building the package and the full first-install
procedure.

> **Validated end to end** on the development/release-test Jetson (192.168.1.15,
> 2026-08-07) with package `0.1.0+r443.gf8520bf` from source
> `f8520bf7f6af949a148c925c0a217f2774c19421`: `apt install` upgrade from
> `0.1.0+r437.gf6ab501` → automatic pre-migration backup → schema **v15 → v16** →
> backup integrity ok → production integrity ok → `denso --check` **READY / rc 0**
> → application launched successfully on the migrated database. Also verified: the
> complete three-model / six-file payload, both modes' compatibility at runtime,
> and a real-root + real-`runuser` gate rehearsal on synthetic data (35/35).
> This is validation history — always install the version you actually have, not
> this one.

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

`<operator>` is one of those placeholders: it is the account this appliance
recorded at install time, readable with `cat /opt/denso/install-state/user`.
It is whatever normal local user the installer resolved — the product hardcodes
no particular username, and neither should any procedure here.

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
# THE RELEASE CUT — the complete canonical set, both product modes
tools/build_package.sh --models-dir models
```

`--models-dir` is the canonical release input. It requires **exactly** the six
canonical files and refuses anything else:

| Model | Sidecar | Family | Mode |
|---|---|---|---|
| `digitv3.engine` | `digitv3.names.json` | `digit_numeric` | Digital Number Reader |
| `float-small.engine` | `float-small.names.json` | `float_ball` | Floating Ball Leveler |
| `float-big.engine` | `float-big.names.json` | `float_ball` | Floating Ball Leveler |

It refuses a missing engine, a missing sidecar, an unexpected fourth engine, and
any `.pt` or `.onnx` in the directory (production packaging is TensorRT-engine
only). It hands the engines downstream in reviewed manifest order — the order is
part of the manifest bytes, and therefore part of the pinned Release-B identity.

**Why it exists:** `--model` builds whatever set it is handed, so a partial
release is the easy mistake. A package built from `--model models/digitv3.engine`
alone is valid and installable and **silently ships without the entire Floating
Ball Leveler mode**. `--models-dir` makes that unrepresentable.

`--model` remains for one-off and diagnostic builds, and is mutually exclusive
with `--models-dir` (an extra `--model` could only widen a canonical set):

```bash
# NOT a release cut — a single-mode package, for diagnosis only
tools/build_package.sh --model models/digitv3.engine --allow-dirty
```

Run it from the repository root. Model paths are resolved relative to the repo
root regardless of where you invoke it from.

Containing all three models does not let either mode load the wrong one: the
`canonical_id -> family -> allowed modes` registry in
`src/core/models/compatibility.cpp` is the only thing that grants authority, no
family is permitted in both modes (a `static_assert` enforces it), and the
manifest never states privileges — so an operator edit in the models directory
cannot widen what a model may do.

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
sudo -u <operator> env DENSO_DATA_DIR=/opt/denso/data /opt/denso/bin/denso --check-running
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
sudo chown -R <operator>:<operator> /opt/denso/data
```

The package enforces this too: `prerm` **refuses** an upgrade or removal while an
instance is running, and reads `--check-running` as the tri-state it is — `4` is
treated as unsafe, never as "probably fine". A refused upgrade leaves the **old**
package installed, which is the correct outcome.

### 6. Create a verified pre-upgrade database backup

**The package now takes one for you.** `postinst` creates and verifies
`/opt/denso/data/denso.db.pre-v<schema>` before it migrates anything (step 10),
and halts the upgrade if that backup cannot be made or cannot be verified. So
this step is no longer the only thing standing between you and a failed
migration.

Take one anyway if you want a copy **outside** `/opt/denso/data` — on other media,
or before a risky release. The gate's backup lives in the data dir, which is no
help if the concern is the filesystem itself. `denso-setup verify` also creates
one, but it runs *after* the migration, so it is not the backup that protects you
from the migration.

Create the directory with `mktemp -d`, and capture the path from the command that
created it. Do not name a directory by timestamp and then rediscover it with `ls`:
second-resolution names collide, and picking up the wrong directory pairs a
`denso.db` with a stranger's `-wal`, which is worse than no backup because it
looks restorable. (`denso-setup verify` uses `mktemp -d` for the same reason.)

Copy the **WAL set** (`denso.db` **and** `denso.db-wal`), **as the target user** —
a root-owned file left in an operator-owned data dir is one the app can no longer
write:

```bash
BK=$(sudo -u <operator> mktemp -d "/opt/denso/data/pre-upgrade-$(date +%Y%m%d-%H%M%S)-XXXXXX")
echo "backup dir: $BK"

sudo -u <operator> bash -c '
    set -eu
    src=/opt/denso/data
    dst=$1
    [ -d "$dst" ]
    cp -p "$src/denso.db" "$dst/denso.db"
    # Test for the WAL, then copy it UNGUARDED — never `2>/dev/null || true`.
    if [ -f "$src/denso.db-wal" ]; then
        cp -p "$src/denso.db-wal" "$dst/denso.db-wal"
    fi
' _ "$BK" && sudo -u <operator> python3 - "$BK/denso.db" <<'PY'
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

The check runs through `sudo -u <operator>` too: `mktemp -d` creates the directory
mode 0700 owned by `<operator>`, so reading it as anyone else fails on permissions.
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

That is also why the package ships **`/opt/denso/lib/denso-db-helper`**, a small
Python 3 tool the upgrade gate uses instead of the `sqlite3` CLI. Adding the CLI
to `Depends` would put an apt fetch in the middle of an offline `.deb` upgrade,
for a database the standard library already speaks. It is usable by hand:

```bash
sudo -u <operator> python3 /opt/denso/lib/denso-db-helper user-version    /opt/denso/data/denso.db
sudo -u <operator> python3 /opt/denso/lib/denso-db-helper integrity-check /opt/denso/data/denso.db
sudo -u <operator> python3 /opt/denso/lib/denso-db-helper backup          /opt/denso/data/denso.db /path/to/snapshot.db
```

`backup` uses SQLite's **online backup API** (`Connection.backup()`), which is
consistent under WAL — it is not a copy of `denso.db`/`-wal`/`-shm`, and it
verifies the snapshot's own `integrity_check` and `user_version` before
reporting success. It refuses a destination that already exists, and it opens the
source with `mode=rw` rather than `rwc`, so a **mistyped source path is an error
instead of a brand-new empty database**. Run it as the data-dir owner, never as
root.

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
schema**, and why the package's `postinst` runs the upgrade gate described in
step 10 before handing the appliance back. If that gate fails, this install
command fails with it and the application stays stopped.

### 8. Configure the installation

`postinst` is structural only (it creates directories); everything requiring
judgement is an explicit operator step:

```bash
sudo denso-setup configure --user <operator>
```

`--user` is **required** and is not inferred from `$SUDO_USER` (absent under
automation, wrong from a root shell). It records the target user, sets data-dir
ownership, and seeds the packaged models into `/opt/denso/data/models` **as that
user**. A model already present and identical is left alone; one that differs is
**kept**, not overwritten.

Autostart is **opt-in and not verified on hardware**:

```bash
sudo denso-setup configure --user <operator> --autostart --enable-autologin
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

### 10. Migrations — applied automatically by `postinst`

**There is normally nothing to do here.** Step 7's install runs the package's
`postinst`, which performs the whole forward-only upgrade gate before you are
given the appliance back:

1. Reads the live schema version.
2. Takes **one** pre-migration backup at
   `/opt/denso/data/denso.db.pre-v<schema>`, and proves it with
   `PRAGMA integrity_check` before anything writes to the live database.
3. Runs `denso --apply-migrations` against the live database, as the target
   user.
4. Runs `denso --check`.

Every database operation runs as the **target user**, never as root — a
root-created WAL or journal file in an operator-owned data dir is one the app can
no longer write.

**If any of those fail, the upgrade halts:** `postinst` exits non-zero, dpkg
leaves the package unconfigured, and the application stays stopped. Nothing is
rolled back and no backup is deleted — recovery is manual and explicit. The
failure message names the backup path. Once you have fixed the cause:

```bash
sudo dpkg --configure -a
```

That re-runs the gate. It will **not** take a second backup: the backup is named
for the schema version being left, so a retry finds the existing one and keeps
it. Overwriting it would replace your only recovery point with a copy of the
half-migrated database.

`--check` exit **10** (Degraded) does **not** halt the upgrade — a per-camera
fault such as a rejected model attachment must not brick a multi-camera
appliance. Exit **78** (Blocked) does halt it.

A database written by a **newer** build than the one you just installed is
refused with 78 before the chain opens it for writing. Downgrades are not
supported; that is what "forward-only" means here.

**Backups accumulate on purpose.** One file per schema version the appliance has
passed through, never pruned automatically. Deleting them is an explicit
operator decision.

#### Running the migration by hand

Only needed if you are recovering a half-configured appliance. As the target
user, never as root:

```bash
sudo -u <operator> env DENSO_DATA_DIR=/opt/denso/data \
    /opt/denso/bin/denso --apply-migrations
```

Expect `apply-migrations: ok db=/opt/denso/data/denso.db from=14 to=15`. It takes
no path argument — see the two-command table in *Command-line modes* for why, and
do **not** substitute `--check-migrations`, which has no backup, no schema-newer
guard, and will silently create an empty database at a mistyped path.

A normal GUI launch will also open and migrate its configured database. **The
deployment workflow does not rely on that.** The gate migrates and verifies at a
controlled moment with a verified backup beside it — not on an operator's first
launch.

### 11. Verify the live schema and integrity

Confirm the live database actually moved, and is sound:

```bash
sudo -u <operator> python3 - /opt/denso/data/denso.db <<'PY'
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
sudo -u <operator> env DENSO_DATA_DIR=/opt/denso/data /opt/denso/bin/denso --check
echo $?
```

A `[trt] Using an engine plan file across different models of devices` warning is
expected — it appears on the build host too — and does not indicate a problem.

### 13. Launch the installed application

```bash
denso-digitalreader
```

Run it as `<operator>`, from a graphical session. That launcher is the **one entry
point** — the menu entry, the systemd user service and an operator shell all go
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

The most likely cause is that the live database is still on the old schema —
which now means the upgrade gate never completed. Check whether the package is
configured (`dpkg -l denso-digitalreader`; state `iU` means unconfigured),
confirm the schema with step 11, then run `sudo dpkg --configure -a` and re-run
`verify`.

**The migration itself failed**

`apply-migrations: BLOCKED: migration chain FAILED` leaves the live database
mid-upgrade, and `postinst` halts the upgrade rather than reporting success. The
gate's own message names the pre-migration backup — it is at
`/opt/denso/data/denso.db.pre-v<schema>` and was **not** restored automatically.
Restore it (below) before doing anything else, then capture
`/opt/denso/data/denso.log` — the failure is a defect, not an operator error.

**`apply-migrations: BLOCKED` naming a newer schema**

The live database was written by a newer build than the one you installed. This
is a downgrade, which is not supported. Install the newer package again; the
database is untouched, because the guard runs before the chain opens it for
writing.

**Restore the database backup**

Restore the **whole set** (the `-wal` matters), as `<operator>`, with the app closed:

```bash
# 1. List the candidates, newest first, and choose one.
ls -dt /opt/denso/data/pre-upgrade-*/ /opt/denso/data/backup-*/ 2>/dev/null

# 2. Set BK to the directory you chose. Paste the REAL path — an angle-bracket
#    placeholder here would be read by the shell as a redirection, not as text.
BK=/opt/denso/data/pre-upgrade-20260803-101500-Ab3xYz

# 3. Restore, fail-fast, in a separate shell process (see the backup step for
#    why a subshell would NOT abort).
sudo -u <operator> bash -c '
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

> **Never `purge` when the operator data must be preserved.**
> `purge` runs `rm -rf /opt/denso/data`, which deletes the database, the
> operator's engines, **and every `denso.db.pre-v*` pre-migration backup the
> upgrade gate has ever taken**. Those backups live in the data directory, so a
> purge destroys the recovery points along with the thing they exist to recover.
> There is no undo and nothing is archived elsewhere.
>
> Use `apt remove` to replace or reinstall the package: it keeps
> `/opt/denso/data` untouched, and an upgrade never purges. If you genuinely
> intend to discard operator data, copy the database and any needed
> `denso.db.pre-v*` file somewhere outside `/opt/denso` **first**.

`remove` reverts autostart/autologin first and restores the recorded original GDM
settings; it refuses rather than proceeding if it cannot, so a purge can never
destroy the only record of your prior configuration.

`postrm` does nothing at all on `remove`, `upgrade`, `failed-upgrade`,
`abort-*` or `disappear` — the deletion is reachable only through `purge`, and
even then only after a `readlink -f` guard confirms `/opt/denso/data` resolves to
exactly itself (a symlinked data dir must never turn a purge into `rm -rf` of
somewhere else).

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
