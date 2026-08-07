# Architecture

Depth reference for Denso-DigitalReader (C++ / Qt Widgets / CMake). For the
quick map and commands, see the root `CLAUDE.md`.

**Verifying on real hardware:** the parts of this design that only exist on the
target — native TensorRT on `sm_87`, NVDEC/GStreamer, the Linux network backend,
and (for deployment) a real GDM session — cannot be proven on the Windows dev
box. The shared device registry at **`d:\workspace\devices.md`** (outside this
repo) has the Jetson's address, credentials, AnyDesk ID and toolchain versions.

## Project layout

Split by concern, wired by a thin top-level `CMakeLists.txt` via
`add_subdirectory`. The app side is the `denso` exe plus three pure static
**subsystem libs** that both `denso` and `denso_tests` link:

| Path | Target | Depends on |
|---|---|---|
| `src/core/` | `denso_core` (static lib) | `Qt6::Core`, `Qt6::Sql` only |
| `src/app/detection/` | `denso_detection` (static lib) | `denso_core`, `Qt6::Core`, OpenCV |
| `src/app/brazing/` | `denso_brazing` (static lib) | `denso_core`, `Qt6::Core` |
| `src/app/camera/` | `denso_camera` (static lib) | `denso_core`, `denso_detection`, `Qt6::Core`/`Gui`, OpenCV |
| `src/app/` | `denso` (Qt Widgets GUI exe) | the three libs + `Qt6::Widgets`/`Multimedia`/`Network` |
| `tests/` | `denso_tests` (Catch2) | `denso_core` + the three subsystem libs |

Final lib graph: `denso_core ← {denso_detection, denso_brazing, denso_camera
(→denso_detection)} ← denso`. The libs hold the **pure, portable** logic
(inference helpers, reporting logic, non-widget capture infra); backend-coupled
engines, Qt-Network transport, and Qt Widgets stay in `denso`. Linking (rather
than re-compiling the `.cpp`s into `denso_tests`) means the tests validate the
same objects the app ships.

`denso_core` holds the ported logic + SQLite persistence **and** the Qt-free
domain↔view boundary (`core/ui/convert` + `viewmodel`). It never links
`Qt6::Widgets`, so the GUI cannot leak into the testable core. Each target's
directory is its own include root: core headers read `network/model.h` /
`ui/convert.h`, the app's widget headers read `ui/theme.h` /
`ui/camera/camera_dialog.h`, and the app reaches core headers through
`denso_core`'s public include dir.

