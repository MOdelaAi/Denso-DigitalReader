# Build & Deploy: `build_package.sh` → Jetson appliance install

**Date:** 2026-07-17
**Status:** Approved design, not implemented
**Authors:** Claude + Codex (adversarial debate)

## Goal

Ship Denso DigitalReader to a Jetson Orin Nano as a `.tar.gz` the operator scp's
to the device, extracts, and installs — after which the app behaves like a normal
desktop application (app-menu entry, icon, launcher on PATH), starts on power-on
unattended, and can be upgraded without losing the database or engines.

## Verified ground truth

Established by probing the dev/bench Jetson (192.168.1.15) and reading the source.
Do not re-derive these; re-verify only if the device image changes.

| Fact | Evidence |
| --- | --- |
| Target: Jetson Orin Nano, aarch64, L4T R36.5.0 (JetPack 6.2), sm_87 | `/etc/nv_tegra_release`, `uname -m` |
| Qt 6.2.4 from Ubuntu apt, **not** in the stock JetPack image (hand-installed on .15) | `dpkg -l | grep libqt6` |
| xcb platform plugin present, from `qt6-qpa-plugins` | `dpkg -L qt6-qpa-plugins` |
| OpenCV is **NVIDIA's JetPack build** in `/usr/local/lib` (`libopencv 4.8.0-1-g6371ee1`), coexisting with Ubuntu's `libopencv-*4.5d` 4.5.4 — different SONAMEs; the `/usr/local` path maps to no dpkg file entry | `pkg-config --variable=libdir opencv4`, `dpkg -S` |
| OpenCV has GStreamer support (1.20.3) | `cv2.getBuildInformation()` |
| GStreamer elements present: `rtspsrc`, `nvv4l2decoder`, `nvvidconv`, `appsink`, `h264parse` | `gst-inspect-1.0` |
| TensorRT 10.3, CUDA 12.6 from JetPack | `dpkg -l | grep libnvinfer` |
| `nmcli` local-session perms OK: `settings.modify.system=yes`, `network-control=yes` — no polkit rule needed for a desktop-launched app (would differ over SSH) | `nmcli general permissions` |
| No passwordless sudo | `sudo -n` fails |
| `systemctl get-default` = `graphical.target`; gdm3 enabled; greeter session `Type=x11` → **Xorg, not Wayland** | `loginctl show-session` |
| **No active graphical user session** — only the GDM greeter. Power-on halts at the login screen; XDG autostart never fires without autologin | `loginctl list-sessions` |
| Linux catalogs `*.engine` + `<stem>.names.json` sidecars; the `*.onnx` scan is `#ifdef _WIN32` only → **ONNX is dead weight on the Jetson** | `src/app/detection/model_sync.cpp:41-67` |
| `attached_model_filenames()` = `SELECT … FROM camera_model cm JOIN model m …` → a fresh DB requires no engines, so a clean install cannot fail warm-up | `src/core/detection/repo.cpp:48` |
| All runtime state is exe-dir-relative: `denso.db`, `denso.log`, `models/`, `models/trt_cache` | `db/db.cpp:30`, `main.cpp:98,154`, `ui/startup.cpp:111` |
| No aarch64 cross-toolchain; Windows dev box is MSYS2 UCRT64 | — |

## Decisions

### D1 — Build on the Jetson
`tools/build_package.sh` runs on an aarch64 JetPack 6.2 machine (the .15 bench
box). Cross-compiling is out of scope. The script refuses a dirty tree unless
`--allow-dirty`, which it stamps into the manifest.

### D2 — No Qt/OpenCV bundling; apt at install time
The target has wifi, so `install.sh --deps` apt-installs the Qt6 **runtime**.
Bundling was rejected: OpenCV is NVIDIA's `/usr/local` build wired to system
GStreamer, and Qt Multimedia / the xcb plugin dlopen the system
GStreamer/X11/EGL/NVIDIA graphics stack — a hand-copied `.so` set is a
dependency-closure project with the NVIDIA graphics libs as its ugliest edge.

