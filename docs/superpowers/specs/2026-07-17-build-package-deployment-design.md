# Build & Deploy: `.deb` package → Jetson appliance install

**Date:** 2026-07-17
**Status:** Approved design, not implemented
**Authors:** Claude + Codex (adversarial debate, 6 rounds)

## Goal

Ship Denso DigitalReader to a Jetson Orin Nano as a **Debian package** the
operator scp's to the device and installs with `apt` — after which the app
behaves like a normal Ubuntu application (app-menu entry, icon, launcher on
PATH, listed in `dpkg -l`, upgraded and removed by `apt`), starts on power-on
unattended, and survives upgrades without losing the database or engines.

**Why `.deb` and not `.tar.gz`+`install.sh`** (the original plan): the stated
goal was "like Discord / Steam / AnyDesk". All three ship `.deb` on Ubuntu, the
Jetson **is** Ubuntu 22.04, and `anydesk 8.0.4 arm64` is already installed as a
`.deb` on the target itself. Choosing the native format deletes most of the
custom, risky machinery a tarball needs: the hand-curated dependency list, the
`ldd` gate, staging + atomic-rename, `uninstall.sh`, payload-manifest
verification, and desktop/icon-cache handling — dpkg does all of it, correctly.

## Verified ground truth

Established by probing the dev/bench Jetson (192.168.1.15) and reading the
source. Do not re-derive; re-verify only if the device image changes.

### Platform

| Fact | Evidence |
| --- | --- |
| Target: Jetson Orin Nano, aarch64, L4T R36.5.0 (JetPack 6.2), sm_87 | `/etc/nv_tegra_release`, `uname -m` |
| **Ubuntu 22.04 — `anydesk 8.0.4 arm64` installed as a `.deb`** (the reference app ships the native format on this very box) | `dpkg -l anydesk` |
| Qt 6.2.4 from Ubuntu apt, **not** in the stock JetPack image (hand-installed on .15) | `dpkg -l \| grep libqt6` |
| xcb platform plugin present, from `qt6-qpa-plugins` | `dpkg -L qt6-qpa-plugins` |
| TensorRT 10.3, CUDA 12.6 from JetPack | `dpkg -l \| grep libnvinfer` |
| OpenCV has GStreamer support (1.20.3) | `cv2.getBuildInformation()` |
| GStreamer elements present: `rtspsrc`, `nvv4l2decoder`, `nvvidconv`, `appsink`, `h264parse` | `gst-inspect-1.0` |
| `nmcli` local-session perms OK: `settings.modify.system=yes`, `network-control=yes` — no polkit rule needed for a desktop-launched app (differs over SSH) | `nmcli general permissions` |
| No passwordless sudo | `sudo -n` fails |
| `systemctl get-default` = `graphical.target`; gdm3 enabled; **greeter** session `Type=x11` | `loginctl show-session` |
| **No active graphical user session** — only the GDM greeter. Power-on halts at the login screen; XDG autostart never fires without autologin | `loginctl list-sessions` |
| Docker 29.3.0 + nvidia-container-runtime + `{devices,drivers}.csv`; 164 GB free | `docker info`, `/etc/nvidia-container-runtime/` |
| No aarch64 cross-toolchain; Windows dev box is MSYS2 UCRT64 → **the package must be built on a Jetson** | — |

### Packaging / linkage