`src/core/reading/` is the append-only detection-reading log (migration v9);
see [Reading log](#reading-log) below. On the app side, `src/app/ui/common/`
is a **leaf**: shared dialog chrome (header, async runner, label/row
factories) with no feature dependencies, so `settings_dialog` and
`camera_dialog` build on it instead of each keeping its own copies.

`src/app/ui/camera/` now holds **widgets only**; the non-UI capture, detection,
and brazing subsystems live in the sibling `src/app/{camera,detection,brazing}/`
dirs (the subsystem libs). Note two `camera/` dirs coexist — `src/core/camera/`
(domain) and `src/app/camera/` (runtime) — resolved by filename through the
separate include roots. Bounded 24/7 file logging lives in `src/app/logging/`
(see [Logging](#logging)).

Outside the CMake graph: `packaging/` and `tools/` are the POSIX-shell ship
pipeline (see [Packaging & ship pipeline](#packaging--ship-pipeline-packaging-tools)),
tested by their own harnesses — `tests/packaging/run.sh` and the Jetson-only
`tests/manual/repro_build.sh` — because ctest covers only the C++ targets.

## Boot sequence (`src/app/main.cpp`)

A thin orchestrator:

0. **Headless dispatch, before any Qt application object exists.** `cli::parse`
   (pure, `src/core/cli/`) turns argv into a `Command`; if the mode is headless,
   `main` constructs a **`QCoreApplication`** — never `QApplication` — and hands
   off to `app::run_headless` (`src/app/cli/`). This ordering is the point:
   `QApplication` loads the xcb platform plugin, so an installer running
   `--check` from a display-less root shell would die before reaching it. See
   [Headless modes](#headless-modes).
1. `QApplication` is constructed for the GUI path. It must come first among the
   GUI steps, because `paths::data_dir()`'s fallback needs `applicationDirPath()`.
2. **The single-instance lock is acquired** (`instance::SingleInstance` over
   `paths::lock_file()`) — *before* the DB, the log sink, or any camera. A second
   instance prints to stderr, shows a message box, and exits **3**. The guard is
   an automatic local, deliberately not `static`: it must be destroyed before the
   `QApplication` above it and before the static log sink it may log through.
3. The bounded file-logging handler is installed (`src/app/logging`, see
   [Logging](#logging)) so every later step's `qDebug/qWarning` lands in
   `denso.log` — the Windows GUI subsystem has no console. Then
   `QLocale::setDefault(English/UnitedStates)` forces Western Arabic digits in
   numeric widgets (`QSpinBox`/`QDoubleSpinBox`) regardless of the OS regional
   format — without it a Thai-locale host renders spin-box values as Thai
   numerals (๐–๙).
4. `db::Db::open(paths::db_file())` opens `denso.db` **in the data dir** (see
   [Mutable paths](#mutable-paths)) in WAL mode; `db::run_migrations` applies the
   `user_version`-gated chain. These are **DB-stage readiness faults, not generic
   failures**: an unreadable database (`DbUnopenable`), one written by a newer
   build (`SchemaNewer` — classified *before* normal startup, so boot and
   `--check` agree on a future-schema appliance) and a failed migration
   (`MigrationFailed`) all `qCritical`, write the reason to the log **and
   `status.json`**, and exit **78** (`EX_CONFIG`) — a configuration fault no
   restart fixes, which is what tells an operator (and a service supervisor) to
   stop retrying and look at the data dir. Ordinary runtime and warm-up failures
   keep their own separately defined exit behaviour (e.g. a missing TensorRT
   engine still `app.exit(1)`s from `WarmupWorker`). None of these use `qFatal`:
   it aborts without unwinding, which would strand the lock file with a dead pid
   and make the operator's next attempt report "already running" instead of the
   real cause.
5. `settings::import_legacy` does a one-time import of any pre-SQLite
   `settings.json` in the data dir (`paths::legacy_settings_json()`).
6. `ui::sync_models` scans the data dir's `models/` (`paths::models_dir()`) and upserts each into
   the `model` catalog, so models dropped there become selectable in the camera
   wizard. **Platform-split** (`model_sync.cpp` — the ONNX branch is
   `#ifdef _WIN32`): Windows catalogs `*.onnx` with class names from the ONNX
   metadata; Linux/Jetson catalogs `*.engine` with class names from a
   `<stem>.names.json` sidecar, skipping any engine whose sidecar is missing or
   unparseable.
7. `network::reassert` re-applies every saved interface config to the OS — the
   app is the source of truth. Best-effort and non-fatal: failures are logged
   via `qWarning`, never block startup. It is **deferred to the first event-loop
   tick** (`QTimer::singleShot(0, …)`, exceptions swallowed) and runs *after*
   `ui::launch`, so the window shows first — a slow or stuck OS CLI (now bounded
   by the backend's `QProcess` timeout) can no longer keep startup from painting.
8. `settings::load` seeds an in-memory `std::shared_ptr<Settings>`.
9. `main` hands off to `ui::launch(app, conn, state)` (`src/app/ui/startup.cpp`),
   which builds the shared `EngineRegistry`, then picks the launch UX via
   `ui/startup_mode`'s `cold_start_needs_splash` — which is **platform-split**,
   so the cold/warm split below is the *Windows* story:

   - **Windows/ORT** (`startup_mode.cpp:36-40`): **Cold** = `*.onnx` present and
     no cached `*.engine` → the first-run TensorRT build takes minutes, so show
     the splash. **Warm** = engine cached → no splash.
   - **Linux/Jetson** (`startup_mode.cpp:47-48`): engines are prebuilt, so
     there's no multi-minute build — but deserialize + warm-up still costs a few
     seconds per model, so it splashes **whenever a `*.engine` exists**. The
     ORT-cache notion of "already warm" does not apply (`cache_dir` is ignored).
     A Jetson with models configured therefore takes the splash path normally.

   **Cold path:** show the `StartupScreen` splash, warm every model on the worker
   while it animates, and on `finished` build + show `MainWindow` (with
   `warmup = nullptr`, so `CameraGrid` starts every camera immediately on
   cache-hits). **Warm path (UI-first):** build + show `MainWindow` immediately
   with a `WarmupState`, then `WarmupState::start()` warms in the background and
   `CameraGrid` starts model-less/ready cameras at once and each pending detection
   camera as its models come ready — no splash.

`Db` (an `optional<Db>` in `main`) outlives the window, so the connection it
hands the UI stays valid for the whole run.

## Mutable paths

`denso::paths` (`src/core/paths/`) is the **only** place that decides where
mutable state lives — database, log + rotated siblings, models, TRT cache, lock,
legacy `settings.json`. It resolves `$DENSO_DATA_DIR` when set and non-empty,
else falls back to `applicationDirPath()` (the historical behavior, so the
Windows dev box and the test suite are unchanged), else `"."`.

Why it exists: an installed build's program dir is root-owned and is **replaced
on upgrade**, so state kept beside the executable would be unwritable at runtime
and destroyed by every package upgrade. The deployment launcher points
`DENSO_DATA_DIR` at `/opt/denso/data`. Derived paths use `QDir::filePath`, not
concatenation — `QDir::cleanPath` keeps the separator on a filesystem root, so
`"/" + "/denso.db"` would yield `"//denso.db"`.

## Headless modes

Four CLI modes, all dispatched **before** `QApplication` (step 0 above) and run
under a `QCoreApplication`, so none needs a display. They are the gates the
`.deb` installer and `denso-setup` call (see the deployment spec in
`docs/superpowers/specs/`).

| Mode | Contract |
|---|---|
| `--version` | prints `APP_VERSION`; takes no lock; no mutation |
| `--check [--engine <file>]...` | validates the data dir + every model the DB references **and** each `--engine` named; **tri-state** exit (`0`/`10`/`78`, below); does not mutate the primary database or app-managed state — see the ownership caveat below |
| `--check-running` | liveness; **the sole mode that takes the lock** — answering requires `tryLock` |
| `--check-migrations <db-path>` | runs the migration chain against **that path only** — a throwaway **COPY**. Not read-only despite the name, and it *creates* a missing file, so a mistyped path yields a new empty database that migrates cleanly and reports ok. Never aim it at the live database. |
| `--apply-migrations` | the **one production migration primitive**: migrates the primary database resolved from `DENSO_DATA_DIR`, in place. Takes **no path**, so it cannot be aimed elsewhere by a typo in a maintainer script and cannot conjure an empty database. Runs the read-only schema classifier *first*, so a DB written by a **newer** build is refused with **78** before the chain opens it for writing. Never rolls back. Exit **0** = at the supported schema (migrated, already current, or absent); **78** = blocked. |

Exit codes (Slice 2's maintainer scripts depend on these): **0** ok — and for
`--check-running`, *an instance is running*; **1** generic failure — and for
`--check-running`, *nothing is running*; **2** bad usage; **3** the GUI refused
to start because another instance holds the lock; **4** (`--check-running`
only) *cannot determine* — the lock file itself is unusable (missing dir,
permissions, ...). This must never be reported as the clean 1 a caller like
Slice 2's `prerm` needs to see before it will proceed with an upgrade.

**`--check` is tri-state**, mapped from `health::Readiness` by
`health::exit_code` (`src/core/health/integrity.cpp`) — a plain pass/fail would
force the caller to choose between blocking a serviceable appliance and shipping
a broken one:

| Code | Readiness | Meaning |
|---|---|---|
| **0** | `Ready` | every referenced model resolves and the schema is sound |
| **10** | `Degraded` | serviceable but not clean — *issues*, not *blockers* (e.g. engines on disk the manifest does not describe). Callers may proceed; this is why `sync_models`' directory scan is retained rather than treated as a fault. |
| **78** | `Blocked` | `EX_CONFIG` — a blocker (`DbUnopenable`, `SchemaNewer`, a missing/invalid engine the DB references). Same code the boot path uses for the same conditions, deliberately: boot and `--check` must agree. |

`1` remains the generic-failure code and is **not** a readiness verdict; the
`--check-running` meanings above are a separate contract and unaffected.

**Ownership caveat for `--check`:** "does not mutate the primary database or
app-managed state" is deliberately narrower than "no mutation at all" — see
`Db::open_read_only`'s contract in `src/core/db/db.h`: a WAL reader needs the
`-shm` index and SQLite may create it, and that file can outlive an abnormal
exit. Running `--check` as the target user bounds **ownership** of any such
support file, not whether one gets created. Installer/setup callers MUST run
`--check` as the target user, never as root — a root-owned `-shm` in an
operator-owned data dir is exactly the poisoning the data-dir rules exist to
prevent.

`--check` deliberately does **not** reuse the normal startup path:
`EngineRegistry::warm_up()` creates the TRT cache dir and `Db::open()` runs
`PRAGMA journal_mode = WAL` — both mutations. It instead opens the DB read-only
(`Db::open_read_only`, which never creates an absent file) and constructs
`BackendEngine` **directly** against a throwaway `QTemporaryDir` cache, so
`OrtEngine` cannot write the real `trt_cache` either. It validates names via
`engine.class_names()` rather than parsing a sidecar, because the backends source
them differently: `TrtEngine`'s ctor reads `<stem>.names.json` itself and throws
without it, while a Windows `.onnx` has no sidecar at all (names live in the ONNX
metadata). Note `TrtEngine::ok()` is a hardcoded `return true` — on the Jetson
the ctor throwing is the only real signal, so both are checked.

A **missing** database is an empty configured-model set (a fresh DB references no
cameras, so it legitimately requires no engines) and is never created; a
**present-but-unreadable** one is a hard failure. Keeping those distinct is why
`detection::try_attached_model_filenames` returns an `optional` — the older
`attached_model_filenames` returns `{}` for both, which would let a corrupt
database pass as a fresh install.

## Install and runtime architecture

### Fresh install resolves an operator; it never assumes one

`sudo apt install ./<deb>` is the entire fresh-install procedure. `postinst`
resolves exactly one operator account and hands the rest to
`denso-setup configure --user <u> --autostart --auto`, reusing the tool that was
verified on hardware rather than keeping a second copy of the
ownership/seeding/autostart logic inside a maintainer script.

`resolve_operator_user` (policy.sh — pure, fixture-tested) applies one precedence:

| | Rule | Why |
|---|---|---|
| 1 | the recorded `install-state/user` | an upgrade must never be re-pointed at whoever runs sudo today. A recorded-but-invalid user is a **hard failure**, not a fall-through — falling through is exactly how an appliance would be silently taken from its operator |
| 2 | `SUDO_USER` | the normal interactive `sudo apt install` case |
| 3 | exactly one acceptable **local, active, non-remote** session user | last resort |

Rule 3's filters are not decoration. `Class=user` drops the GDM greeter — which
runs as the `gdm` system account and is otherwise the only "graphical" session on
a powered-on appliance — and `Remote=no` drops the administrator's own SSH
session, because resolving to whoever SSH'd in is the wrong answer when they are
installing *for* someone else.

Refused: root, `nobody`, system/service accounts, `nologin`/`false` shells, uids
outside `[1000,60000]`, and unknown names. **Zero candidates and several
candidates both fail the installation.** Nothing guesses; nothing ever falls back
to root. The operator username is arbitrary and is never hardcoded.

Autostart is enabled automatically. **Autologin is never touched** — no
maintainer script references GDM, and `denso-setup configure --enable-autologin`
remains an explicit operator action.

### One graphical launch path

```
graphical login → systemd --user → denso-digitalreader.service
                                     → /usr/bin/denso-digitalreader  (exports DENSO_DATA_DIR)
                                     → /opt/denso/bin/denso

desktop / menu click → systemctl --user start denso-digitalreader.service
                       (the SAME unit; a no-op when already active)
```

**systemd `--user` is the sole autostart authority.** There is no XDG autostart
entry, and the legacy one is deleted on migration. That is not tidiness: with an
entry still in `~/.config/autostart`, `systemctl --user disable` would not have
stopped Denso starting at login, and `disable` would be a lie.

`denso-digitalreader.service` is a systemd **user** unit in
`/usr/lib/systemd/user/`, never a system/root service: Denso is a GUI and must
live and die with the operator's graphical session, and a root service would have
no session, no display, and would write root-owned artifacts into an
operator-owned data dir. Four deliberate properties:

- **`[Install] WantedBy=graphical-session.target`**, so `enable`/`disable`
  (and `--now`) are real. The anchor was verified on the target, not assumed:
  `gnome-session.target` declares `BindsTo=`/`Before=graphical-session.target`
  so the target is genuinely reached, `enable`/`disable` were measured to create
  and remove `~/.config/systemd/user/graphical-session.target.wants/…`, and the
  target's `RefuseManualStart=yes` does not block dependency wiring.
- **`PartOf=graphical-session.target`**, so Denso stops when the session ends.
- **`Restart=no`.** Denso is single-instance; a restart loop against a held lock
  produces exit-3 churn that buries the original fault.
- **No invented display.** `ExecStartPre=/usr/bin/denso-session-check` fails
  first, naming the missing `DISPLAY`/`WAYLAND_DISPLAY`, rather than letting Qt
  abort on a display that is absent or belongs to the greeter. On this X11
  appliance (`WaylandEnable=false`) GNOME publishes the real values through
  `/etc/X11/Xsession.d/95dbus_update-activation-env`
  (`dbus-update-activation-environment --systemd --all`) before the session
  target is reached; nothing in the package supplies a value of its own.

Nothing runs `systemctl` at build or install time. `postinst` enables the unit
for the resolved operator by creating the same `.wants` symlink
`systemctl --user enable` would — that user is normally not logged in during
`apt install`, so there is no user manager to talk to. A hand-made symlink was
measured to be reported `enabled`, removed by `systemctl --user disable`, and
re-creatable by `systemctl --user enable`.

**Legacy XDG → systemd migration** is guarded by
`install-state/autostart-migrated` and preserves intent: legacy entry present →
remove **only** that file and enable the unit; legacy entry absent → autostart
was deliberately off, so the unit is left disabled; marker present → do nothing.
An upgrade therefore can never silently re-enable a service the operator turned
off. A fresh install never enters this path — it always enables, and writes the
marker so migration never runs on it.

### Lifecycle status is not application health

| Question | Answered by |
|---|---|
| process/service lifecycle and autostart | `systemctl --user status\|start\|stop\|restart\|enable\|disable denso-digitalreader` |
| is an instance holding the lock | `denso-digitalreader --check-running` (0/1/4, tri-state) |
| application, database and model health | `denso-digitalreader --check` (0 Ready / 10 Degraded / 78 Blocked) |

These are not interchangeable, and `systemctl status` must never be documented as
a replacement for the application health checks.

Runtime output reaches the journal (`StandardOutput`/`StandardError=journal`).
The appliance runs a **volatile** journal with no per-user journal files, so the
supported reader is
`sudo journalctl _SYSTEMD_USER_UNIT=denso-digitalreader.service -f`.
`journalctl --user -u …` requires persistent per-user journals, which this
package neither requires nor configures — a package has no business changing a
whole machine's log retention and write policy.

## Upgrade architecture (manual `.deb`)

Denso is an embedded appliance updated by an **administrator**, manually. There
is **no automatic updater**.

### Two ownership domains

The whole design rests on one split, and dpkg respects it:

| Domain | Paths | Owner | Lifecycle |
|---|---|---|---|
| **Package-owned / immutable application content** | `/opt/denso/bin`, `/opt/denso/lib`, `/opt/denso/models` | `root:root` | **Replaced** by the `.deb` on every upgrade |
| **Operator data** | `/opt/denso/data` (`denso.db`, WAL/SHM, `denso.log`, `models/`, `denso.lock`, `denso.db.pre-v*`) | the resolved operator |  **Persists** across normal package upgrades and `apt remove` |

The package **does not own the operator database**, its WAL/SHM, or generated
migration backups — dpkg never touches `/opt/denso/data`. That is precisely why
mutable state lives there and not beside the root-owned, upgrade-replaced binary,
and why `DENSO_DATA_DIR` exists.

### Upgrade flow

```
Admin maintenance
    ↓
Denso stopped
    ↓
APT/dpkg package replacement
    ↓
postinst shared DB gate  (packaging/lib/policy.sh :: db_upgrade_gate)
    ↓
schema classification
    ↓
verified backup if migration required   (denso.db.pre-v<schema>)
    ↓
--apply-migrations
    ↓
integrity / runtime checks              (denso --check)
    ↓
application left stopped
    ↓
admin launches application
```

### Invariants

- **Forward-only.** A database written by a *newer* build is refused before the
  chain opens it for writing. Downgrades are not supported.
- **Fail-closed.** `postinst` exits **non-zero** on a failed backup, migration or
  integrity check — deliberately reversing its own older "structural only, never
  fail" rule. dpkg then leaves the package unconfigured, which is the intended,
  visible manual-recovery gate; `dpkg --configure -a` retries it.
- **Manual recovery.** Nothing is ever automatically restored, and no backup is
  ever automatically deleted or pruned. The halt message names the backup path.
- **No automatic updater. No automatic rollback.**
- **The package does not own the operator database.**
- **`purge` is destructive to operator data by Debian package policy.**
  `postrm purge` runs `rm -rf /opt/denso/data` — deleting the database, the
  operator's engines and every `denso.db.pre-v*` recovery point — behind a
  `readlink -f` guard that refuses if the path does not resolve to exactly
  itself. `remove` and `upgrade` delete nothing.
- **Never as root.** Every database operation runs through
  `runuser -u <recorded-user> --`. Opening a WAL database creates `-wal`/`-shm`
  beside it and `--check` writes a probe file into the data dir; a root-owned
  artifact in an operator-owned data dir is the documented failure mode (`prerm`
  then reads `--check-running` as 4 — "cannot determine" — forever).
- **Backup naming is deterministic** (`denso.db.pre-v<schema>`, no timestamp) so
  a `dpkg --configure -a` retry finds and *keeps* the existing recovery point
  instead of overwriting it with a copy of the half-migrated database. It also
  bounds the backup count by schema version, which is what makes "no automatic
  pruning" safe to promise.

`packaging/denso-db-helper` performs the backup and the `user_version` /
`integrity_check` reads using the **Python 3 standard-library `sqlite3` module**
— `Connection.backup()`, the SQLite online backup API, which is consistent under
WAL. It is deliberately not a copy of `denso.db`/`-wal`/`-shm`, and deliberately
not the `sqlite3` CLI: that CLI is absent from the Jetson image, and depending on
it would put an apt fetch in the middle of an offline `.deb` upgrade.

### Operating modes and model payload

The package ships the canonical **six-file** model set — `digitv3`,
`float-small`, `float-big`, each `.engine` plus its `.names.json` sidecar.
Production packaging is TensorRT-engine only; no `.pt` or `.onnx` is required or
shipped.

| Mode | Models | Family |
|---|---|---|
| Digital Number Reader | `digitv3` only | `digit_numeric` |
| Floating Ball Leveler | `float-small`, `float-big` only | `float_ball` |

Carrying all three models does not let either mode load the wrong one: the
`canonical_id → family → allowed modes` registry in
`src/core/models/compatibility.cpp` is the sole grantor of authority, a
`static_assert` forbids any family from being allowed in both modes, and the
manifest never states privileges — so an operator edit under the models
directory cannot widen what a model may do. At runtime the warmup logs
`skipping <engine> (not permitted by the compatibility policy in the current
mode)` for the other mode's models.

## Packaging & ship pipeline (`packaging/`, `tools/`)

Not CMake targets: POSIX shell, so they are proven by their own harnesses rather
than ctest — `tests/packaging/run.sh` (312 assertions natively on the Jetson; the
file-mode ones are Linux-only and skip elsewhere) and, Jetson-only,
`tests/manual/repro_build.sh` (19). AGENTS.md holds the operator runbook and the derived-dependency rules;
README.md holds the copy-paste install. This section is the *why*.

| Path | Role |
|---|---|
| `tools/build_package.sh` | The whole build. Release invocation is `--models-dir <repo-models-dir>`, which requires **exactly** the canonical six-file set in reviewed manifest order and refuses a missing engine, a missing sidecar, an unexpected fourth engine or any `.pt`/`.onnx` (`resolve_models_dir` in `policy.sh`, so the rule is testable off-Jetson). `--model` remains for one-off/diagnostic builds and is mutually exclusive with it. Refuses a non-aarch64 host outright and a dirty tree unless `--allow-dirty` — never use `--allow-dirty` for a release artifact. Stages `/opt/denso`, derives deps via `dpkg-shlibdeps`, emits `dist/` = `<deb>` + `<deb>.sha256` + `preflight-denso-<ver>.sh` + `<name>.tar.gz`. |
| `packaging/lib/policy.sh` | The ONE definition of "would this transaction damage the JetPack stack?" (`apt_plan_ok`). |
| `packaging/lib/gen_preflight.sh` | Emits the standalone preflight guard. |
| `packaging/lib/gen_bundle.sh` | Emits the transport bundle. |
| `packaging/denso-setup` | Post-install operator tool (`configure` / `verify` / `preflight` / …). |
| `packaging/debian/` | `control.in`, `postinst`/`prerm`/`postrm`, `shlibs.local`. |

**The preflight guard is generated, never hand-written.** `denso-setup preflight`
cannot protect a *first* install — `denso-setup` ships inside the very `.deb` it
would vet, so by the time it exists the transaction is already done. Hence a
standalone twin beside the `.deb`. It is assembled by concatenating `policy.sh`
**verbatim**, so both routes share ONE definition of the protected-package
decision (`apt_plan_ok`); a hand-copy would drift from `policy.sh`, and a second
emitter would let the two callers' generated scripts drift from each other.
Concatenation rather than sourcing because the guard must run on a box where
`/opt/denso` does not exist. Note what this does **not** guarantee: the two
drivers around that shared decision (apt simulation, path normalization, error
handling) are separately implemented in `gen_preflight.sh` and `denso-setup` and
*can* drift — `tests/packaging/run.sh` checks them against representative plan
fixtures, which is regression coverage, not proof. Version skew is also normal
and expected: an installed `denso-setup` uses the installed package's policy,
while a freshly generated guard embeds the *prospective* package's.

**The `.deb` and its guard ship as one bundle** because they are useless apart:
the guard embeds the SHA-256 of the exact `.deb` it was generated for and refuses
any other. Pairing an old guard with a new `.deb` used to be silent (every build
overwrote one `preflight-denso.sh`), producing a PASS for an artifact never
inspected. The `.tar.gz` makes "these travel together" a property of the artifact
instead of a step in a runbook. `SHA256SUMS` inside it is **regenerated with bare
filenames** — copying `dist/<deb>.sha256` would record the path `dist/…`, so
`sha256sum -c` would fail on a perfectly intact bundle unpacked in `$HOME`.

**Clean builds are byte-reproducible on one machine, and must be.** A clean
artifact is named `r<count>.g<sha>` with no content hash, so that name is a
truthful identifier only if one commit yields one set of bytes. It did not:
rebuilding a commit produced different bytes under an *identical* filename,
silently replacing an artifact an operator may already have shipped and recorded.
Four independent sources of variance had to be closed, each sufficient on its own:

- `SOURCE_DATE_EPOCH` — pinned to the **commit** timestamp, feeding both the
  MANIFEST date and `dpkg-deb`'s mtime clamping (it reads the variable from the
  environment). A clean build **refuses** a differing caller-supplied value:
  honouring it, as the reproducible-builds convention would, hands two clean
  builds of one commit different bytes under one name — the original defect by
  another door. A dirty build may override, since its name carries a content hash.
- **`gzip -n`.** gzip writes its own timestamp and filename into its header, so
  `tar -czf` is non-reproducible even when every tar entry is pinned
  (`--sort=name --owner=0 --group=0 --numeric-owner --mtime=@epoch`).
- **Modes set explicitly rather than umask-inherited.** The bundle's top-level
  directory came out 0775 on the Jetson (umask 002) and 0755 elsewhere — a byte
  difference under an identical name, caused by an entry that holds no data.
  Everything staged into the `.deb` is now pinned too: the payload via
  `install -m`/`install -d` plus an explicit `chmod` on the redirect-created
  `MANIFEST`, and `DEBIAN/control`/`md5sums` at `0644`.

  **Know the asymmetry, because it decides where the risk is.** `dpkg-deb
  --build` normalizes **control**-archive members to 0644 whatever the staged
  mode (measured on-device: staged 0600 → 0644 in the `.deb`), but it does
  **not** touch the **data** archive — a payload file staged 0600 ships 0600. So
  the control-file pins are defense-in-depth against an implicit dependency on
  another tool's behaviour, while the payload pins are the ones actually holding
  the guarantee up. A future `>`-created payload file with no `chmod` is a real
  reproducibility defect; the same mistake in `DEBIAN/` is silently absorbed.
  This was established by measurement after a review predicted the opposite —
  see the two-umask checks in `repro_build.sh`, which assert modes in both real
  archives rather than trusting either rule.
- **ASLR addresses stripped from the MANIFEST's `ldd` output.** This one
  re-randomized the `.deb` on *every* build and is invisible, since only the hex
  in `(0x0000ffff91f00000)` changes. It defeated all the timestamp work until it
  was found; the soname→path mapping, which is the actual diagnostic value, stays.

Reproducibility is scoped **per-machine and clean-only** — the MANIFEST records
toolchain and JetPack versions, so a different box legitimately differs, and a
dirty build is disambiguated by a hash suffix rather than made reproducible.

**Shell rules these scripts follow**, each from a real defect:

- Emitters are **subshell-bodied functions** `f() ( … )`, not `{ … }`: POSIX shell
  variables are global, so a brace body's working variables clobber a caller's of
  the same name. A subshell still writes its output file and propagates status.
- **`{ cmd || rc=$?; } | other` cannot capture `rc`** — a pipeline component runs
  in its own subshell, so the assignment never reaches the caller and `rc` stays
  0. `tar | gzip` was written that way with a comment claiming a failing tar could
  not be masked; it could, and gzip would compress the truncated stream into a
  valid-looking archive. Split into separate steps over an intermediate file.
  (`PIPESTATUS` is bash-only and these are POSIX sh.)
- **The emitters** (`gen_preflight.sh`, `gen_bundle.sh` — not every packaging
  output; `dpkg-deb` writes the `.deb` straight to its final path) write to a temp
  file in the **destination directory**, chmod it, then `mv`: a generator dying
  mid-heredoc must never leave a partial-but-executable script where an operator
  might run it, and a same-directory `mv` is an atomic rename.

## UI ↔ domain boundary (`src/core/ui/`)

Feature modules never reference UI view types. `ui/convert.{h,cpp}` is the
single crossing point: `to_*` build view models (`viewmodel.h`:
`NetStatus`/`NetConfigUi`/`WifiRow`) from domain types, `from_ui_config` parses
an editable view model back to a domain `NetConfig` (blank/unparseable fields
become unset). It is `std::string`-only and unit-tested (`test_convert.cpp`);
the widgets convert to/from `QString` at their edge.

A config change travels: UI edit → `SettingsDialog::apply_net_config` →
`from_ui_config` → `network::save` (persist; app owns truth) →
`backend().apply_config` (push to OS) → status string back to the card.

## GUI (`src/app/ui/`)

Grouped by feature so the folder scales: the **app shell** at `ui/` root, with
`ui/settings/` and `ui/camera/` subfolders.

**Shell (`ui/`)**
- `theme.{h,cpp}` — the dark/light palette + a stylesheet builder applied to
  the whole app (the Slint `Theme` global / `Palette.color-scheme` analog).
- `mainwindow.{h,cpp}` — root window: top button bar (Camera / Settings) over
  the content area. Hosts the settings-persistence handlers (resolution / theme
  / fullscreen / reset), since those resize the window and restyle the app, and
  opens the settings + camera modals.

**Common (`ui/common/`)**

A **leaf**: three shared dialog primitives, Qt-only with no feature
dependencies, so either dialog can build on them without depending on the
other. `dialog_chrome.{h,cpp}` (`dialog_header`) builds the consistent modal
title bar; `async_runner.{h,cpp}` (`run_on_worker`/`post_to_gui`) wraps the
worker-thread-then-marshal-back pattern each dialog's threaded operations
(scan/connect/refresh, snapshot capture) already needed; `form_widgets.{h,cpp}`
(`eyebrow`/`dim_label`/`spec_row`/`hline`) are the small label/row factories
that used to be copy-pasted between `settings_dialog`'s anonymous namespace and
the camera dialog's `page_util`. `page_util::dim_label` now delegates to
`common::dim_label` instead of keeping its own definition.

**Settings (`ui/settings/`)**
- `settings_dialog.{h,cpp}` — modal: a left nav over five panels (Appearance,
  Display, System, Network, About). The Network panel is extracted into
  `network_panel.{h,cpp}` (`NetworkPanel`), a self-contained widget that owns
  the two `NetCard`s, the DB handle, and the threaded apply/scan/connect/refresh
  handlers (`on_shown()` re-seeds editors + refreshes status, reproducing the
  Slint original's "entering the tab reloads"); `settings_dialog` itself is now
  a thin view over the nav + the four other panels.
- `netcard.{h,cpp}` — one interface's live status + editable IP/DNS config +
  (Wi-Fi) scan list with per-row connect.

**Camera (`ui/camera/` widgets + the `src/app/camera/` runtime lib)**

`ui/camera/` now holds **widgets only**, under two root entry points
(`camera_view`, `camera_dialog`) plus the `wizard_controller`. **`grid/`** is the
live-view widgets (`camera_grid`, `camera_tile`, `grid_layout`). **`dialog/`** is
the modal internals (the five page widgets + `page_util`, plus the dialog-only
`wizard_stepper`, `roi_canvas`, and the source scanners `camera_devices`,
`ip_scan`). **`shared/`** is now just `roi_geometry` (the last cross-cutting UI
primitive). The **non-widget runtime** — `frame_processor`, `fps_meter`,
`stream_pacing`, `warmup_gate`, `preview_gate`, `zone_assembly`, `safe_process.h`, `snapshot`,
`frame_convert`, `rtsp_templates`, `gst_pipeline` — moved to the sibling
`src/app/camera/` dir (the `denso_camera` lib); `camera_stream` (a `QObject`)
also moved there in path but stays compiled into `denso`. Dependencies flow one
way: the runtime lib + `shared/` are leaves; `grid/` and `dialog/` depend only on
them, never on each other; the root entry points compose all three.

- `camera_view.{h,cpp}` — the main content area: a switcher between the empty
  "no cameras" state (+ Add) and the live **`CameraGrid`**. `release_streams()`
  stops capture while the Camera modal is open (so its snapshot can claim the
  same USB device); `reload()` rebuilds + restarts when the modal closes.
- `camera_grid.{h,cpp}` / `camera_tile.{h,cpp}` / `camera_stream.{h,cpp}` —
  the live 1–4 feed grid. `CameraGrid` lays out one `CameraTile` + one
  `CameraStream` per **`camera::runtime()`** camera — enabled AND finished, filtered
  in SQL (first four by id; `grid_dims` picks 1 / 1×2 / 2×2).
  Each `CameraStream` runs a `cv::VideoCapture` read loop on its **own
  `std::thread`**, converts each frame (`mat_to_qimage`), runs it through a
  `FrameProcessor` (wrapped in `safe_process` so a throw from a malformed frame
  can't kill the capture thread), and emits `frame_ready`/`status_changed` as
  **queued** signals to its tile (capped ~15 fps; finite open/read timeout so a
  dead camera can't hang teardown; `stop()` joins). `run()` is an **outer
  reconnect loop**: a failed open or a mid-stream read drop no longer ends the
  thread — it emits Offline, backs off (`next_backoff_ms`: 1s→×2→10s cap, reset
  on a live frame) via a stop-responsive `wait_or_stop`, and reopens, so a camera
  that blinks out recovers on its own without an app restart. Each source opens
  via a **capture-backend ladder** — the first candidate that opens AND reads a
  frame wins (remembered for reconnects): RTSP through hardware **NVDEC**
  `rtsp_gst_pipeline` (H.264 → H.265) then FFMPEG; USB MJPEG → YUYV → `CAP_ANY`.
  Mixed-codec fleets auto-discover; GStreamer drops stale frames so
  glass-to-glass lag stays bounded. The
  display cap is paced by a high-resolution waitable timer (`precise_sleep`),
  because MinGW's `std::this_thread::sleep_for` is pinned to the ~15.6 ms OS
  tick and would undershoot the target rate (~9 fps for a 15 fps cap). The
  loop's flow-control policy is factored into pure, unit-tested helpers in
  `stream_pacing.{h,cpp}`: `next_backoff_ms` (reconnect backoff schedule) and
  `should_emit` (drop-oldest backpressure gate). Backpressure is a
  `shared_ptr<atomic<int>>` in-flight counter shared by the stream and its tile:
  the stream only emits when `should_emit(queued, kMaxInFlight=2)` and increments;
  `CameraTile::set_frame` decrements on consume. A GUI that falls behind drops
  frames instead of letting full-res `QImage` events pile up unboundedly (OOM).
  `CameraTile` is a pure view — paints the latest frame aspect-fit with a name,
  status dot, and a live per-tile FPS readout (`FpsMeter`), and overlays the
  camera's saved ROI polygons (`set_areas`) as gold outlines. The overlay maps
  the normalized vertices through the **same** `roi_geometry::fitted_image_rect`
  the frame is drawn into, and the frame is already oriented (the stream's
  processor), so ROIs — stored normalized to the oriented frame — line up
  without extra transform. When detection is active the ROI is also enforced on
  the *pixels*: `DetectionProcessor` keeps only boxes whose centre falls inside
  an area polygon (empty areas = whole frame).
- `frame_processor.{h,cpp}` — the per-camera processing seam. `FrameProcessor`
  is the interface; `OrientationProcessor` (applies rotation/pitch/roll) is the
  orientation-only impl, and `DetectionProcessor` layers detection on top,
  **decoupled from display**: it orients on the display path but runs the platform
  inference backend (ORT or native TensorRT) on a worker thread over a drop-oldest
  latest-frame slot, overlaying the newest boxes (per-class confidence filter →
  ROI confinement → labelled boxes). The ROI step keeps only boxes whose normalized centre lands
  inside one of the camera's area polygons (`camera::inside_any_area`); a camera
  with no areas detects the whole frame.
  `camera_grid` picks per camera: `DetectionProcessor` when the camera has
  attached, loadable models (resolved via `detection::detection_for` +
  `EngineRegistry`), else plain `OrientationProcessor` — the capture loop and
  tile don't change. Every per-frame `process()` call is wrapped in
  `safe_process()` (`safe_process.h`) so a throw from a malformed frame is caught
  on the capture thread and the raw frame is shown instead — one bad frame can't
  `std::terminate()` the process. `grid_layout.{h,cpp}` is the pure, unit-tested
  `grid_dims(n)`.
- `camera_dialog.{h,cpp}` — the camera management hub: a thin **view** over a
  5-page stack run as a guided wizard — list + delete, then **① Source**
  (USB auto-scan, or IP via manufacturer + main/sub stream + credentials with a
  live RTSP-URL preview) → **② Configure** (snapshot preview + resolution / fps /
  rotation / pitch / roll) → **③ Models** (attach 1..N detection models, each with
  per-class confidence) → **④ Areas** (draw ROI polygons). Each page is its
  own widget under `dialog/` (see below), owning its controls and emitting
  request signals; the dialog itself owns only the page stack, the
  `WizardStepper`, and modal sizing (Back/Next/Finish footers; `show_page(index)`
  switches the stack page, drives the stepper, and resizes — the modal grows to
  **near-fullscreen on the Areas step** for drawing room and restores the
  compact size on leaving). All flow-state, the threaded snapshot capture, and
  every DB write (camera insert/update, model attach, ROI replace) live in
  `wizard_controller.{h,cpp}` (`CameraWizardController`, a `QObject`, not a
  widget): the controller never touches the `QStackedWidget` or stepper
  directly — it drives page transitions through an injected `show_page`
  callback and a `request_show_list()` signal for "return to the list", and
  emits `cameras_changed()` for the main view to refresh. The camera is
  inserted/updated when Configure's **Next** is pressed, so both the Models and
  Areas steps attach to a known camera id; Models persists via
  `detection::set_camera_models` on its Next, Areas is **optional** (Skip
  returns without writing ROIs, Finish saves them). Each list row also has an
  **Areas** button to draw/edit later. `showEvent` reopens the reused dialog on
  the list at compact size. Persists through `camera::repo` + `detection::repo`.
- **Editable source + ROI quarantine.** The Source page is editable on an
  existing camera, not just on add. Because moving the lens or swapping the feed
  invalidates ROIs drawn against the old view, `wizard_controller` diffs the edit
  with `camera::requires_area_review` (pure `source_change` logic:
  `same_effective_source` + `view_geometry_changed`) and, when it changed the
  effective source or capture geometry, sets `camera.areas_need_review`
  (migration **v11**). While that flag is set the live grid **excludes those ROIs
  and pauses zone reporting** — the tile shows an "Areas need review" banner
  (`CameraTile::set_review_paused`). Clearing it requires the operator to save the
  Areas step against a **valid live preview** of the new source (saving with no
  preview is refused, so ROIs are never "verified" blind). A cosmetic edit
  (name / credentials) leaves any existing flag untouched. The Source page also
  tags devices already owned by another camera **"(in use)"** (the controller
  pushes the used IP/USB set via `push_used_sources()` before each scan), and the
  Models page has **Select all / Clear** buttons for a model's class list.
- `dialog/` — the five page widgets the dialog coordinates, each self-contained
  and DB-light: `page_util` (shared `dim_label` + error colour), `list_page`
  (`CameraListPage`: reads/deletes cameras, emits add/configure/areas requests),
  `add_page` (`CameraAddPage`: the Source form + USB/IP scans, emits the
  assembled draft), `configure_page` (`CameraConfigurePage`: preview +
  orientation controls; the coordinator pushes frames in via `set_frame`),
  `models_page` (`ModelsPage`: lists the model catalog with an attach checkbox +
  per-class select/conf, pre-filled from `models_for`; emits its selections for
  `set_camera_models`), and `areas_page` (`CameraAreasPage`: edits a working ROI
  set over the pushed background frame, emits the set on save — no DB access of
  its own). The Areas page validates before it emits, so a predictable problem
  is named there instead of arriving as a generic write failure from
  `replace_areas`: an unresolved draw blocks the save, degenerate and
  self-intersecting polygons are refused, and zone clashes are reported with
  their holder (the picker disables zones held by other cameras —
  `zones_owned_by_other_cameras` — and by this camera's other areas). Drawing is
  a locked sub-task: the list and "+ New area" disable until the shape is
  finished or cancelled. Anything that discards work confirms first, including
  `CameraDialog::reject()` so Escape and the window's X can't slip past the
  guard the Back/Exit buttons apply.
- `camera_devices.{h,cpp}` — USB enumeration via Qt Multimedia (`QMediaDevices`).
- `ip_scan.{h,cpp}` — crude IP discovery: a threaded subnet probe for hosts with
  the RTSP port open (Qt Network).
- `rtsp_templates.{h,cpp}` — manufacturer → RTSP URL template map (Dahua for
  now); builds the credential-free URL and injects credentials at capture time.
- `snapshot.{h,cpp}` + `frame_convert.h` — grab one frame for the Configure
  preview (OpenCV `VideoCapture`, off the GUI thread, finite open/read timeout)
  and orient it for display. `apply_orientation(src, degrees, pitch, roll)`
  composes the preset rotation + roll (in-plane, about Z) + pitch (out-of-plane
  tilt about X, rendered as a `QTransform` perspective warp about the image
  centre); the perspective viewer distance is derived from frame size, not
  stored. The rotation combo and the pitch/roll spin boxes all re-render the
  preview live on change. `apply_rotation` (the 0/90/180/270 preset) stays as a
  separately-tested helper.
- `roi_canvas.{h,cpp}` + `roi_geometry.{h,cpp}` — the **Areas** page's editing
  surface. `RoiCanvas` paints the oriented snapshot (reusing `apply_orientation`,
  so ROIs sit on exactly the configured view) plus the camera's *other* areas,
  dimmed and zone-labelled, so coverage and overlap are visible while editing.
  Three modes: **Idle** (view only), **Drawing** (click to add a vertex; click
  the ringed first vertex / double-click / Enter / "Done shape" to close), and
  **Editing** (drag a corner, click an edge to insert one, tap a corner then
  "Remove corner" to drop it — never below 3). It holds vertices **normalized to
  [0,1]** and knows no DB policy — the page loads/persists.
  `roi_geometry` is the pure, unit-tested mapping it builds on: aspect-fit rect,
  widget↔normalized conversion, `image_contains`, `hit_test_vertex` (nearest
  wins), and `nearest_edge_insert_index` (distance to the segment, closing edge
  included).

  Two rules worth knowing before touching this. **A click is gated by
  `image_contains`; a drag is not.** The clamp in `to_normalized` is a safety
  net, not a gate — gating the click stops a tap in the letterbox bars becoming
  a vertex silently pinned to the frame's edge, while a drag that leaves the
  frame is the operator holding the handle and pulling, which reads as "put it
  on the edge". **Because a drag clamps, coincident vertices are manufactured,
  not a fluke:** two corners pulled past the same border land on the identical
  coordinate, and several pulled off one side end up collinear along it. That is
  why `camera::polygon_self_intersects` treats touching and collinear overlap as
  intersections, not just proper crossings — those shapes pinch the polygon shut
  while keeping a healthy area, so the area floor cannot catch them, and
  `point_in_polygon`'s even-odd fill would turn the lobes into holes.
- `wizard_stepper.{h,cpp}` — `WizardStepper`, the non-interactive
  "① Source — ② Configure — ③ Models — ④ Areas" indicator above the page stack;
  `set_current()` emphasizes the active step. Navigation stays with the dialog's
  Back/Next/Finish buttons.

### Threading

All four blocking OS calls — `apply_net_config`, `scan_wifi`, `connect_wifi`,
`refresh_network` — run on a worker `QThread` (`QThread::create`, so QProcess in
the backends has an event dispatcher) and post results back with
`QMetaObject::invokeMethod(this, …, Qt::QueuedConnection)`. This is the Qt
analog of the Rust `std::thread` + `upgrade_in_event_loop`. A fresh
`network::backend()` is created per operation. **Worker lifetime is guarded in
two layers** (a dialog can be closed mid-operation): `run_on_worker` returns the
`QThread*`, which `NetworkPanel` records in `workers_` and `wait()`s in its
destructor; and each worker captures a `QPointer<NetworkPanel>` and skips its
`post_to_gui` if the panel is already gone. Together they close the
use-after-free window on `this`. A per-panel `net_busy_` flag (set at each
handler's entry, cleared in its GUI post) plus a disabled Refresh button
serialize actions, so a rapid double-click can't spawn duplicate workers or
double-apply a config. Moving `apply_net_config` off the GUI thread means a
stuck `netsh` (now bounded by the backend's `QProcess` timeout) can no longer
freeze the UI.

## Operating modes (`src/core/mode/`)

The appliance does exactly **one** job at a time, selected by an explicit
operator action. Two modes exist:

| Mode | Token | This release |
|---|---|---|
| Digital Number Reader | `digit_reader` | the shipping job — `DetectionProcessor` + zone reporting |
| Floating Ball Leveler | `ball_leveler` | persistence exists; the operator surface is **still guarded** — see below |

**Ball Leveler persistence exists; the operator surface does not.** Schema v14
gives Ball Leveler a durable home (`ball_level_calibration`), and
`save_level_configuration` is its one write chokepoint. What is still NOT
implemented: the calibration UI, inference, percentage mapping, OpenCV level
annotation, the runtime state machine and the EngineRegistry replacement.
Selecting `ball_leveler` therefore still persists the mode, retains every camera
connection, and lands on an explicit "setup is not available in this release"
state — no stream, no processor, no reporter, no wizard. `mode_setup_required`
is no longer hardcoded `true`: it is driven by real calibration, and answers
`nullopt` (undeterminable) rather than guessing when its query fails.

### `mode.target` — a key, not a schema change

`src/core/mode/` lives in `denso_core` — **Widgets-free, `Qt6::Core`/`Sql` only**
(the load/save/switch entry points take a `QSqlDatabase`) — and rides the existing
`settings` key/value table under the key **`mode.target`**, so the mode key itself
needs no migration. **The schema is at v14**, raised by the additive
`ball_level_calibration` table (one row per camera, `camera_id PRIMARY KEY`), not
by anything the mode key required.

`parse_target_mode` follows the `parse_display_mode` contract: **any absent,
unknown or corrupt token resolves to `digit_reader`, never to the newer mode**,
so every existing installation upgrades with no operator action and no behaviour
change.

`mode.target` is deliberately **NOT** a field on `settings::Settings`.
`MainWindow::on_reset_defaults` assigns a default-constructed `Settings`
wholesale, so any field wired into that aggregate would be wiped by "Reset to
defaults" — silently changing the appliance's job. `settings::save` does not
delete unknown keys, so an independent key is untouched by that path.

### What a switch preserves vs destroys

**Camera connections are preserved.** A camera's connection and capture
configuration is a property of the *appliance*, not of the job it is doing, so
every `camera` row and its stable **`id`** survive a switch — all 18 identity /
connection / capture columns (including `active`) are untouched. The operator
never re-enters an RTSP URL, credential, USB index, resolution or orientation
because the appliance changed job. Only the two **processing** columns,
`setup_complete` and `areas_need_review`, are reset to 0.

The **mode-owned processing workspace is destroyed atomically**: every row of
`camera_area`, `camera_model`, `camera_model_class`, `reading` and
`model_migration_receipt`. `camera_model_class` is deleted **unconditionally**,
not scoped by `camera_model_id` — `camera::remove` already orphans such rows, so
a scoped delete would strand them; the unconditional form also repairs
pre-existing damage. Receipts must go because they store `camera_model_id`
values and SQLite reuses rowids (no `AUTOINCREMENT`), so a retained receipt could
repoint an unrelated future attachment.

**Reporting is disabled.** `brazing.enabled` is set false **inside the same
transaction**; `brazing.base_url` is preserved. Re-enabling is an explicit
operator action — it must never resume implicitly when the new mode is later
configured. Preserved alongside it: the display settings, `net_config`, the
`model` catalog, and everything on disk (engines, sidecars, `trt_cache`, logs).

`camera::runtime()` filters `active = 1 AND setup_complete = 1`, so zeroing
`setup_complete` on every row makes it **empty by construction** — the guarantee
that nothing streams after a switch lives in SQL, not in UI logic.

### Ordering rules that are not locally obvious

These are the invariants a reader would otherwise have to rediscover:

- **Teardown must precede the transaction.** The switch calls the ONE
  authoritative teardown primitive (`CameraGrid::teardown()` → `clear()`);
  re-implementing the sequence is forbidden. **`CameraView::reload()` must NOT be
  used as the pre-transaction teardown** — it clears and then immediately
  re-queries `runtime()`, which still returns the *old* mode's cameras because
  the transaction has not run yet, restarting every one of them.
  `release_streams()` is also insufficient: it joins only capture threads,
  leaving a queued frame able to reach the `ZoneSink` afterwards.
- **The in-memory mode is updated only after the commit succeeds.** On rollback
  the handler **re-reads** the mode from the database rather than keeping an
  optimistically-assigned target. Otherwise SQLite's atomicity stays intact while
  the running process disagrees with the DB about what the appliance is doing —
  worse than either failure alone.
- **Rollback reloads the old pipeline.** Any failed statement or commit rolls the
  whole transaction back — mode key, reporting flag and every camera's processing
  flags revert together — the old mode's pipeline is rebuilt, and the SQL error
  is surfaced verbatim.
- **Warm-up is deliberately left running** across a teardown. `warm_up()` has no
  cancellation and `quit()` cannot interrupt a blocking call, so destroying it
  would block the GUI thread for the remainder of a TensorRT build. Late
  `on_model_ready`/`on_warmup_finished` are harmless no-ops.
- A queued callback from a torn-down worker cannot inhibit a camera in the new
  generation: `clear()` advances a **generation counter** first, and both
  `WorkerFailedFn` and the `status_changed` lambda drop events whose captured
  epoch is stale. This matters *because* camera ids are retained — a stale event
  for `camera_id 3` now names a real, live row.

**The reporting guarantee is narrow and precise.** Once the confirmed switch
enters the synchronous teardown, no inference result or retry tick can *initiate*
a new transport request: the GUI thread is blocked throughout, inference
callbacks are posted with the `BrazingReporter` as their Qt context and that
context is destroyed before the event loop resumes, the retry `QTimer` is
parented to the reporter, and in-flight replies die with the
`QNetworkAccessManager`. It is **not** claimed that a request already handed to
the OS socket fails to reach the server. A pending-but-undelivered snapshot is
discarded and logged — **zone count and zone numbers only, never payload
values**.

### Operator-facing surface

Confirmation is up-front (stated consequence, **default Cancel**, no
type-to-confirm). The safe action is what Enter triggers: `Cancel` is given
`setDefault`/`setAutoDefault(true)` and initial focus, and the accept button's
auto-default is explicitly cleared so the button box cannot promote the switch.
The copy carries **no counts and promises no deletion** — it states that camera
connections and BOTH modes' setup are kept, that processing pauses while the
target mode is prepared, and that reporting is turned off with its address kept.
It must never say "cannot be undone": the switch is reversible, and saying
otherwise would frighten an operator out of a safe action. There is no
confirm/revert countdown — nothing is destroyed, so there is nothing to revert.

`CameraView` has **three** states, because `camera::all()` can be non-empty while
`runtime()` is empty and the old "No cameras yet" copy would be a
lie: the empty state (+ Add Camera), the retained-connections
setup-required state (`digit_reader`, with a Set up cameras action), and the
retained-connections **unavailable** state (`ball_leveler`, with **no** setup
action). In `ball_leveler` the top-bar Camera button is disabled and
`open_camera()` short-circuits, so the wizard is unreachable by any path.

`status.json` carries `mode` and `mode_setup_required`, both **omitted rather
than guessed** when the DB cannot be read (the earliest writer runs precisely
when the DB is unopenable, schema-newer, or migration-failed). `CameraGrid` is
the **single owner** of runtime status writing — its live `refresh_status_file()`
and, for an intentionally idle mode, `publish_idle_status()`; the orchestrator
never writes `status.json` directly, and no caller ever passes a placeholder
`IntegrityVerdict{}` that would erase real blockers.

**Known limitations (documented, not defects):** the switch visibly freezes the
UI while capture threads join (GStreamer candidates open with no timeout);
`EngineRegistry` never unloads, so a switch frees no GPU memory; the backend
receives no "mode changed" signal; and `evaluate_integrity` has **no
Leveler-specific readiness checks** — it is camera/model-centric, so no amount of
Leveler mis-configuration can register as a fault. (It still reports Degraded or
Blocked from the mode-independent model-directory / manifest /
unmanifested-engine checks — an idle `ball_leveler` appliance with an
unmanifested engine reports `degraded`, not `ready`.)

**Validation note:** on-device mode validation runs on the Jetson at
`192.168.1.15` **only**. `192.168.1.81` is reserved for manual `.deb` testing and
is excluded from all automated and remote operation.

## Persistence model (`src/core/db/`)

One file, `denso.db`, WAL mode so the UI reads while a worker writes. The schema
is an ordered, `user_version`-gated chain inside `db::run_migrations`
(`db/db.cpp`) — add a migration, never edit a shipped one. Each feature's `repo`
exposes only the operations its data policy allows (e.g. `hardware` is not
stored at all). The `settings` table is a typed key/value store; `net_config`
is typed columns, one row per interface.

## Network feature (`src/core/network/`)

Two distinct datasets share the Network tab:
- **Live status** — `snapshot()` reads the OS (`ipconfig`/`netsh` on Windows,
  `nmcli` on Linux) via `QProcess`. Read-only, transient.
- **Config** — `NetConfig` is user-owned, persisted, and reasserted to the OS
  at boot via `reassert`.

OS work sits behind the `NetworkBackend` base class. `backend()` returns the
platform impl (`WindowsBackend`, `LinuxBackend`, or a `NullBackend` fallback);
each platform's code is grouped under `network/windows/` and `network/linux/`,
and exactly one `*_backend.cpp` is compiled per OS (the other
`make_*_backend()` declaration is never odr-used). The pure helpers are
unit-tested off-device: Windows `netsh`/`parse`/`wifi`, Linux `nmcli`. Errors
mirror the Rust `Result::Err(String)` as a thrown `std::runtime_error`;
`reassert` catches them into non-fatal `(iface, message)` pairs. Every `QProcess`
wait is **bounded** (`waitForFinished(15s)` → `kill()` + 2s grace), never
`-1` — a stuck OS CLI can't wedge the calling thread; `run` returns empty on
timeout, `run_checked` throws a timeout error.

## Detection feature (`src/core/detection/` + `src/app/detection/`)

Per-camera YOLOv8 detection, split across the same domain/runtime line as the
rest of the app. **Domain** (`src/core/detection/`, Qt/OpenCV-free, unit-tested):
`detection.h` structs (`DetectionModel` catalog rows, `CameraModel` attachments
with per-class `ModelClassSelection`, and the resolved `CameraDetection` bundle),
`class_names` JSON (de)serialization for the `model.class_names` column, and
`repo` — the model catalog (`upsert_model`/`list_models`), per-camera attachments
(`models_for`/`set_camera_models`, replace-all in one transaction), and
`detection_for`, the resolve query that joins a camera's attachments to their
filenames + class names for the runtime. Schema is migration **v8**
(`model` / `camera_model` / `camera_model_class`). ROI confinement rides on the
camera domain: `camera/area_geometry` (`point_in_polygon` / `inside_any_area`,
Qt/OpenCV-free, unit-tested) tests a detection's normalized box centre against a
camera's area polygons.

**Runtime** (`src/app/detection/`, OpenCV + a platform-split inference backend —
ONNX Runtime on Windows, native TensorRT on Jetson; app target only). The pure,
backend-free helpers form the `denso_detection` lib (linked by both `denso` and
`denso_tests`); the backend engines + registry + `model_sync` stay in `denso`.
Pure unit-tested helpers: `letterbox` (aspect-preserving resize +
gray pad to 640, plus the inverse box map), `yolo_decode` (two decoders chosen by
output shape — `decode_yolo` for the raw transposed `[1, 4+nc, na]` head via
per-anchor argmax + confidence floor + class-agnostic `cv::dnn::NMSBoxes`, and
`decode_yolo_end2end` for an NMS-free `[1, N, 6]` output where the model already
did NMS so only a confidence floor + inverse box map remain), and
`names_metadata` (parse the ONNX `names` dict). These feed the `InferenceEngine`
interface, implemented **per platform** (selected via the `BackendEngine` alias in
`engine_registry.h`):

- **Windows / dev — `OrtEngine`:** one ORT session with a **TensorRT → CUDA →
  CPU** provider fallback. The TensorRT tier runs FP16 with a serialized engine
  cache (`models/trt_cache/`); its first-run build is minutes-long and
  non-interruptible, so `EngineRegistry::warm_up()` runs one blank inference over
  every `models/*.onnx` on the warm-up worker to absorb it off the hot path.
- **Jetson Orin Nano (real target) — `TrtEngine`:** native TensorRT (`nvinfer` +
  `cudart`) that **deserializes a prebuilt `.engine` only** (built on-device with
  `trtexec` for TRT 10.3 / `sm_87`) — never built at runtime, **no fallback**. A
  missing/incompatible engine **fails loud** (throwing ctor → `WarmupWorker` →
  `app.exit(1)`). Class names come from a `<engine>.names.json` sidecar; inference
  is mutex-guarded across cameras. `warm_up()` scans `models/*.engine`.

  **Where the plan comes from, and how far it travels.** It is built with
  `trtexec` on the aarch64 build host and **shipped inside the `.deb`** — not
  rebuilt per appliance — so the bundle is qualified only for the supported
  configuration (Orin Nano / L4T R36.5.0 / TRT 10.3 / CUDA 12.6 / `sm_87`). A
  plan is a compiled artifact: matching `sm_87` is necessary, not sufficient.

  **The `[trt] Using an engine plan file across different models of devices`
  warning is non-diagnostic.** Measured 2026-07-21 on the build host, loading the
  engine that host built: the app prints it, and so does TensorRT's own
  `trtexec --loadEngine`, which then executes the plan at 140 qps / 7.48 ms mean
  / `PASSED`. Since it fires on the builder itself it cannot discriminate builder
  from target, so it is **not** evidence about any particular appliance — it was
  briefly mistaken for exactly that. The internal device-property comparison
  responsible is unidentified; treat it as a benign TRT-10.3-on-Orin artifact,
  actionable only alongside a real failure (deserialization error, CUDA error,
  wrong output) or a changed platform baseline.

  **Loading ≠ inferring.** `--check` builds the engine and validates bindings,
  shapes and class names, but never calls `infer()`; `warm_up()` runs the first
  real inference and discards its output. So neither proves *correct* detection
  — a newly commissioned appliance still wants one known-answer inference.

Each detection camera starts only after its models finish warming, so warm-up
never lands on a capture thread. `EngineRegistry` keeps one shared engine per
model filename (lazy; failed loads cached as `nullptr`). `model_sync` catalogs
`models/*.onnx` (Windows, names from ONNX metadata) or `models/*.engine` (Jetson,
names from the sidecar) at boot.

Streaming feeds these through a **capture-backend ladder** (`camera_stream.cpp`):
each source tries candidates and keeps the first that opens AND reads a frame —
RTSP via hardware **NVDEC** (`rtsp_gst_pipeline`: `nvv4l2decoder → nvvidconv →
BGR`, per-codec depay/parse) H.264 → H.265 → FFMPEG; USB MJPEG → YUYV → `CAP_ANY`.
Mixed-codec fleets auto-discover per camera; the leaky queue sits after the
decoder only. Inference is **decoupled from display** — `DetectionProcessor` runs
the model on a worker thread over a drop-oldest latest-frame slot and overlays the
newest detections snapshot, so video stays smooth regardless of model speed.

`CameraGrid` chooses per camera: it resolves `detection_for`, asks the registry
for each attached model, and constructs a `DetectionProcessor` (orient → infer →
per-class conf filter → ROI confinement → draw labelled boxes) when ≥1 model
loads, else a plain `OrientationProcessor`. `EngineRegistry::warm_up()` runs on
the warm-up worker thread in the background; `get()` is **mutex-guarded** because
it is now called from both that worker and the UI thread (`CameraGrid` starting a
camera whose models are ready — a cache-hit, never a build). Detection cameras are
gated on readiness (pure `warmup_gate` `PendingStart` + `WarmupState`), so the
engines outlive the capture streams and the build stays off them. ORT +
provider DLLs and every `models/*.onnx` are copied beside the exe by a
`POST_BUILD` step; the GPU provider DLLs come from the git-ignored
`third_party/gpu_ep/` (see `docs/GPU_SETUP.md`), and a missing GPU stack silently
degrades to the CPU provider.

## Model / operating-mode compatibility

Which model may be loaded, selected and attached in which operating mode. The
maintainer/operator contract — the matrix, the manifest format, every reason
code, how to add a model — is **`docs/MODEL_COMPATIBILITY.md`**. This section is
the *architecture*: the seams, the direction of the data flow, and why the pieces
are separated the way they are.

### Declaration vs corroboration vs authorization

Three questions, three owners, deliberately never merged:

| Question | Owner | Notes |
|---|---|---|
| *What is this artifact?* — **declaration** | schema-2 `manifest.json` | identity is declared, never inferred from a filename or class signature |
| *Is the artifact on disk actually that?* — **corroboration** | `models::resolve_model_metadata` | SHA-256 over the active backend's files, ordered class names/count, and (TensorRT only) the platform triple |
| *May that thing run in this mode?* — **authorization** | `models::model_compatibility` | one compiled policy, `src/core/models/compatibility.cpp` |

The separation is what makes the models directory safe to hand to an operator.
Because the manifest states only *what an artifact is* and carries no
`allowed_modes` field to parse into, editing a file there can **narrow** what is
authorized (a hash stops matching) but can never **widen** it. Widening requires
recompiling the registry.

The registry itself is two `constexpr` tables (family→modes, canonical id→family)
with compile-time invariants: every family allows ≥1 mode, **no family allows
both modes**, and every model's family is registered. `CompatibilityResult`
default-constructs to a rejection, so a path that forgets to assign a verdict
denies rather than permits.

### The `ManifestView` backend boundary

`ManifestView` (`src/core/models/model_identity.h`) binds a parsed manifest, a
models directory, and a **backend fixed at construction**. The production
constructor binds `active_backend()` — the one compile-time platform split that
decides which backend's manifest block is read, mirroring the `BackendEngine`
alias split in `engine_registry.h`; the explicit-`Backend` constructor is a test
seam so both platforms' resolution is exercised off-host.

This is why no enforcement API takes a `Backend` parameter. The backend is
resolved *away* at `resolve_model_metadata`, which yields a `ModelMetadata` for
the active backend only. `model_compatibility` therefore never sees a backend at
all and is a pure function of mode + identity — the property that makes a
TensorRT `built_for` mismatch structurally incapable of rejecting an ONNX Runtime
deployment (`built_for` lives only inside `runtime.tensorrt`, which the ONNX path
never reads).

Symmetrically, `denso_core` never probes the device: `PlatformInfo` is supplied by
the caller from the one application-layer provider,
`platform::measured_platform_info()`, which probes **and normalizes** exactly
once. A probe failure yields an *empty* `PlatformInfo`, never a substituted
constant — empty corroborates no `built_for`, so nothing is authorized.

### Central-policy data flow

```text
catalog row (DB)  ─┐
manifest.json  ────┼─> resolve_model_metadata ─> ModelMetadata ─> model_compatibility ─> verdict
PlatformInfo   ────┘   (per ACTIVE backend)      (identity +       (mode + identity)     + reason
                                                  corroboration)
```

Five production paths consume that verdict, and **all five call the same
function**:

1. **warm-up allow-list** — `loadable_model_files` → `EngineRegistry`'s
   `allow_list`;
2. **mode-filtered fail-loud required set** — `detection::attached_model_filenames`
   (`try_attached_model_filenames` for `--check`, which must distinguish "no
   attachments" from "query failed");
3. **selectable-model list** — `detection::selectable_models`;
4. **attachment write + runtime resolution** — `detection::set_camera_models` and
   `detection::detection_for`;
5. **boot / integrity** — `health::evaluate_integrity`, shared by GUI boot, the
   live grid and `--check`.

`mode`, `view` and `platform` are **non-defaulted** on every one of them, so a
forgotten call site fails to compile rather than silently authorizing. `startup.cpp`
resolves the three inputs **once** and reuses them for both the readiness verdict
and the warm-up firewall — the set of models the appliance may *load* and the set
it reports as *rejected* are then two readings of the same facts and cannot drift.

### The warm-up firewall

`EngineRegistry::warm_up()` scans the models directory and would otherwise
deserialize every runtime artifact it finds, attachment or not. The allow-list
turns that scan into a filter: a file not in it is **skipped** — never `get()`-ed,
never deserialized, never warmed — with one informational line carrying the
directory-entry name verbatim (a filesystem name, not a database column, so no
sanitizer is applied there; the DB-controlled diagnostic paths below do reduce).
`get()` called for a filename outside the allow-list **throws**: reaching it means
a caller bypassed the policy, and failing loud beats quietly running a rejected
plan. There is no default allow-list.

This is what makes an **idle wrong-mode artifact a normal state** rather than a
fault: declared, valid, unattached and wrong-mode, it is skipped by warm-up,
absent from the required set, produces no camera issue, and leaves the appliance
**Ready / exit 0**. A `digit_reader` appliance carries all three packaged model
pairs and loads only `digitv3`.

### Camera-scoped inhibition

A rejected **attached** model is a per-camera fault, never a whole-machine one.
`evaluate_integrity` emits `ZoneIssue{ModelCompatibilityRejected}` with the real
`camera_id` and the policy's verbatim reason → **Degraded / exit 10**, not
Blocked / 78. `CameraGrid` installs `ZoneCause::ModelUnavailable` before the
camera can publish, and `start_one` refuses the camera outright: no
`DetectionProcessor`, no `engines_->get()`, tile Offline, siblings unaffected.

Two design points that are easy to get wrong and are load-bearing here:

- **No new `ZoneCause` bit.** To a camera, "model rejected by policy" and "engine
  missing from disk" are the same thing — it has no usable model — so the existing
  `ModelUnavailable` bit is reused. The cause bitmask is a file format; the
  distinct diagnosis survives in the issue's reason code and detail.
- **All-or-nothing per camera.** `detection_for` returns an **empty** model set
  with `compatibility_rejected`, never the allowed subset, and `set_camera_models`
  refuses a mixed set inside the transaction. Running a camera on the surviving
  half of an attachment set the operator did not choose would silently change what
  that camera measures — and a demotion to orientation-only would look healthy
  while reading nothing.

### UI filtering

The unfiltered catalog (`detection::list_models`) is never rendered by a selection
surface; `selectable_models` is. It returns each allowed row **paired with its
resolved metadata** as one value (not two containers indexed in step), in catalog
order, rejected entries **absent** — not greyed, not annotated. Fail-closed: an
absent/schema-1/invalid manifest declares nothing, so the list is empty; it never
falls back to the raw catalog.

Absence creates one hazard the wizard must handle explicitly: a model a camera is
*attached* to but that is no longer offered cannot be carried by the page's
selection set, so a plain save would **detach** it — converting a diagnosable,
inhibited camera into an apparently-healthy camera with no model. It cannot simply
be preserved either, since `set_camera_models` refuses any set containing a
rejected model. So `CameraWizardController` recomputes the hidden set from the same
three inputs the page was given, names exactly what will be removed, and writes
nothing if the operator declines.

Mode always comes from the **committed** database value (`mode::load(db)`), never
the settings-page selector, which may hold an unapplied choice.

### Package artifact-placement sequence

The manifest is load-bearing — without it nothing is selectable or loadable — but
packaging cannot deliver it as an upgrade side effect: `postinst` is structural by
design (it creates the `/opt/denso/data/models` and `install-state` skeleton and
chmods the latter — no ownership, no artifact), and the `.deb` payload contains no
file under `/opt/denso/data`. Shipping enforcement and the manifest together would
install enforcing code onto an appliance with no manifest and inhibit every
camera. Hence the ordering:

```text
Release A   schema-2 support + a declaration for the EXISTING digitv3
            + seed-manifest + verify that changes nothing under models/
            (NOT globally read-only).             NO Float artifact.
   ↓        (no application behaviour change)
Gate A      upgrade rehearsal on the Jetson — blocking
   ↓
Release B   the policy, then the warm-up firewall, then camera-scoped
            enforcement, UI filtering, on-device validation …
   ↓
Slice 12    the FIRST package carrying Float artifacts
```

The firewall had to exist before Float placement, because a Float engine in the
models directory during Release A would have been deserialized and run on a Digital
Number Reader appliance at every boot. The ordering is **machine-enforced** by
`assert_float_seeding_guarded` (`packaging/lib/gen_payload.sh`), which refuses the
build if a `float-*` stem is approved without a real `loadable_model_files` and a
real call to it from `startup.cpp`.

Placement itself is two-stage and stays inside the existing ownership split: the
package stages the approved pairs into **`/opt/denso/models`** with the generated
manifest, `models.approved` and `SHA256SUMS` in `/opt/denso/lib`; `denso-setup
configure` seeds them pair-wise, as the target user, into the operator-owned
**`/opt/denso/data/models`**, which is where the app reads them.
`seed-manifest` places `manifest.json` and nothing else — explicit,
manifest-only, atomic, idempotent, canonical-equivalence-accepting, and never a
model replacement. `verify` only observes: no `--repair` exists and it changes
nothing under `models/` — though it is **not** globally read-only, since it
retains its pre-existing DB-backup write outside `models/`.

### Status and reason output

Both vocabularies reaching `status.json` are **file formats**: never rename, reuse
or renumber, only add.

- `health::reason_code(ZoneIssue::Kind)` → `model_compatibility_rejected` for a
  policy refusal, alongside the existing `engine_missing` /
  `engines_unmanifested`.
- The issue additionally carries `policy_reason` — the central policy's own code,
  verbatim, from the fixed first-failure-wins order (`model_undeclared` →
  `model_unknown_id` → `model_family_mismatch` → `model_shape_unsupported` →
  `model_classes_mismatch` → `model_provenance_failed` →
  `model_mode_incompatible`).

The kind is deliberately *not* named after the wrong-mode branch: it covers all
seven refusals, and naming it `ModelModeIncompatible` would make six other faults
describe themselves as a mode problem and send an operator hunting for a mode
switch that never happened. Only a genuine valid wrong-mode model reports
`model_mode_incompatible`. Diagnostics are redaction-safe: a database-controlled
filename is reduced by `models::diagnostic_filename` (a fail-closed allow-list) and
paired with the catalog row id, so an unprintable name is still identifiable.

### Current Ball Leveler lock

The compatibility layer for `ball_leveler` is complete — the mode persists, the
policy authorizes `float_ball` models for it, and the package ships their
artifacts. The **application feature is not implemented**: no production Leveler
wizard, `CameraStream`, `DetectionProcessor`, `ZoneHealth` wiring or reporter is
constructed, `mode_setup_required` stays permanently true, and no ball position,
percentage, calibration or final level-result algorithm exists anywhere in the
tree. Packaged, declared, authorizable artifacts are not a shipped feature;
unlocking it requires a new approved design and plan.

## Reading log

`src/core/reading/` is the append-only log of captured readings, Qt/OpenCV-free
like the rest of `denso_core`: `reading.h` (`Reading`: `id` / `camera_id` /
`ts_ms` / `value` / `conf`) + `repo` (`insert`, and `query(camera_id, from_ms,
to_ms)` ordered by `ts_ms` then `id`). Schema is migration **v9** — a `reading`
table indexed on `(camera_id, ts_ms)` for the by-camera time-range read. It's
append + range-read only; there is no update/delete, since a reading is an
immutable capture.

The write side is not wired up yet. `DetectionProcessor` (`frame_processor.h`)
has a dormant seam for it: an optional `ReadingSink*` (plus a `camera_id`) on
`DetectionProcessor`'s ctor — now 8 params (`degrees, pitch, roll, models, areas,
camera_id, sink, zone_sink`), all passed at its sole call site
(`ui/camera/grid/camera_grid.cpp:195-200`). When a sink is
set, `infer_loop()` calls `sink->on_reading(camera_id, ts_ms, kept)` with the
frame's post-ROI-confinement detections — **not** `process()`, which only submits
the latest frame and overlays the last snapshot. **Threading contract:**
`on_reading`
runs on the **inference worker thread** (`frame_processor.h:59`; the call site is
in `infer_loop()`) — *not* the capture thread, since inference is decoupled from
display. An implementation must not
block or do DB I/O inline; it has to hand the data off to a worker (e.g. via
`common::run_on_worker`/`post_to_gui`) and return immediately, the same rule
`camera_stream` already follows for its own frame processing. Assembling a
sink's kept detections into a `Reading::value` (e.g. digit-string reconstruction
across models) is deferred to the future logging/export feature (Spec 2) — this
task only lands the storage + the capture-thread hook, not a consumer.

## Brazing zone reporting

Pushes each ROI's number to a backend as one combined JSON POST on change.
Config lives in `src/core/brazing/config` (`BrazingConfig{enabled, base_url}` over
the `settings` key/value table); each ROI (`camera_area`) carries a `zone` number
(migration **v10**, nullable — NULL = ROI-only, not reported).

Pipeline, all off the GUI thread until the final POST:

```
capture thread (per camera)
  DetectionProcessor::process
    → submit latest frame to the worker's slot (drop-oldest), overlay snapshot

inference worker thread (per camera)
  DetectionProcessor::infer_loop
    → kept digit boxes (existing detection)
    → group_into_zones(kept, areas, w, h)   [pure: assemble_zone_value sorts
                                             digits left-to-right → int, per zone]
    → ZoneReporter::on_zones(cam_id, zones) [mutex] → ZoneAggregator::observe
         per-zone debounce (kStableFrames=5) + change detection      [pure]
         if a stable value changed: post_to_gui(reporter, submit(snapshot)) ─┐ queued
GUI thread                                                                  ▼
  BrazingReporter::submit → BrazingRetryPolicy decides →
     Send  → BrazingClient::post → POST {base}/api/brazing/update (async, 5 s timeout)
              → done(ok): 2xx → delivered; else arm retry QTimer (1s→×2→30s cap)
     ArmRetry → retry timer → re-send the latest pending snapshot
```

The four pure units (`zone_assembly`, `zone_aggregator`, `brazing_payload`,
`brazing_retry_policy`) are unit-tested; the structural pieces
(reporter/client/wiring) are build + suite + on-device verified. **Delivery is
reliable, latest-value-wins**: every POST carries the full `{zone_no→value}`
snapshot, and `BrazingReporter` keeps retrying the newest snapshot (single-flight,
exponential backoff) until the server 2xx-acks it — so a downed server no longer
drops the last value. New readings coalesce into the pending snapshot in real
time; retry state is in-memory only (no outbox, no idempotency, no persistence —
unlike the DeepStream sibling). Lifetime is safe by teardown order:
`CameraGrid::clear()` stops and **deletes** every `CameraStream` before resetting
`reporter_` then `brazing_reporter_`. Joining the capture threads is *not* what
makes this safe — capture threads never call the reporter. Deleting the stream
destroys the `unique_ptr<FrameProcessor>` it owns (`camera_stream.h:51`), and
`~DetectionProcessor` signals + joins its **inference worker**
(`frame_processor.cpp:39`) — the thread that actually calls `on_zones`. That join
is what guarantees no worker can reach the reporter afterwards. Each
in-flight POST's completion callback is `QPointer`-guarded, so a POST that finishes
after the reporter is torn down is dropped rather than calling into a destroyed
object (it does not rely on transitive `QNetworkAccessManager`/reply ownership).

## Logging

`src/app/logging/` is a **bounded, 24/7-safe** file logger installed as a
`qInstallMessageHandler` at the top of `main` (the Windows GUI subsystem has no
console, so `qDebug/qWarning/qCritical` would otherwise vanish). Design goal: a
box that runs for months must never fill the disk or lose the tail.

- `RotatingLogSink` (`log_sink`) writes to `denso.log` beside the exe and rolls
  at a size cap — **5 files × 5 MiB ≈ 25 MiB total**. Roll = close → shift
  `denso.log.N` → reopen, all under a mutex. Each record is truncated to
  ≤16 KiB, and `write()` is `noexcept` with a catch-all so a logging failure can
  never propagate into the caller. On a disk-full / write error it falls back to
  `stderr`, marks itself degraded, and keeps retrying the reopen.
- `log_rotation` is the **pure, unit-tested** size/roll policy (which file, when
  to roll, how to renumber) — no I/O, so it's testable off-device.
- `log_throttle` (`LogEpisode`) collapses a repeating condition (e.g. a flapping
  camera reconnecting) to a single "opened" + single "closed" line plus a
  periodic count, so a stuck fault can't flood the file.
- `redact` (`sanitize_url`) strips credentials from RTSP URLs before they reach
  the log.
- `qSetMessagePattern` bakes local ISO time + the (fixed-at-start) UTC offset +
  level + category + pid + tid into every line; `DENSO_LOG_LEVEL` sets the
  severity floor; a `SESSION` banner at startup and a 5-minute `HEARTBEAT` line
  mark liveness for after-the-fact log reading.

## Gotchas

- **`models/` is git-ignored by pattern** (`*.onnx`, `*.pt`, `*.engine`,
  `*.names.json`, `trt_cache/`) — they are build/training artifacts, device- and
  version-specific. **NOTHING under `models/` is tracked** — `denso.onnx` was the
  last exception (tracked before that rule, and gitignore cannot untrack an
  already-tracked file) until it was `git rm`'d with the digitv2 family, so a
  fresh clone has an EMPTY `models/`: see **Provisioning a model** in README.md.
  This USED to be a `git add -A` landmine (the
  ignore list named `models/digitv2.onnx` *specifically*, so every new model was
  sweepable); the pattern closed it. It also stopped a subtler bug: an unignored
  new model permanently dirties the tree, and `tools/build_package.sh` refuses to
  package a dirty tree — so dropping in `digitv3.onnx` silently blocked the build.
- **`ctest` does not cover `packaging/` or `tools/`** — they are shell, proven by
  `tests/packaging/run.sh`. Run it for any packaging change; a green `ctest` says
  nothing about them. `tests/manual/repro_build.sh` is stricter still: it refuses
  to start on a dirty tree, then makes and reverts its own edits to
  `packaging/lib/policy.sh`, so it must run **exclusively** — a concurrent edit to
  that one file is discarded by its restore.
- **QSQLITE keeps a read cursor alive until the `QSqlQuery` is finished or
  destroyed.** A live read cursor (e.g. an un-scoped `PRAGMA user_version`
  query) makes a later schema change on the same connection fail with
  `SQLITE_LOCKED` ("database table is locked"). `run_migrations` reads the
  version in its own scope so the cursor is released before any DDL — keep that
  pattern (finish/scope reads before writes); `rusqlite` finalized this for us,
  QSQLITE does not.
- Builds on the **MSYS2 UCRT64** toolchain (GCC + Qt6 from `pacman`): configure
  with `cmake -S . -B build -G Ninja` — its CMake finds Qt6 with no
  `CMAKE_PREFIX_PATH`. On that toolchain the MSVC `/utf-8` flag is a harmless
  no-op (GCC reads UTF-8 by default).
- MSVC needs `/utf-8` (set in the top-level CMake) so the UI's non-ASCII
  literals (`✕ … — 🔒`) reach the binary byte-for-byte (the sources are UTF-8
  without BOM).
- Linux disk sum over-counts loop/tmpfs/overlay mounts; sub-GB renders "0 GB"
  (embedded MB-range accepted). Verify on a real Linux device.
- `nmcli -t` SSID escaping (`\:`) and VLAN device names (`eth0:0`) are not yet
  handled — deferred to on-device validation.
- Platform backend tests are compiled per-OS, so the passing test count differs
  between Windows and Linux.
- Deferred UI parity nits from the port: the Network cards don't dim while
  loading (only the Refresh label changes); re-clicking the already-active
  Network nav item doesn't re-trigger a refresh (the Refresh button does).
- `denso.db` (+ `-wal`/`-shm`) is created in the data dir (`denso::paths`) at runtime and
  is git-ignored.
- **Frame-pacing duration units:** `CameraStream`'s display-rate cap sleeps in
  chunks computed in the clock's own (nanosecond) `steady_clock::duration`, NOT
  whole milliseconds. `duration_cast<milliseconds>` of a sub-millisecond
  remainder truncates to 0 → a `remaining -= 0` chunk loop busy-spins forever
  and wedges that capture thread. This froze the faster feeds in the live grid
  (the slow USB feed, whose per-frame time exceeded the interval, skipped the
  loop and was the only one that survived) — keep pacing math in the clock's
  duration. `qWarning()` is routed to a `denso.log` file beside the exe (GUI
  subsystem on Windows has no console); `cv::setNumThreads(0)` keeps OpenCV
  conversions inline since each camera already has its own thread.
- **MinGW `sleep_for` is pinned to the ~15.6 ms OS scheduler tick** and ignores
  `timeBeginPeriod`, so a 66 ms (15 fps) pace overshoots to ~100 ms and delivers
  ~9 fps. `CameraStream` sleeps via `precise_sleep`, a high-resolution waitable
  timer (`CREATE_WAITABLE_TIMER_HIGH_RESOLUTION`), falling back to `sleep_for`
  off-Windows or if the timer can't be created. The `thread_local` timer handle
  is held in a small RAII struct so its `CloseHandle` runs at thread exit — one
  kernel handle per capture thread was leaking on every grid reload before.
- **Never call `cap.set(CAP_PROP_FRAME_WIDTH/HEIGHT)` on a live GStreamer
  pipeline** — it reconfigures the pipeline caps and segfaults inside
  `gst_caps_new_simple`. The capture-resolution request is gated to USB devices;
  an RTSP camera dictates its own resolution, and any IP-side reframing belongs
  in the pipeline (`videoscale`), not `VideoCapture::set`.
- **The TensorRT EP's first-run engine build is minutes-long and
  non-interruptible.** It must be triggered from `EngineRegistry::warm_up()` on
  the warm-up worker thread — never lazily on a capture thread, which froze the UI
  and blocked stream `join()` on teardown (the reason TensorRT was dropped once
  before it was re-added behind the warm-up). Startup splits by whether that build
  is needed (`ui/startup_mode`): a **cold** start warms behind the blocking
  `StartupScreen` splash before the window (and any capture thread) exists; a
  **warm** restart is UI-first, creating each detection capture thread only after
  its models finish warming so `get()` there is a cache-hit. Later runs load the
  cached engine from `models/trt_cache/`.
- **IP-camera latency depends on the GStreamer decode plugins being installed.**
  Without them `cv::CAP_GSTREAMER` fails to open and `CameraStream` silently
  falls back to the buffering FFMPEG backend, and RTSP lag returns — install the
  `gst-plugins-{base,good,bad}` + `gst-libav` packages (see `CLAUDE.md`).