CUDA, TensorRT, OpenCV and L4T come from JetPack and are **never** installed or
upgraded by us. Guard rail: run `apt-get -s install` first and **abort** if the
plan proposes removing or replacing any NVIDIA/CUDA/TensorRT/L4T package.
`apt-get update` is separately controllable — a stale repo must not block an
already-satisfied dependency set.

**Apt-plan parser contract** (D2a). "Package name contains `nvidia`" is neither
sufficient nor safe. The `--deps` guard must:
- run apt with `LC_ALL=C` (simulation output is locale-sensitive);
- parse `Inst`, `Remv` and upgrade records;
- **abort on any removal**, unconditionally;
- abort if the plan touches a protected family: `nvidia-l4t-*`, `cuda-*`,
  `libnvinfer*`, `libnvonnxparsers*`, `tensorrt*`;
- abort on **unexpected Ubuntu OpenCV installation** — Ubuntu's 4.5.4 must never
  become the selected runtime over NVIDIA's `/usr/local` 4.8.0.

Curated direct-runtime list (confirm exact names against the JetPack 6.2 repos):

```
libqt6core6 libqt6gui6 libqt6widgets6 libqt6network6
libqt6sql6 libqt6sql6-sqlite libqt6multimedia6 qt6-qpa-plugins
gstreamer1.0-tools gstreamer1.0-plugins-base gstreamer1.0-plugins-good
gstreamer1.0-plugins-bad gstreamer1.0-libav network-manager
```

Include `libqt6multimediawidgets6` only if linkage requires it. The list is
hand-curated and reviewed, **not** auto-derived from `ldd` — `ldd` cannot see
dlopened Qt platform/imageformat/sqldriver plugins or GStreamer registry
plugins, so deriving from it produces false confidence. `ldd` output is recorded
in the manifest as diagnostic evidence and used as an install-time *gate*.

### D3 — Install layout
```
/opt/denso/releases/<ver>+<sha>/   root:root, not writable by the app user
  bin/denso
  models/                          seed artifacts
  MANIFEST
/opt/denso/current -> releases/<ver>+<sha>    root-managed symlink
/opt/denso/data/                   modela-owned mutable state
  denso.db  denso.log*  models/  models/trt_cache/
/usr/local/bin/denso-digitalreader root-owned launcher
/usr/share/applications/com.denso.DigitalReader.desktop
/usr/share/icons/hicolor/<size>/apps/denso-digitalreader.png
```
`/opt/denso/data` chosen over `/var/lib/denso` (user decision): app + state in one
tree, trivially tar-able for support. The launcher exports `DENSO_DATA_DIR`, sets
no `LD_LIBRARY_PATH`, and execs `/opt/denso/current/bin/denso`. The launcher is
not writable by the operator.

### D4 — `denso::paths` + `DENSO_DATA_DIR` (code change, load-bearing)
One centralized API for every mutable path — database, log + rotated logs,
models, `.names.json`, trt_cache, lock file, future state. It honors
`$DENSO_DATA_DIR`, defaulting to `applicationDirPath()` so Windows dev and the
test suite are unchanged.

Without this, `denso.db` and the engines live *inside* the versioned release
directory and **every upgrade destroys the operator's database and models**.
Rejected alternatives: `chown` all of `/opt/denso` (lets the running app rewrite
its own executable); per-release symlinks into a data dir (easy to omit, and a
symlinked `denso.log` is incompatible with rename-based rotation — the logger
renames the symlink, not the target); extracting to `~/denso` (no immutable/
mutable separation).

### D5 — Single-instance guard (code change, v1-mandatory)
Autostart + a clickable menu icon guarantees two `denso` processes eventually.
Consequences: duplicate camera opens and inference load, competing network
config, SQLite write contention, and silent log loss — rename-based rotation
does **not** move another process's open file descriptor to the new pathname, so
process B keeps writing into `denso.log.1` after A rotates, and rotation later
renames/deletes inodes another process still holds.