| Fact | Evidence |
| --- | --- |
| `dpkg-deb` 1.21.1 **and `dpkg-shlibdeps`** present → a `.deb` can be built natively on the build box | `which` |
| **The binary is relocatable**: the only RUNPATH is `/usr/local/cuda/lib64` (absolute system path, not build-tree); `/usr/local/lib` is in the ld cache via `ld.so.conf.d/libc.conf`; copying the exe out of the build tree → `ldd \| grep -c "not found"` = **0** → **no `LD_LIBRARY_PATH`, no `$ORIGIN` needed** | `readelf -d`, `ldd` |
| NVIDIA's OpenCV **is dpkg-managed**: `ii libopencv 4.8.0-1-g6371ee1 arm64`, owning `/usr/lib/libopencv_core.so.408`. (Corrects an earlier draft claiming it mapped to no dpkg entry — that probe used the wrong path.) Ubuntu's `libopencv-*4.5d` 4.5.4 coexists; different SONAMEs | `dpkg -l`, `dpkg -L libopencv` |
| **`libopencv` ships NO `.shlibs` and NO `.symbols`** → `dpkg-shlibdeps` cannot derive it. `libnvinfer10` HAS `.shlibs`; Qt has both | `ls /var/lib/dpkg/info/` |
| **`dpkg-shlibdeps` errors strictly**: `no dependency information found for /usr/local/cuda/lib64/libcudart.so.12` → derived Depends covers the Debian-owned libs only; the NVIDIA stack must be hand-declared | `dpkg-shlibdeps -O` |
| Hand-declare set is small and named: `libcudart.so.12` → **cuda-cudart-12-6**; `libopencv_{core,imgproc,imgcodecs,videoio,dnn}.so.408` → **libopencv**; `libcudla.so.1` → NVIDIA L4T | `dpkg -S`, `ldd` |

### Application

| Fact | Evidence |
| --- | --- |
| Linux catalogs `*.engine` + `<stem>.names.json` sidecars; the `*.onnx` scan is `#ifdef _WIN32` only → **ONNX is dead weight on the Jetson** (~73 MB) | `src/app/detection/model_sync.cpp:41-67` |
| `attached_model_filenames()` joins `camera_model` → a fresh DB requires no engines, so a clean install **cannot** fail warm-up → **no baseline DB need be seeded** | `src/core/detection/repo.cpp:48` |
| All runtime state is exe-dir-relative: `denso.db`, `denso.log`, `models/`, `models/trt_cache` | `db/db.cpp:30`, `main.cpp:98,154`, `ui/startup.cpp:111` |
| `EngineRegistry::warm_up()` **mutates** — `fs::create_directories(cache_dir_, ec)` | `engine_registry.cpp:42` |
| `Db::open()` **mutates** — `PRAGMA journal_mode = WAL` (+ `-wal`/`-shm` files) | `db.cpp:76` |
| `main()` constructs `QApplication` **first** → any headless mode must dispatch before it | `main.cpp:84` |
| `TrtEngine` **writes nothing** — `(void)cache_dir;` → constructing it directly is the safe validation path | `trt_engine.cpp:87` |

## Decisions

### D1 — Build on the Jetson
`tools/build_package.sh` runs on an aarch64 JetPack 6.2 machine (the .15 bench
box); cross-compiling is out of scope. Clean out-of-tree Release build → assemble
a package tree → `dpkg-deb --build --root-owner-group` →
`denso-digitalreader_<version>+<sha>_arm64.deb`. Refuses a dirty tree unless
`--allow-dirty`, which it stamps into the package metadata.

### D2 — Dependencies: derived where possible, hand-declared where NVIDIA forces it
No bundling of Qt/OpenCV. Rejected because OpenCV is NVIDIA's build wired to
system GStreamer, and Qt Multimedia / the xcb plugin dlopen the system
GStreamer/X11/EGL/NVIDIA graphics stack — a hand-copied `.so` set is a
dependency-closure project with the NVIDIA graphics libs as its ugliest edge.
`.deb` makes this moot: `Depends:` is the mechanism, and `apt` resolves it.

```
Depends: ${shlibs:Depends},
         libopencv (>= 4.8.0), cuda-cudart-12-6,        # no shlibs metadata — hand-declared
         qt6-qpa-plugins, libqt6sql6-sqlite,            # dlopened — shlibdeps cannot see these
         gstreamer1.0-plugins-base, gstreamer1.0-plugins-good,
         gstreamer1.0-plugins-bad, gstreamer1.0-libav,
         network-manager
```

`${shlibs:Depends}` covers the Debian-owned libs (Qt, TensorRT via
`libnvinfer10.shlibs`). It **cannot** cover: NVIDIA's `libopencv`/`cuda-cudart`
(no shlibs metadata — measured, not assumed), nor anything dlopened (xcb platform
plugin, Qt SQLite driver, GStreamer elements, `nmcli`). A mixed list is
defensible precisely because the manual exceptions are **small, named and
auditable** — this is not the hand-curated whole-list rot we rejected.

