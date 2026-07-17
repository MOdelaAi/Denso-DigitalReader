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
`--check` is headless and non-mutating; it must run before the GUI is created:
- engines deserialize **through the real `TrtEngine` path** (create an execution
  context; validate tensor names, modes, dtypes, expected I/O shape) — not just
  `trtexec --loadEngine`, which proves TensorRT can read the plan but not that
  *this app* can load, bind, and execute it;
- `.names.json` sidecars parse;
- every model referenced by the existing DB has a usable engine + sidecar;
- data dir writable as the target user.

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

Model selection in `build_package.sh` is mechanical: every `models/*.engine` with
a matching `.names.json` (this naturally excludes `digitv2.engine.bak`).

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
- records prior values in install metadata; `--disable-autologin` and uninstall
  restore **only those keys, and only if they still hold the values we set**
  (a blind full-file restore would erase later admin changes);
- warns that autologin grants anyone with physical access that desktop session.

`--user` is explicit and required; `$SUDO_USER` is not trusted (absent in
automation, wrong from a root shell).

**Startup timing:** no delay is required for correctness. `CameraStream` already
reconnects and network reassert is deferred to the first event-loop tick, so the
app must tolerate no-network and no-cameras indefinitely. An optional small
`X-GNOME-Autostart-Delay` (3–5 s) is allowed, but nothing may depend on it. The
app must **not** be restart-looped: warm-up failure exits deliberately, and a
supervisor would turn that into a crash storm.

**Session type:** the greeter runs `Type=x11`, so the autologin session is Xorg.
Do not force `QT_QPA_PLATFORM=xcb` in the launcher until the power-on autologin
session has actually been observed.

### D9 — Transactional install/upgrade
1. Verify archive checksum + manifest; keep extracted input **outside**
   root-owned locations until verified.
2. Validate `<ver>+<sha>` against a strict character allowlist before it is ever
   used as a path component.
3. Host preflight (D7).
4. **Refuse if the app is running** (detect via lock/PID; never kill it) — a
   `current` swap under a live old process leaves it running indefinitely with
   ambiguous model/DB state.
5. Extract into a staging dir **on the same filesystem** as
   `/opt/denso/releases`, verify, then `rename` into place.
6. `ldd bin/denso | grep "not found"` must be empty.
7. Seed models into `<data>/models` — absent → seed; present with same hash →
   leave; **present with different hash → do not overwrite silently** (require
   `--replace-model <stem>`). Engine + sidecar install as a **pair** via temp
   files + atomic rename, so power loss can't leave mismatched artifacts.
8. Back up `denso.db`.
9. `denso --check` gate.
10. Atomically swap `current` (rename a new symlink; never `rm` + `ln`).
11. Leave the previous release intact.

**Rollback is attended, not automatic.** Migrations are forward-only (schema at
v11); flipping `current` back does not un-migrate the database. The DB backup is
the remedy, and this must be documented as such rather than advertised as
rollback.

### D10 — Uninstall
Removes integration + releases; **keeps** `/opt/denso/data` (db, models, logs,
backups) unless `--purge-data`, which warns. Disables only the autostart/
autologin settings this package installed (per D8's key-level restore).

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
| `--version` / `--check` | headless, non-mutating install gate | `TrtEngine`, sidecar reader, `detection::repo` |

## Testing

- **Unit (Catch2, off-device):** `denso::paths` resolution (env set / unset /
  empty / relative); manifest version-string allowlist; seeding decision table
  (absent / same-hash / different-hash); autologin INI edit + key-level restore
  (incl. "admin changed it since" → refuse); dep-plan parser rejecting an
  `apt -s` plan that touches an NVIDIA package.
- **On-device (Jetson .15):** `--check` against a real engine; install onto a
  **clean stock JetPack 6.2 image** (not the build box — that's the only honest
  dep-list test); upgrade over an existing data dir preserving db + models;
  refuse-while-running; power-cycle → autologin → autostart → GUI up.
- **Not covered off-device:** anything needing the NVIDIA graphics/TensorRT
  runtime or a real GDM session.

## Explicitly cut (YAGNI)

Bundled Qt/OpenCV · ONNX in the package (~73 MB dead weight) · install-time
trtexec · trt_cache packaging · `systemd --user` · crash supervision ·
raise-existing-window IPC · auto-derived deps from `ldd` · package signing ·
versioned model artifact filenames · DB-copy migration smoke test · baseline
seeded DB (a fresh DB requires no engines — see ground truth) · automatic
rollback · JetPack versions beyond the one tested.

## Known risks

1. **Dep list rot** — mitigated only by testing install on a clean JetPack image.
2. **Autologin session differs from the observed greeter** (Wayland vs Xorg) —
   resolved by observing the real power-on session before pinning `QT_QPA_PLATFORM`.
3. **Upgrade while running** — mitigated by the lock check; the operator must
   close the app.
4. **Forward-only migrations** make downgrade unsafe; DB backup is the remedy.