`QLockFile` at `<data>/denso.lock`, acquired **before** the DB opens, logging
initializes, or cameras start — so the data dir must resolve very early. Second
instance: brief "already running" notice if it has a GUI session, then exit with
a documented code. Stale-lock recovery logs to stderr (the sink may not exist
yet). `--version` and installer checks must **not** take the production lock.
Raise-existing-window IPC (`QLocalServer`) is deferred — polish, not correctness.

### D6 — `--version` and `--check` (code change)
`--check` is headless and makes **no persistent mutation** (a temp probe is still
a mutation — the honest term). Three verified traps make the naive version false:

| Trap | Evidence | Consequence |
| --- | --- | --- |
| `EngineRegistry::warm_up()` creates the cache dir | `engine_registry.cpp:42` `fs::create_directories(cache_dir_, ec)` | `--check` would create `models/trt_cache` |
| `Db::open()` sets journal mode | `db.cpp:76` `PRAGMA journal_mode = WAL` | mutates the DB, creates `-wal`/`-shm` |
| `main()` constructs `QApplication` first | `main.cpp:84` | headless installer loads xcb and fails |

Therefore `--check` **must not**: construct `QApplication` (dispatch before it —
`QCoreApplication` only); initialize the rotating log sink; acquire the
production lock; call `EngineRegistry::warm_up()`; call `sync_models`; run
migrations; or open the DB via `Db::open()`.

It uses a dedicated validation path that:
- opens the DB **read-only** (Qt SQLite read-only connect option + query-only) to
  read configured models;
- constructs `TrtEngine` **directly** — verified safe: `trt_engine.cpp:87`
  `(void)cache_dir;`, so it reads the engine + sidecar and writes nothing — then
  creates an execution context and validates tensor names, modes, dtypes and
  expected I/O shape, optionally running one blank `infer()`. `trtexec
  --loadEngine` is not an acceptable substitute: it proves TensorRT can read the
  plan, not that *this app* can load, bind and execute it;
- parses `.names.json` sidecars;
- confirms every model referenced by the existing DB has a usable engine +
  sidecar;
- probes data-dir writability with a **real create-and-remove** file test as the
  target user — `access(W_OK)` is weaker and doesn't prove creation succeeds
  under the actual mount/ACL/quota/read-only conditions.

**The installer must run it as the target user, not root** — otherwise root-owned
`trt_cache`/log/lock artifacts poison an operator-owned data dir and the real app
silently fails to write:
```sh
sudo -u "$user" env HOME="$home" DENSO_DATA_DIR=/opt/denso/data \
  CUDA_CACHE_PATH="$tmp/cuda-cache" TMPDIR="$tmp/tmp" \
  /opt/denso/releases/<ver>+<sha>/bin/denso --check
```
with `$tmp` target-user-owned and removed afterwards. `--version` and `--check`
must never take the production lock.

**`--check-running` is the one deliberate exception to that rule.** Liveness
detection *requires* `QLockFile::tryLock()`, which briefly acquires and releases
the lock when no instance is running — there is no way to reuse the app's own
stale-lock semantics without touching the lock. It is therefore explicitly
exempt, and **must run as the target user**: as root it would leave a root-owned
lock artifact in an operator-owned data dir, which is the exact poisoning D6
exists to prevent.

**Missing-DB behavior (fresh install).** A first install has no `denso.db`.
`--check` must treat an absent live DB as an **empty configured-model set** and
must **not create one** (consistent with the ground truth that a fresh DB
requires no engines). Only the packaged engine is validated in that case.

Only package engines and engines referenced by current configuration are
activation blockers — an unrelated stray engine in the operator's models dir must
not block an upgrade. GUI/xcb validation is **not** a separate mode in v1: the
operator launches the app once from the menu over AnyDesk after install, which
exercises the real session. A root installer under `sudo` may lack usable display
credentials anyway, so release activation must never depend on a `DISPLAY`.

### D7 — Engines: ship prebuilt, never build at install
Ship the validated `.engine` + `.names.json`; no `trtexec` at install. Installer
builds fail on: `trtexec` absent, ONNX-parse becoming an install dependency the
app deliberately doesn't need, workspace/memory/thermal/power-mode/disk
variance across a 5–10 min build, builder-flag drift from the production recipe,
and unclear state on interrupt. Engine production stays an explicit operator
action; the package promotes a *validated artifact*.