**Build must fail, never paper over.** Run `dpkg-shlibdeps` **without**
`--ignore-missing-info` (which would silently drop a dep and look derived while
being wrong — worse than a hand list). `build_package.sh` verifies: every
unresolved SONAME maps to a declared package; each declared package actually owns
the resolved library; no ignored warnings; final `ldd` clean; `denso --check`
passes.

**Install with apt, never `dpkg -i`** — `dpkg -i` does not resolve dependencies:
```sh
sudo apt install ./denso-digitalreader_<version>_arm64.deb
```
Retain a **plan inspection** (`apt-get -s install ./…deb`, `LC_ALL=C`) in
`denso-setup verify`: a `Depends:` declaration does not stop apt from upgrading
or replacing a protected NVIDIA package to satisfy constraints. Abort on any
removal, on any change to `nvidia-l4t-*`/`cuda-*`/`libnvinfer*`/`tensorrt*`, and
on unexpected **Ubuntu** OpenCV installation (its 4.5.4 must never displace
NVIDIA's 4.8.0).

### D3 — Install layout
dpkg owns versioning and file replacement. **No `releases/<ver>` + `current`
symlink** — see D9 for why that was dropped.

```
/opt/denso/bin/denso                    package-owned  (Debian policy allows /opt for add-ons)
/opt/denso/models/                      package-owned  seed engines + sidecars
/opt/denso/data/                        operator-owned mutable state, created by postinst:
                                          denso.db, denso.log*, models/, models/trt_cache/, denso.lock
/opt/denso/install-state/               root-owned setup state (D8)
/usr/bin/denso-digitalreader            launcher: exports DENSO_DATA_DIR, execs /opt/denso/bin/denso
/usr/bin/denso-setup                    configure / verify / unconfigure (D8, D9)
/usr/share/applications/com.denso.DigitalReader.desktop
/usr/share/icons/hicolor/256x256/apps/denso-digitalreader.png
```

Note `/usr/bin`, **not** `/usr/local/bin` — Debian policy reserves `/usr/local`
for the local admin; a package must never install there. (An earlier tarball
draft used `/usr/local/bin`; correct for a tarball, wrong for a package.)

The launcher sets **no** `LD_LIBRARY_PATH` — verified unnecessary (ground truth:
relocatable binary). Desktop-database and icon-cache refreshes are **dpkg
triggers**; we write no code for them.

### D4 — `denso::paths` + `DENSO_DATA_DIR` (code change, load-bearing)
One centralized API for every mutable path — database, log + rotated logs,
models, `.names.json`, trt_cache, lock file, future state. Honors
`$DENSO_DATA_DIR`, defaulting to `applicationDirPath()` so Windows dev and the
test suite are unchanged.

Still load-bearing under `.deb`: `/opt/denso/bin/` is package-owned and
root-owned, so an app running as the operator cannot write `denso.db` beside its
executable, and dpkg replaces that tree on upgrade. Data must live in
`/opt/denso/data/`, which dpkg does not own.

Rejected alternatives: `chown` the program dir (lets the running app rewrite its
own executable); symlinks from the program dir into a data dir (a symlinked
`denso.log` is incompatible with rename-based rotation — the logger renames the
symlink, not the target).

### D5 — Single-instance guard (code change, v1-mandatory)
Autostart + a clickable menu icon guarantees two `denso` processes eventually.
Consequences: duplicate camera opens and inference load, competing network
config, SQLite write contention, and silent log loss — rename-based rotation does
**not** move another process's open file descriptor to the new pathname, so
process B keeps writing into `denso.log.1` after A rotates, and rotation later
renames/deletes inodes another process still holds.

`QLockFile` at `<data>/denso.lock`, acquired **before** the DB opens, logging
initializes, or cameras start — so the data dir must resolve very early. Second
instance: brief "already running" notice if it has a GUI session, then exit with
a documented code. Stale-lock recovery logs to stderr (the sink may not exist
yet). Raise-existing-window IPC (`QLocalServer`) is deferred — polish, not
correctness.

### D6 — Headless modes (code change)
Four modes, **all dispatched before `QApplication` is constructed** (`main.cpp:84`
constructs it first today, so this is a real refactor, not an added branch):

| Mode | Contract |
| --- | --- |
| `--version` | prints version; takes no lock; no mutation |
| `--check` | validates runtime + engines; **no persistent mutation**; takes no lock |
| `--check-running` | liveness; **takes the lock by design** — the sole exemption. **Tri-state**: `0` running / `1` not running / `4` cannot determine (lock unusable). Callers proceed only on `1` — see D9's fail-safe contract |
| `--check-migrations <db-path>` | runs the migration chain against the given path only |

`--check` must not: construct `QApplication`; initialize the rotating log sink;
acquire the production lock; call `EngineRegistry::warm_up()` (creates
`trt_cache`, `engine_registry.cpp:42`); call `sync_models`; run migrations; or
open the DB via `Db::open()` (sets `journal_mode=WAL`, `db.cpp:76`). Instead it:
- opens the DB **read-only** (Qt SQLite read-only connect option + query-only) to
  read configured models. **A missing DB = an empty configured-model set, never
  created** (consistent with: a fresh DB requires no engines);
- constructs `TrtEngine` **directly** — verified safe (`trt_engine.cpp:87`
  `(void)cache_dir;` → reads engine + sidecar, writes nothing) — creates an
  execution context and validates tensor names, modes, dtypes and expected I/O
  shape, optionally running one blank `infer()`. `trtexec --loadEngine` is not an
  acceptable substitute: it proves TensorRT can read the plan, not that *this
  app* can load, bind and execute it;
- parses `.names.json` sidecars;
- confirms every model referenced by the existing DB has a usable engine +
  sidecar;
- probes data-dir writability with a **real create-and-remove** file test —
  `access(W_OK)` is weaker and doesn't prove creation succeeds under the actual
  mount/ACL/quota/read-only conditions.

"No persistent mutation" is the honest term: a temp probe is still a mutation.

**Anything touching `/opt/denso/data` must run as the target user, never root.**
Maintainer scripts and `denso-setup` run as root, so root-owned `trt_cache`/log/
lock artifacts would poison an operator-owned data dir and the real app would
silently fail to write:
```sh
runuser -u "$user" -- env DENSO_DATA_DIR=/opt/denso/data \
  CUDA_CACHE_PATH="$tmp/cuda-cache" TMPDIR="$tmp/tmp" \
  /opt/denso/bin/denso --check
```
with `$tmp` target-user-owned and removed afterwards. `--check-running` is
exempt from the no-lock rule but **not** from this one — as root it would leave a
root-owned lock artifact, the exact poisoning this rule exists to prevent.

### D7 — Engines: ship prebuilt, never build at install
Ship the validated `.engine` + `.names.json`; no `trtexec` at install. Installer
builds fail on: `trtexec` absent, ONNX-parse becoming an install dependency the
app deliberately doesn't need, workspace/memory/thermal/power-mode/disk variance
across a 5–10 min build, builder-flag drift from the production recipe, and
unclear state on interrupt. Engine production stays an explicit operator action;
the package promotes a **validated artifact**.

**Model selection is explicit, never a directory glob.** An earlier draft shipped
"every `models/*.engine` with a matching sidecar"; that is accidental
directory-content selection, and a forgotten experimental engine with a valid
sidecar would reach production. Dirty-tree refusal does not help — models are
untracked (`.gitignore`: `models/*.engine`). Instead: a **tracked packaging
manifest** lists approved model stems with expected SHA-256 and trtexec recipe,
and `build_package.sh --model models/digitv2.engine` must match it. Hash mismatch
is a **hard failure**.

Preflight (in `denso-setup verify`) asserts: `aarch64`; L4T exactly R36.5.0;
NVIDIA driver operational; TensorRT 10.3; CUDA runtime resolvable; compute
capability 8.7; every required `.engine` has its sidecar; **and the engine
actually deserializes on the target** — the authoritative check. Version
comparisons are diagnostics only.

### D8 — Autostart + autologin live in `denso-setup`, not maintainer scripts
`apt install` has no clean way to accept `--user`/`--autostart` options, and
interactive maintainer-script prompts are wrong. So configuration is an explicit
second command:

```sh
sudo apt install ./denso-digitalreader_<version>_arm64.deb
sudo denso-setup configure --user modela --autostart --enable-autologin
denso-setup verify
```

State lives in root-owned `/opt/denso/install-state/` so upgrades can reuse it.
`--user` is explicit and required; `$SUDO_USER` is not trusted (absent in
automation, wrong from a root shell).

**Autostart** writes `~<user>/.config/autostart/com.denso.DigitalReader.desktop`,
owned by the target user, invoking the same launcher as the menu entry. XDG
autostart over a `systemd --user` unit: the latter frequently lacks a correct
`DISPLAY`/`XAUTHORITY`/DBus session environment and complicates polkit identity.

**Autologin** is a **separate, explicit** flag (user-approved). Without it,
autostart never fires — the box halts at the GDM greeter on power-on (verified:
no active graphical user session). `denso-setup`:
- verifies the user exists with a valid home, GDM is the active display manager,
  and `systemctl get-default` is `graphical.target` (report and require an
  explicit option rather than silently changing either);
- backs up `/etc/gdm3/custom.conf` preserving permissions;
- parses the INI and sets only `[daemon] AutomaticLoginEnable=true` and
  `AutomaticLogin=<user>` — never replaces the file from a template;
- records the **original pre-Denso values ONCE**, **never rebased on upgrade**.
  (Bug this avoids: first install records `AutomaticLoginEnable=false` then sets
  true; an upgrade that re-records the *current* value would store `true` as the
  "original", and uninstall could never restore the real prior state.) State
  distinguishes: Denso enabled it / it was already enabled by the admin / the
  admin later changed the user / Denso's values are still active;
- `denso-setup unconfigure` (and `postrm purge`) restore **only those keys, and
  only if they still hold the values we set** — a blind full-file restore would
  erase later admin changes;
- warns that autologin grants anyone with physical access that desktop session.

**Startup timing:** no delay is required for correctness. `CameraStream` already
reconnects and network reassert is deferred to the first event-loop tick, so the
app must tolerate no-network and no-cameras indefinitely. An optional small
`X-GNOME-Autostart-Delay` (3–5 s) is allowed, but nothing may depend on it. The
app must **not** be restart-looped: warm-up failure exits deliberately, and a
supervisor would turn that into a crash storm.

**Session type:** the *greeter* runs `Type=x11`. That does **not** prove the
logged-in user session is Xorg — GDM's greeter and the GNOME user session can
differ. Honest statement: greeter is X11; **the user session type is unknown
until autologin is exercised**. Do not force `QT_QPA_PLATFORM=xcb` until the
power-on autologin session has actually been observed.

### D9 — Lifecycle: dpkg owns it; maintainer scripts stay trivial
**The staging + `current`-symlink design was dropped**, and this is the sharpest
call in the spec. Codex argued for keeping it inside `postinst` (copy payload →
validate → flip `current`) so a bad upgrade could never replace a working
release. It doesn't earn its complexity **because every install here is
attended**: the user installs each box by hand over AnyDesk. There is no
unattended upgrade, no fleet, no CI push. The app *runs* unattended; it is never
*upgraded* unattended. So the failure it prevents — a bad upgrade silently
bricking a box at the next power-cycle — cannot occur: a human just typed
`apt install`, is watching the output, has the DB backup, and GNOME is still up
so AnyDesk works even if `denso` won't start. Worst case is minutes of attended
downtime in a maintenance window he chose. Against that, staging costs: a second
versioning system layered on dpkg's; **two sources of truth about what version is
running** (`dpkg -l` says 0.2.0, `current` → 0.1.0) — which bites at 2am; and it
loads the most logic into `postinst`, the one script whose failure is worst (a
failed `postinst` leaves the package unconfigured and blocks later apt
operations). Codex conceded: *"dpkg's configured version should be the version
installed and launched."*

Maintainer scripts are therefore **minimal, idempotent, and hard to fail** (dpkg
may re-invoke them during recovery):

| Script | Does |
| --- | --- |
| `prerm` | **Proceed ONLY on exit code 1** from `--check-running` (as the target user) — see the fail-safe contract below. Failing here leaves the old package installed — exactly right. Never kills the app; never deletes or "repairs" the lock. |
| `postinst` | Create + chown `/opt/denso/data{,/models}`; seed missing model pairs; cheap structural sanity only. Nothing that can reasonably fail after a successful package build. |
| `postrm purge` | Remove `/opt/denso/data` behind a **resolved-path guard** (resolve symlinks, assert the canonical path, refuse anything else); revert autologin/autostart from install-state. Plain `remove` keeps all data. |

**The `--check-running` fail-safe contract — `prerm` MUST get this exactly right.**
`--check-running` is tri-state, because "I couldn't tell" is not the same answer
as "nothing is running":

| Code | Meaning | `prerm` must |
| --- | --- | --- |
| `0` | an instance IS running | **refuse** |
| `1` | definitely NOT running | **proceed** |
| `4` | **cannot determine** — the lock file is unusable (missing/read-only/root-owned data dir, wrong ownership) | **refuse** |
| any other | unexpected | **refuse** |

**Proceed only on exactly 1.** The obvious shell is wrong:
```sh
if denso --check-running; then refuse_upgrade; fi   # WRONG: proceeds on BOTH 1 and 4
```
That treats "couldn't tell" as "safe", which is precisely the unsafe upgrade —
replacing a release under a live app — that the tri-state exists to prevent. Use:
```sh
set +e
runuser -u "$user" -- /opt/denso/bin/denso --check-running
rc=$?
set -e
case "$rc" in
  1) ;;  # definitely not running — proceed
  0) echo "Denso is running; refusing upgrade" >&2; exit 1 ;;
  *) echo "Cannot establish Denso liveness; refusing upgrade" >&2; exit 1 ;;
esac
```
Note `--check-running` is the one mode that takes the lock (answering requires
`tryLock`), so it must run **as the target user** — as root it leaves a
root-owned lock artifact in an operator-owned data dir, which then makes every
later check return `4`.

Validation lives in **`denso-setup verify`**, run by the human at the keyboard:
apt-plan inspection (D2), host preflight (D7), engine deserialize via `--check`
(D6), and the **migration smoke test** — copy `denso.db` into a throwaway
target-user-owned dir and run `--check-migrations` against the copy (no existing
DB → an empty temp DB, exercising the full v0 chain the app runs on first
launch). Mutation stays confined to the copy.

**Model seeding** (postinst, idempotent): absent → seed; present with same hash →
leave; **present with a different hash → never overwrite silently** (require an
explicit `denso-setup replace-model <stem>`). Engine + sidecar are **ordered and
crash-resistant, NOT atomic** — two flat files cannot be made atomic with two
renames. The engine's appearance is the commit marker: write+fsync sidecar temp →
rename sidecar → write+fsync engine temp → rename engine **last** → fsync the
directory. A newly-appearing engine therefore always has its sidecar. Replacement
semantics are weaker; document recovery rather than claim pair atomicity.
(Versioned immutable model filenames would solve this cleanly — cut, see YAGNI.)

**Rollback is attended:** keep the previous `.deb` and `apt install ./old.deb`.
Migrations are forward-only (schema v11), so a downgrade does not un-migrate the
database — the DB backup taken by `denso-setup verify` is the real remedy. Do not
advertise this as rollback.

**Integrity:** dpkg verifies the package's own md5sums (`dpkg -V`); the operator
can check a `.sha256` alongside the `.deb` before installing. Neither
authenticates the publisher — not supply-chain verification. Signing arrives free
if we ever host an apt repo (phase 2, YAGNI for a handful of hand-managed boxes).

### D10 — Removal
`apt remove` drops the program and integration and **keeps** `/opt/denso/data`;
`apt purge` additionally runs the `postrm purge` path above. Both are gated by
`prerm`'s running-instance refusal. No `uninstall.sh` — dpkg's remove/purge
semantics already match the keep-data-unless-purge rule exactly.

## Components

| Unit | Purpose | Depends on |
| --- | --- | --- |
| `tools/build_package.sh` | Jetson-side: clean Release build → package tree → `dpkg-deb --build` → `.deb` (+ dep derivation & verification, D2) | git, cmake, dpkg-deb, dpkg-shlibdeps |
| `packaging/debian/control` | metadata + the mixed `Depends:` (D2) | — |
| `packaging/debian/{prerm,postinst,postrm}` | minimal idempotent lifecycle (D9) | `denso --check-running` |
| `packaging/denso-setup` | `configure` / `verify` / `unconfigure` / `replace-model` (D8, D9) | `denso --check`, `--check-migrations`, apt, gdm |
| `packaging/denso-digitalreader` (launcher) | export `DENSO_DATA_DIR`, exec `/opt/denso/bin/denso` | — |
| `packaging/com.denso.DigitalReader.desktop` | menu entry + autostart template | launcher, icon |
| `packaging/lib/*.sh` | pure installer policies: apt-plan guard, seeding decision, GDM INI edit/restore, version allowlist | — |
| `src/core/paths/` (`denso::paths`) | single source of truth for mutable paths | `$DENSO_DATA_DIR` |
| single-instance guard | `QLockFile` before DB/log/cameras | `denso::paths` |
| `--version` / `--check` / `--check-running` / `--check-migrations` | headless gates, dispatched before `QApplication` | `TrtEngine`, sidecar reader, read-only `detection::repo`, `db::run_migrations` |

## Testing

**Where each policy authoritatively lives** — installer policies are shell, so
Catch2 cannot reach them; implementing them twice (C++ + shell) to make them
testable would be worse than not testing them. Single implementation each:

| Policy | Lives in | Tested by |
| --- | --- | --- |
| `denso::paths`, lock, the four headless modes | C++ | Catch2 |
| apt-plan guard, seeding decision, GDM INI edit/restore, version allowlist | `packaging/lib/*.sh` (pure functions, sourced) | `tests/packaging/` shell harness (assert-based, no new deps) |

- **Unit (Catch2, off-device):** `denso::paths` resolution (env set / unset /
  empty / relative); `--version`/`--check` take no lock and don't construct
  `QApplication`; `--check` performs no persistent mutation (no `trt_cache`, no
  `-wal`/`-shm`, no log, no lock) **and treats a missing DB as an empty
  configured-model set without creating one**; `--check-migrations` migrates the
  copy it is given and leaves the live DB untouched, incl. the empty-temp-DB
  case; `--check-running` reports correctly and releases the lock when unheld;
  second-instance rejection.
- **Unit (shell harness, off-device):** apt-plan parser rejecting a plan that
  removes anything or touches a protected family; seeding decision table (absent
  / same-hash / different-hash); GDM INI edit + key-level restore incl. "admin
  changed it since" → refuse, and the never-rebase-on-upgrade rule; version
  allowlist.
- **Package-level (Jetson):** `lintian` on the built `.deb`; `apt install ./…deb`
  resolves deps; `dpkg -l` lists it; **`prerm` refuses while running**; `apt
  remove` keeps `/opt/denso/data`; `apt purge` removes it; reinstall/upgrade is
  idempotent; `dpkg -V` clean.
- **On-device (Jetson .15):** `denso-setup verify` against a real engine; upgrade
  over an existing data dir preserving db + models; power-cycle → autologin →
  autostart → GUI up → network + camera recovery.
- **Supplemental container diagnostic (NOT a gate):** see below.
- **v1 acceptance test:** the first install onto a real target device. There is
  no substitute.
- **Not covered off-device:** anything needing the NVIDIA graphics/TensorRT
  runtime or a real GDM session.

### Container diagnostic — what it does and does not prove

Docker + nvidia-container-runtime are available on the Jetson, so a container run
is *cheap supplemental evidence*. It is **not** a clean-image dependency gate and
must not be described as one: the package contract includes the host OS, display
manager, NVIDIA multimedia stack and desktop session — not just ELF loading.

`l4t-base:r36.2` is actively misleading: it has no in-image CUDA/TensorRT/NVIDIA
OpenCV (CSV injection supplies host/device integration, not the JetPack SDK
filesystem), so those read as "missing" and we would wrongly add them to
`Depends:` — the opposite of the goal. Closest usable image is
`nvcr.io/nvidia/l4t-jetpack:r36.4.0`, **pinned by digest, not tag**, with the
image's pre-test package inventory captured so our `Depends:` gets no credit for
packages already present. It is R36.4 while the target contract is exactly
R36.5.0.

Proves: the package installs and its deps resolve in that container; the exe
loads; headless TensorRT/CUDA init works; engine validation succeeds.
Does **not** prove: deps are sufficient on a clean JetPack 6.2 device; NVIDIA
OpenCV/GStreamer integration matches the target rootfs; xcb works; NVDEC works;
GDM autologin/autostart works; apt leaves the real target's NVIDIA packages
untouched.

## Explicitly cut (YAGNI)

`.tar.gz` + `install.sh`/`uninstall.sh` (superseded by `.deb`) · staging +
`releases/<ver>` + `current` symlink (D9) · bundled Qt/OpenCV · ONNX in the
package (~73 MB dead weight) · install-time trtexec · trt_cache packaging ·
`systemd --user` · crash supervision · raise-existing-window IPC · a
hand-curated whole dependency list · package signing / hosted apt repo (phase 2)
· versioned model artifact filenames · baseline seeded DB (a fresh DB requires no
engines) · automatic rollback · JetPack versions beyond the one tested · a
separate `--check-gui` mode (the operator launches once from the menu over
AnyDesk, exercising the real session; a root context may lack display
credentials anyway, so nothing may depend on `DISPLAY`).

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

**Slice 2 — packaging + appliance integration** (Jetson-verifiable only):
launcher + desktop + icon + autostart template → pure shell helpers (apt-plan
guard, seeding, GDM INI, allowlist) + their harness → `debian/control` with
derived+manual `Depends:` and the D2 verification → minimal maintainer scripts →
`denso-setup` → `build_package.sh` → lintian + container diagnostic → bench
install → upgrade preservation + prerm refusal → power-cycle → autologin →
autostart → GUI → network/camera recovery.

## Known risks

1. **Dep list rot** — reduced but not eliminated: `${shlibs:Depends}` is derived
   and self-maintaining, but the NVIDIA + dlopened block is hand-declared. The
   D2 build-time verification is the mitigation; the container diagnostic is not
   a substitute; the first real-target install is the honest gate.
2. **User session type unknown** (Xorg vs Wayland) — the greeter is X11 but that
   doesn't determine the user session. Observe the real power-on autologin
   session before pinning `QT_QPA_PLATFORM`.
3. **A failed `postinst` blocks later apt operations** — accepted, and the reason
   maintainer scripts are deliberately trivial and idempotent.
4. **Forward-only migrations** make downgrade unsafe; the DB backup is the
   remedy, and `denso-setup verify`'s migration smoke keeps a bad migration from
   reaching a live DB.
5. **`/opt/denso` mixed ownership** (root program + operator `data/`) is
   deliberate; D6's run-as-target-user rule is what keeps root artifacts out of
   `data/`.

## Decision history

Rounds that changed the design, kept because the reasoning is the spec's load:
- **R1** Codex killed the bundled-Qt/OpenCV plan and install-time `trtexec`;
  Claude's "catalog `*.onnx`" premise was wrong (`model_sync.cpp` is
  `#ifdef _WIN32`) — dropped ~73 MB.
- **R2** Claude refuted Codex's "seed a baseline DB or first launch exits"
  (`repo.cpp:48` — fresh DB requires no engines). Bundling retired empirically
  (relocatable binary, 0 `ldd` not-found).
- **R3** Codex proved `--check` was mutating three ways (`engine_registry.cpp:42`,
  `db.cpp:76`, `main.cpp:84`); migration gate reinstated after Claude wrongly cut
  it; pair-atomicity claim killed; engine glob → explicit approved list.
- **R4** `--check-running` lock exemption; `--check-migrations` added (the gate
  had no executable path); fresh-install missing-DB behavior defined.
- **R5** User asked "another way if not tar.gz?" → `.deb` wins (AnyDesk on the
  target *is* a `.deb`). Claude's "dpkg derives deps mechanically" over-sold:
  measured that `libopencv` ships no shlibs and `dpkg-shlibdeps` errors on
  `libcudart` → mixed derived/manual `Depends:`.
- **R6** Claude refuted Codex's staging/`current` hybrid on evidence (installs are
  always attended) → dpkg owns activation; postinst stays trivial.