**Model selection is explicit, never a directory glob.** An earlier draft shipped
"every `models/*.engine` with a matching sidecar"; that is accidental
directory-content selection, and a forgotten experimental engine with a valid
sidecar would reach production. Dirty-tree refusal does not help — models are
untracked (`.gitignore`: `models/*.engine`). Instead: a **tracked packaging
manifest** lists approved model stems with their expected SHA-256 and trtexec
recipe, and `build_package.sh --model models/digitv2.engine` must match it.
Hash mismatch against the approval metadata is a **hard failure**.

Preflight asserts: `aarch64`; L4T exactly R36.5.0; NVIDIA driver operational;
TensorRT 10.3; CUDA runtime resolvable; compute capability 8.7; every required
`.engine` has its sidecar; **and the engine actually deserializes on the target**
— the authoritative check. Version comparisons are diagnostics only.

### D8 — Autostart + autologin (two separate, explicit options)
`install.sh --user modela --autostart` writes
`~modela/.config/autostart/com.denso.DigitalReader.desktop`, owned by the target
user, invoking the same system launcher as the menu entry. XDG autostart is
chosen over a `systemd --user` unit: the latter frequently lacks a correct
`DISPLAY`/`XAUTHORITY`/DBus session environment and complicates polkit identity.

`--enable-autologin` is **separate and explicit** (approved by the user). The
box currently halts at the GDM greeter on power-on, so without it autostart never
fires. The installer:
- verifies the user exists with a valid home, GDM is the active display manager,
  and `systemctl get-default` is `graphical.target` (report + require an explicit
  option rather than silently changing either);
- backs up `/etc/gdm3/custom.conf` preserving permissions;
- parses the INI and sets only `[daemon] AutomaticLoginEnable=true` and
  `AutomaticLogin=<user>` — never replaces the file from a template;
- records the **original pre-Denso values ONCE**, in root-owned
  `/opt/denso/install-state/` — outside any release dir, and **never rebased on
  upgrade**. (Bug this avoids: first install records `AutomaticLoginEnable=false`
  then sets true; an upgrade that re-records the *current* value would store
  `true` as the "original", and uninstall could never restore the real prior
  state.) The state must distinguish: Denso enabled autologin / it was already
  enabled by the admin / the admin later changed the user / Denso's requested
  values are still active;
- `--disable-autologin` and uninstall restore **only those keys, and only if they
  still hold the values we set** (a blind full-file restore would erase later
  admin changes);
- warns that autologin grants anyone with physical access that desktop session.

`--user` is explicit and required; `$SUDO_USER` is not trusted (absent in
automation, wrong from a root shell).

**Startup timing:** no delay is required for correctness. `CameraStream` already
reconnects and network reassert is deferred to the first event-loop tick, so the
app must tolerate no-network and no-cameras indefinitely. An optional small
`X-GNOME-Autostart-Delay` (3–5 s) is allowed, but nothing may depend on it. The
app must **not** be restart-looped: warm-up failure exits deliberately, and a
supervisor would turn that into a crash storm.

**Session type:** the *greeter* runs `Type=x11`. That does **not** prove the
logged-in user session is Xorg — GDM's greeter and the GNOME user session can
differ in type. The honest statement: greeter is X11; **the user session type is
unknown until autologin is exercised**. Do not force `QT_QPA_PLATFORM=xcb` in the
launcher until the power-on autologin session has actually been observed.

### D9 — Transactional install/upgrade
Ordering is explicit, because dependency install has to sit between the two
preflights: a host preflight *before* `--deps` would report expected Qt failures,
while installing deps *after* staging can abandon a staged release.

1. Verify every payload file against the **internal manifest** (see the checksum
   contract below). Validate `<ver>+<sha>` against a strict character allowlist
   before it is ever used as a path component.
2. Inspect/simulate the dependency plan (D2a). Define behavior when `apt-get
   update` is skipped and the package indexes are insufficient: report and stop,
   never partially install.
3. Install dependencies **only if `--deps` was explicitly requested**.
4. Final host/runtime preflight (D7).
5. **Refuse if the app is running** — never kill it. Detection is
   `denso --check-running`, which reuses the app's own `QLockFile` semantics; the
   installer must **not** reimplement Qt's stale-lock rules in shell, and must
   never delete or "repair" the lock. Its only job is to refuse an active
   instance. A `current` swap under a live old process leaves it running
   indefinitely with ambiguous model/DB state.
6. Extract into a staging dir **on the same filesystem** as
   `/opt/denso/releases`, verify, then `rename` into place.
7. `ldd bin/denso | grep "not found"` must be empty.
8. Seed models into `<data>/models` — absent → seed; present with same hash →
   leave; **present with different hash → do not overwrite silently** (require
   `--replace-model <stem>`).
9. Back up `denso.db`.
10. **Migration smoke test on a throwaway copy** (see below).
11. `denso --check` gate, run as the target user (D6).
12. Atomically swap `current` (rename a new symlink; never `rm` + `ln`).
13. Leave the previous release intact.

**Engine + sidecar installation is ordered and crash-resistant, NOT atomic.** Two
flat files cannot be made atomic with two renames — power can fail between them.
The engine's appearance is the commit marker: write+fsync the sidecar temp file →
rename sidecar into place → write+fsync the engine temp file → rename the engine
into place **last** → fsync the directory. This guarantees a newly-appearing
engine always has its sidecar. Replacement semantics remain weaker than that;
document the recovery behavior rather than claiming pair atomicity. (Versioned
immutable model filenames would solve this cleanly but are cut — see YAGNI.)

**Migration gate (reinstated).** An earlier draft cut this as YAGNI; that was
wrong. Migrations run forward-only at first GUI launch, *after* activation — so a
failing migration produces an activated release that dies at **every autologin**,
on an appliance whose whole point is unattended power-on. The check is cheap
next to diagnosing that in a factory.

It needs its own executable path: `--check` is forbidden from running migrations
(D6) and normal startup constructs the GUI and continues into the app, so neither
can serve. Add a fourth headless mode — **`--check-migrations <db-path>`** — that
dispatches before `QApplication`, runs *only* the migration chain against the
path it is given, and exits with the result. The installer copies `denso.db` into
a throwaway target-user-owned dir and points the mode at the copy, so mutation is
confined there and D6's contract for `--check` is untouched.

**Fresh install:** with no existing `denso.db` there is nothing to copy. The
smoke test then runs against an **empty temporary DB**, which exercises the full
migration chain from v0 — the same chain the app will run on first launch.

**Rollback is attended, not automatic.** Migrations are forward-only (schema at
v11); flipping `current` back does not un-migrate the database. The DB backup is
the remedy, and this must be documented as such rather than advertised as
rollback.

**Checksum contract** — three distinct things, never conflated:
- the **operator** verifies `<archive>.sha256` *before* extraction;
- the **installer** verifies each payload file against the internal manifest (it
  cannot verify the hash of the archive it was extracted from);
- **neither authenticates the publisher.** Not supply-chain verification.

### D10 — Uninstall
Removes integration + releases; **keeps** `/opt/denso/data` (db, models, logs,
backups) unless `--purge-data`, which warns. Disables only the autostart/
autologin settings this package installed (per D8's key-level restore).

Uninstall **must refuse while an instance is running** (same `--check-running`
gate as D9) — removing `current`, the launcher or the models under a live process
creates the same ambiguity as upgrading. `--purge-data` requires an exact
**resolved-path guard** before deleting `/opt/denso/data` (resolve symlinks,
assert the canonical path, refuse anything else).

### D11 — MANIFEST
Source SHA + dirty flag, build date, compiler, CMake options, L4T/TensorRT/CUDA/
Qt/OpenCV (path + version) of the **build box**, executable SHA-256, per-engine
`{filename, sha256, TRT version, compute capability, exact trtexec recipe}`,
sidecar hashes, curated dep list, and the `ldd`/ownership report classifying each
resolved library as: application artifact / Debian-owned / `/usr/local` NVIDIA /
unknown. **Unknown paths are a review failure**, not an automatic dependency.

Archive ships with a `.sha256`. This catches transfer corruption only — it is not
authentication, and must not be described as supply-chain verification. Signing
is deferred until packages cross an untrusted channel.

## Components

| Unit | Purpose | Depends on |
| --- | --- | --- |
| `tools/build_package.sh` | Jetson-side: clean Release build → stage → tar.gz + sha256 + MANIFEST | git, cmake, the build box's JetPack stack |
| `packaging/install.sh` | preflight → deps → stage → validate → seed → backup → atomic switch → desktop/autostart/autologin | `denso --check`, apt, gdm |
| `packaging/uninstall.sh` | reverse D3/D8 integration; keep data by default | install metadata |
| `packaging/denso-digitalreader` (launcher) | export `DENSO_DATA_DIR`, exec `current/bin/denso` | — |
| `packaging/com.denso.DigitalReader.desktop` | menu entry + autostart template | launcher, icon |
| `src/core/paths/` (`denso::paths`) | single source of truth for mutable paths | `$DENSO_DATA_DIR` |
| single-instance guard | `QLockFile` before DB/log/cameras | `denso::paths` |
| `--version` / `--check` / `--check-running` / `--check-migrations <db>` | headless install gates, all dispatched before `QApplication`. `--check`: no persistent mutation. `--check-running`: takes the lock by design (sole exemption). `--check-migrations`: mutates only the throwaway copy it is given | `TrtEngine`, sidecar reader, read-only `detection::repo`, `db::run_migrations` |
| `packaging/lib/*.sh` | pure installer policies (version allowlist, apt-plan guard, seeding, GDM INI) | — |

## Testing

**Where each policy authoritatively lives** — installer policies are shell, so
Catch2 cannot reach them; implementing them twice (C++ + shell) to make them
testable would be worse than not testing them. Single implementation each:

| Policy | Lives in | Tested by |
| --- | --- | --- |
| `denso::paths` resolution, lock, `--check` logic | C++ | Catch2 |
| version-string allowlist, apt-plan parse, seeding decision, GDM INI edit/restore | `packaging/lib/*.sh` (pure functions, sourced) | `tests/packaging/` shell harness (assert-based, no new deps) |

- **Unit (Catch2, off-device):** `denso::paths` resolution (env set / unset /
  empty / relative); `--version`/`--check` take no lock and don't construct
  `QApplication`; `--check` performs no persistent mutation (no `trt_cache`, no
  `-wal`/`-shm`, no log, no lock) **and treats a missing DB as an empty
  configured-model set without creating one**; `--check-migrations` migrates the
  copy it is given and leaves the live DB untouched, incl. the empty-temp-DB
  (fresh install) case; `--check-running` reports true/false correctly and
  releases the lock when no instance holds it; second-instance rejection.
- **Unit (shell harness, off-device):** version-string allowlist; seeding
  decision table (absent / same-hash / different-hash); autologin INI edit +
  key-level restore incl. "admin changed it since" → refuse, and the
  never-rebase-on-upgrade rule; dep-plan parser rejecting an `apt -s` plan that
  removes anything or touches a protected family.
- **On-device (Jetson .15):** `--check` against a real engine; upgrade over an
  existing data dir preserving db + models; refuse-while-running; power-cycle →
  autologin → autostart → GUI up → network + camera recovery.
- **Supplemental container diagnostic (NOT a gate):** see below.
- **v1 acceptance test:** the first install onto a real target device. There is
  no substitute.
- **Not covered off-device:** anything needing the NVIDIA graphics/TensorRT
  runtime or a real GDM session.

### Container diagnostic — what it does and does not prove

Docker 29.3 + nvidia-container-runtime + CSV mounts are available on the Jetson
(164 GB free), so a container run is *cheap supplemental evidence*. It is **not**
the clean-image dependency gate, and must not be described as one: the package
contract includes the host OS, display manager, NVIDIA multimedia stack and
desktop session — not just ELF loading.

`l4t-base:r36.2` is actively misleading here: it has no in-image CUDA/TensorRT/
NVIDIA OpenCV (CSV injection supplies host/device integration, not the JetPack
SDK filesystem), so those would read as "missing" and we would wrongly add them
to the apt list — the exact opposite of the goal. Closest usable image is
`nvcr.io/nvidia/l4t-jetpack:r36.4.0`, **pinned by digest, not tag**, with the
image's pre-test package inventory captured so the curated list gets no credit
for packages already present. Note it is R36.4 while the target contract is
exactly R36.5.0.

Proves: the packaged exe loads in that container; the curated packages are
installable from the configured repos; headless TensorRT/CUDA init works; engine
validation succeeds; no obvious direct ELF dependency is absent there.
Does **not** prove: the dep list is sufficient on a clean JetPack 6.2 device;
NVIDIA OpenCV/GStreamer integration matches the target rootfs; xcb works; NVDEC
pipelines work; GDM autologin/autostart works; apt leaves the real target's
NVIDIA packages untouched.

## Explicitly cut (YAGNI)

Bundled Qt/OpenCV · ONNX in the package (~73 MB dead weight) · install-time
trtexec · trt_cache packaging · `systemd --user` · crash supervision ·
raise-existing-window IPC · auto-derived deps from `ldd` · package signing ·
versioned model artifact filenames · baseline seeded DB (a fresh DB requires no
engines — see ground truth) · automatic rollback · JetPack versions beyond the
one tested · a separate `--check-gui` mode (the operator launches once from the
menu over AnyDesk, which exercises the real session; a root installer under
`sudo` may lack display credentials anyway, so activation must never depend on
`DISPLAY`).

**Reinstated after being cut:** the DB-copy migration smoke test (D9) — cutting
it was a bad call, see D9's rationale.

## Implementation slicing

One design, **two independently mergeable slices**, in this order — every task is
verifiable when it lands, and packaging cannot be tested before the app modes it
calls exist.

**Slice 1 — application deployment primitives** (off-device verifiable, Catch2 on
the Windows box; independently valuable and reviewable):
`denso::paths` → move every mutable path onto it (db, log, models, cache, legacy
settings.json, lock) → unit-test env handling → refactor arg parsing so
`--version`/`--check`/`--check-running`/`--check-migrations` dispatch **before**
`QApplication` → read-only DB inspection path (missing DB = empty set, never
created) → direct engine/sidecar validation bypassing `EngineRegistry::warm_up()`
→ `--check-migrations` against a given path → early `QLockFile` guard on normal
GUI startup only → tests → verify `--check` against a real engine on the Jetson.

**Slice 2 — packaging + host integration** (Jetson-verifiable only):
launcher + desktop + icon + autostart template → pure testable shell helpers
(version allowlist, apt-plan guard, seeding policy, GDM INI edit/restore) →
`install.sh` → `uninstall.sh` → `build_package.sh` + manifest → container
diagnostic → bench install → upgrade preservation + refuse-while-running →
power-cycle → autologin → autostart → GUI → network/camera recovery.

## Known risks

1. **Dep list rot** — the container diagnostic is not a substitute; the honest
   mitigation is the first install on a real target, plus review of the curated
   list. Accepted for v1 and documented rather than papered over.
2. **User session type unknown** (Xorg vs Wayland) — the greeter is X11 but that
   doesn't determine the user session. Observe the real power-on autologin
   session before pinning `QT_QPA_PLATFORM`.
3. **Upgrade while running** — mitigated by `--check-running`; the operator must
   close the app.
4. **Forward-only migrations** make downgrade unsafe; DB backup is the remedy,
   and the D9 migration smoke test keeps a bad migration from being activated.
5. **`/opt/denso` mixed ownership** (root `releases/` + operator `data/`) is
   deliberate; the D6 run-as-target-user rule is what keeps root artifacts out of
   `data/`.
