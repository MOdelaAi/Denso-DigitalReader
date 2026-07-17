# Deployment Slice 2: Packaging & Appliance Integration — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Turn the built app into a `.deb` the operator `scp`s to a Jetson and installs with `apt`, after which it behaves like a normal Ubuntu application — menu entry, icon, launcher on PATH, listed in `dpkg -l`, upgradable, and starting unattended on power-on.

**Architecture:** dpkg owns distribution, dependencies, versioning, upgrade and removal. We add: a hand-rolled package tree built by `dpkg-deb --build`; **minimal, idempotent** maintainer scripts (the one place whose failure blocks future apt operations); and `denso-setup`, a separate root command carrying everything `apt install` cannot accept options for (target user, autostart, autologin, verification). All non-trivial policy lives in pure sourceable shell functions with their own test harness.

**Tech Stack:** dpkg-deb 1.21.1, bash, Debian policy, GDM3, XDG autostart. Package built **on the Jetson** (aarch64; no cross-toolchain exists).

## Global Constraints

Copied verbatim from `docs/superpowers/specs/2026-07-17-build-package-deployment-design.md`:

- **Install with apt, never `dpkg -i`** — `dpkg -i` does not resolve dependencies: `sudo apt install ./denso-digitalreader_<version>_arm64.deb`.
- CUDA, TensorRT, OpenCV and L4T come from JetPack and are **never** installed or upgraded by us.
- Apt-plan guard: run with `LC_ALL=C`; parse `Inst`, `Remv` and upgrade records; **abort on any removal**, unconditionally; abort on any plan touching `nvidia-l4t-*`, `cuda-*`, `libnvinfer*`, `libnvonnxparsers*`, `tensorrt*`; abort on **unexpected Ubuntu OpenCV installation** (its 4.5.4 must never displace NVIDIA's 4.8.0).
- `/usr/bin`, **not** `/usr/local/bin` — Debian policy reserves `/usr/local` for the local admin; a package must never install there.
- The launcher sets **no** `LD_LIBRARY_PATH` (verified: the exe is relocatable).
- Ship prebuilt engines; **no `trtexec` at install**. Model selection is **explicit**, never a directory glob — a tracked approval manifest lists stems + SHA-256; hash mismatch is a **hard failure**.
- **Anything touching `/opt/denso/data` must run as the target user, never root** — root-owned artifacts poison an operator-owned data dir.
- `--user` is explicit and required; **`$SUDO_USER` is not trusted**.
- Autologin records the **original pre-Denso values ONCE**, in root-owned `/opt/denso/install-state/`, **never rebased on upgrade**; restore only those keys, and only if they still hold the values we set.
- Maintainer scripts are **minimal, idempotent, and hard to fail** (dpkg may re-invoke them during recovery). `postinst` does structural work only — nothing that can reasonably fail after a successful build.
- **`prerm` proceeds ONLY on `--check-running` exit code exactly 1.** `0` (running), `4` (cannot determine) and anything unexpected must **refuse**.
- Model seeding: absent → seed; same hash → leave; **different hash → never overwrite silently**. Engine + sidecar are **ordered and crash-resistant, not atomic**: sidecar written+fsynced+renamed first, engine renamed **last** as the commit marker, then fsync the directory.
- `postrm purge` removes `/opt/denso/data` behind a **resolved-path guard**; plain `remove` keeps all data.
- Do **not** force `QT_QPA_PLATFORM=xcb` until the power-on autologin session has actually been observed (the greeter is X11; the *user* session type is unknown until exercised).
- Test names must be **ASCII** and never start with `--`.
- **Never `git add -A`** — `.gitignore` names `models/digitv2.onnx` specifically, not `models/*.onnx`.

**Exit codes — the contract Slice 1 shipped and this slice consumes:**

| Code | Meaning |
| --- | --- |
| 0 | ok — and for `--check-running`, **an instance IS running** |
| 1 | failed — and for `--check-running`, **definitely NOT running** |
| 2 | bad usage |
| 3 | GUI refused: another instance holds the lock |
| 4 | `--check-running` **cannot determine** (lock unusable) |

## Verified ground truth (do not re-derive)

| Fact | Evidence |
| --- | --- |
| Every dependency resolves on the target: `libqt6core6/gui6/widgets6/network6/sql6/sql6-sqlite/multimedia6` `6.2.4`, `qt6-qpa-plugins` `6.2.4`, `libopencv` `4.8.0-1-g6371ee1`, `cuda-cudart-12-6` `12.6.68-1`, `network-manager` `1.36.6`, `gstreamer1.0-libav` `1.20.3` | `apt-cache policy` on 192.168.1.15 |
| Real apt plan lines: `Inst cowsay (3.03+dfsg2-8 Ubuntu:22.04/jammy [all])`, `Conf …`, and `Remv …` | `LC_ALL=C apt-get -s install` |
| **`libopencv` ships NO `.shlibs`/`.symbols`; `dpkg-shlibdeps` errors: "no dependency information found for /usr/local/cuda/lib64/libcudart.so.12"** | `dpkg-shlibdeps -O` |
| Owning packages for the un-derivable libs: `libcudart.so.12` → **cuda-cudart-12-6**; `libopencv_*.so.408` → **libopencv** | `dpkg -S` |
| `lintian` is **NOT installed** on the Jetson | `which lintian` |
| `dpkg-deb` 1.21.1 present; `anydesk 8.0.4 arm64` installed as a `.deb` | `dpkg-deb --version`, `dpkg -l` |
| Jetson halts at the **GDM greeter** on power-on; `systemctl get-default` = `graphical.target`; greeter `Type=x11` | `loginctl` |
| `assets/icon.png` is force-tracked despite the `*.png` ignore rule | `.gitignore` |

## DEVIATION FROM THE SPEC — read before Task 3

The spec's D2 says `Depends: ${shlibs:Depends}` + a manual block. **That substvar cannot be produced here**: `dpkg-shlibdeps` hard-errors on `libcudart.so.12` (no dependency information) and `libopencv` has no shlibs metadata, so the only way to make it run is `--ignore-missing-info` — which the spec explicitly forbids because it drops deps silently while *looking* derived.

This plan therefore **declares `Depends:` in full, by hand, and has `build_package.sh` VERIFY it mechanically**: for every `NEEDED` soname in the built exe, resolve the library, find its owning package, and **fail the build** if that package is not covered by the declared `Depends:`. This keeps the spec's actual requirement ("the build must fail, never paper over") while dropping a mechanism that cannot work on this platform. Same guarantee, reachable implementation.

## File Structure

| File | Responsibility |
| --- | --- |
| `packaging/denso-digitalreader` **(new)** | Launcher: export `DENSO_DATA_DIR`, exec the real binary. No `LD_LIBRARY_PATH`. |
| `packaging/com.denso.DigitalReader.desktop` **(new)** | Menu entry; doubles as the autostart template. |
| `packaging/lib/policy.sh` **(new)** | Pure, sourceable policy functions — the only non-trivial logic, so the only thing worth testing. |
| `packaging/debian/control.in` **(new)** | Package metadata + the full hand-declared `Depends:`. `@VERSION@`/`@ARCH@` substituted at build. |
| `packaging/debian/{postinst,prerm,postrm}` **(new)** | Minimal idempotent lifecycle. |
| `packaging/denso-setup` **(new)** | `configure` / `verify` / `unconfigure` / `replace-model`. Everything needing options or judgement. |
| `packaging/models.approved` **(new)** | Tracked approval manifest: stem, SHA-256, trtexec recipe. |
| `tools/build_package.sh` **(new)** | Jetson-side: build → stage → verify deps → `dpkg-deb --build` → `.deb` + `.sha256` + MANIFEST. |
| `tests/packaging/run.sh` **(new)** | Dependency-free assert harness for `policy.sh`. |

---

### Task 1: Launcher, desktop entry, icon

**Files:**
- Create: `packaging/denso-digitalreader`, `packaging/com.denso.DigitalReader.desktop`
- Test: manual (`desktop-file-validate`)

**Interfaces:**
- Consumes: Slice 1's `$DENSO_DATA_DIR` contract.
- Produces: `/usr/bin/denso-digitalreader` (the single entry point the menu entry, the autostart entry, and any operator shell all use) and `com.denso.DigitalReader.desktop`.

- [ ] **Step 1: Write the launcher**

Create `packaging/denso-digitalreader`:

```sh
#!/bin/sh
# Launcher for the packaged app. The ONE entry point: the menu entry, the XDG
# autostart entry, and an operator shell all go through this, so the environment
# is identical however it starts.
#
# DENSO_DATA_DIR is why this exists: /opt/denso/bin is root-owned and dpkg
# REPLACES it on upgrade, so mutable state (denso.db, denso.log, models/) must
# live outside it or be destroyed on every upgrade.
#
# No LD_LIBRARY_PATH: verified unnecessary — the only RUNPATH in the exe is the
# absolute /usr/local/cuda/lib64, and /usr/local/lib is in the ld cache via
# ld.so.conf.d/libc.conf. Copying the exe out of the build tree resolves every
# library with zero "not found".
#
# QT_QPA_PLATFORM is deliberately NOT set: the GDM greeter is X11, but that does
# not prove the autologin USER session is Xorg. Pin it only after observing a
# real power-on session.
set -eu
DENSO_DATA_DIR="${DENSO_DATA_DIR:-/opt/denso/data}"
export DENSO_DATA_DIR
exec /opt/denso/bin/denso "$@"
```

- [ ] **Step 2: Write the desktop entry**

Create `packaging/com.denso.DigitalReader.desktop`:

```ini
[Desktop Entry]
Type=Application
Version=1.0
Name=Denso Digital Reader
Comment=Read 4-digit 7-segment displays from live cameras
Exec=/usr/bin/denso-digitalreader
Icon=denso-digitalreader
Terminal=false
Categories=Utility;
StartupNotify=true
```

`Icon=` is the **theme name**, not a path — it resolves to
`/usr/share/icons/hicolor/256x256/apps/denso-digitalreader.png`, which is where
Task 6 stages `assets/icon.png`. dpkg triggers refresh the icon cache and the
desktop database; we write no code for that.

- [ ] **Step 3: Validate**

```sh
chmod +x packaging/denso-digitalreader
sh -n packaging/denso-digitalreader && echo "launcher: syntax ok"
desktop-file-validate packaging/com.denso.DigitalReader.desktop && echo "desktop: valid"
```
Expected: both ok. If `desktop-file-validate` is absent locally, run this step on the Jetson (`sudo apt install desktop-file-utils`) — do not skip it; a malformed entry fails silently by simply never appearing in the menu.

- [ ] **Step 4: Commit**

```bash
git add packaging/denso-digitalreader packaging/com.denso.DigitalReader.desktop
git commit -m "feat(packaging): launcher + desktop entry

The launcher is the single entry point (menu, autostart, operator shell) so the
environment is identical however the app starts. It exports DENSO_DATA_DIR
because dpkg replaces /opt/denso/bin on upgrade; state kept there would die with
it. No LD_LIBRARY_PATH (the exe is relocatable) and no QT_QPA_PLATFORM (the
greeter is X11, but the autologin session type is still unobserved)."
```

---

### Task 2: `policy.sh` — the pure decisions, with a test harness

**Files:**
- Create: `packaging/lib/policy.sh`, `tests/packaging/run.sh`
- Test: `tests/packaging/run.sh`

**Interfaces:**
- Consumes: nothing.
- Produces (sourced by Tasks 4, 5, 6):
  - `version_ok <string>` → 0 if it is a safe path component / Debian version
  - `apt_plan_ok <plan-file>` → 0 if the plan is safe; prints the reason and returns 1 otherwise
  - `seed_decision <src> <dst>` → prints `seed` | `same` | `differs`
  - `gdm_set_autologin <conf> <user>` / `gdm_restore_autologin <conf> <orig-enable> <orig-user>` → edit only `[daemon]` keys
  - `install_pair <engine-src> <sidecar-src> <dst-dir>` → ordered, crash-resistant install

**Why these four and nothing else:** they are the only decisions in the slice that are *wrong-able* in a way tests can catch. Everything else is `install -m` and `dpkg-deb`.

- [ ] **Step 1: Write the failing tests**

Create `tests/packaging/run.sh`:

```sh
#!/usr/bin/env bash
# Dependency-free assert harness for packaging/lib/policy.sh. No bats, no pip —
# this must run on the Jetson and on the MSYS2 dev box unchanged.
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
. "$HERE/../../packaging/lib/policy.sh"

pass=0; fail=0
ok()   { echo "ok   - $1"; pass=$((pass+1)); }
bad()  { echo "FAIL - $1"; fail=$((fail+1)); }
is()   { if [ "$2" = "$3" ]; then ok "$1"; else bad "$1 (want '$3', got '$2')"; fi; }
rc_is(){ if [ "$2" = "$3" ]; then ok "$1"; else bad "$1 (want rc=$3, got rc=$2)"; fi; }

# ── version_ok: it becomes a FILESYSTEM PATH component, so anything clever is a
# security bug, not a style issue.
version_ok "0.1.0+g5a84491"; rc_is "version: accepts a real version" $? 0
version_ok "0.1.0";          rc_is "version: accepts a plain version" $? 0
version_ok "../../etc";      rc_is "version: rejects traversal" $? 1
version_ok "0.1.0; rm -rf /"; rc_is "version: rejects a shell metachar" $? 1
version_ok "0.1.0/../x";     rc_is "version: rejects a slash" $? 1
version_ok "";               rc_is "version: rejects empty" $? 1

# ── apt_plan_ok: the guard that keeps apt from eating the JetPack stack.
T=$(mktemp -d)
printf 'Inst cowsay (3.03+dfsg2-8 Ubuntu:22.04/jammy [all])\nConf cowsay (3.03+dfsg2-8 Ubuntu:22.04/jammy [all])\n' > "$T/plain"
apt_plan_ok "$T/plain" >/dev/null; rc_is "apt: a plain install is allowed" $? 0

printf 'Inst libqt6core6 (6.2.4 Ubuntu:22.04/jammy [arm64])\nRemv cowsay (3.03+dfsg2-8)\n' > "$T/remove"
apt_plan_ok "$T/remove" >/dev/null; rc_is "apt: ANY removal is refused" $? 1

printf 'Inst libnvinfer10 (10.3.0.30-1+cuda12.5 [arm64])\n' > "$T/trt"
apt_plan_ok "$T/trt" >/dev/null; rc_is "apt: touching libnvinfer* is refused" $? 1

printf 'Inst cuda-cudart-12-6 (12.6.68-1 [arm64])\n' > "$T/cuda"
apt_plan_ok "$T/cuda" >/dev/null; rc_is "apt: touching cuda-* is refused" $? 1

printf 'Inst nvidia-l4t-core (36.4.0 [arm64])\n' > "$T/l4t"
apt_plan_ok "$T/l4t" >/dev/null; rc_is "apt: touching nvidia-l4t-* is refused" $? 1

printf 'Inst libopencv-core4.5d (4.5.4+dfsg-9ubuntu4 Ubuntu:22.04/jammy [arm64])\n' > "$T/ubuntucv"
apt_plan_ok "$T/ubuntucv" >/dev/null; rc_is "apt: Ubuntu OpenCV is refused (must not displace NVIDIA 4.8)" $? 1

printf 'Inst libopencv (4.8.0-1-g6371ee1 [arm64])\n' > "$T/nvcv"
apt_plan_ok "$T/nvcv" >/dev/null; rc_is "apt: NVIDIA libopencv itself is NOT the Ubuntu one" $? 0

# ── seed_decision: never silently overwrite an operator's engine.
mkdir -p "$T/s" "$T/d"
printf 'aaa' > "$T/s/m.engine"
is "seed: absent at destination -> seed" "$(seed_decision "$T/s/m.engine" "$T/d/m.engine")" "seed"
printf 'aaa' > "$T/d/m.engine"
is "seed: identical -> leave it alone" "$(seed_decision "$T/s/m.engine" "$T/d/m.engine")" "same"
printf 'bbb' > "$T/d/m.engine"
is "seed: DIFFERENT -> never overwrite silently" "$(seed_decision "$T/s/m.engine" "$T/d/m.engine")" "differs"

# ── install_pair: the engine must appear LAST, so a newly visible engine always
# has its sidecar (power can fail between two renames — this is ordering, not
# atomicity).
rm -rf "$T/p"; mkdir -p "$T/p/src" "$T/p/dst"
printf 'ENGINE' > "$T/p/src/m.engine"; printf '{"0":"a"}' > "$T/p/src/m.names.json"
install_pair "$T/p/src/m.engine" "$T/p/src/m.names.json" "$T/p/dst"
rc_is "pair: installs" $? 0
[ -f "$T/p/dst/m.engine" ] && [ -f "$T/p/dst/m.names.json" ] && ok "pair: both landed" || bad "pair: both landed"
is "pair: engine content intact" "$(cat "$T/p/dst/m.engine")" "ENGINE"

# ── gdm_set_autologin: edit ONLY [daemon] keys; never template the file.
cat > "$T/gdm.conf" <<'EOF'
# GDM configuration storage
[daemon]
# Uncomment the line below to force the login screen to use Xorg
#WaylandEnable=false

[security]
AllowRoot=true

[xdmcp]
EOF
gdm_set_autologin "$T/gdm.conf" modela
rc_is "gdm: set succeeds" $? 0
grep -q '^AutomaticLoginEnable=true$' "$T/gdm.conf" && ok "gdm: enable set" || bad "gdm: enable set"
grep -q '^AutomaticLogin=modela$' "$T/gdm.conf" && ok "gdm: user set" || bad "gdm: user set"
grep -q '^AllowRoot=true$' "$T/gdm.conf" && ok "gdm: OTHER sections untouched" || bad "gdm: OTHER sections untouched"
grep -q '^\[xdmcp\]$' "$T/gdm.conf" && ok "gdm: later sections survive" || bad "gdm: later sections survive"
is "gdm: the key lands INSIDE [daemon]" \
   "$(awk '/^\[daemon\]/{d=1;next} /^\[/{d=0} d&&/^AutomaticLogin=modela$/{print "yes"}' "$T/gdm.conf")" "yes"

# Idempotent: a second set must not duplicate the keys (dpkg re-invokes scripts).
gdm_set_autologin "$T/gdm.conf" modela
is "gdm: idempotent (no duplicate keys)" "$(grep -c '^AutomaticLogin=' "$T/gdm.conf")" "1"

# Restore puts back ONLY what we changed, and only if it still holds our value.
gdm_restore_autologin "$T/gdm.conf" "false" ""
rc_is "gdm: restore succeeds" $? 0
grep -q '^AutomaticLoginEnable=false$' "$T/gdm.conf" && ok "gdm: enable restored" || bad "gdm: enable restored"

# If an admin changed the user AFTER us, restore must refuse rather than clobber.
gdm_set_autologin "$T/gdm.conf" modela
sed -i 's/^AutomaticLogin=modela$/AutomaticLogin=someoneelse/' "$T/gdm.conf"
gdm_restore_autologin "$T/gdm.conf" "false" ""
rc_is "gdm: refuses to clobber an admin's later change" $? 1
grep -q '^AutomaticLogin=someoneelse$' "$T/gdm.conf" && ok "gdm: admin's value survives" || bad "gdm: admin's value survives"

rm -rf "$T"
echo; echo "passed: $pass   failed: $fail"
[ "$fail" -eq 0 ]
```

- [ ] **Step 2: Run to verify it fails**

```sh
chmod +x tests/packaging/run.sh && ./tests/packaging/run.sh
```
Expected: FAIL — `packaging/lib/policy.sh: No such file or directory`.

- [ ] **Step 3: Write the implementation**

Create `packaging/lib/policy.sh`:

```sh
# Pure, sourceable policy decisions for the Denso packaging.
#
# Everything here is a decision that can be WRONG in a way a test can catch.
# Mechanics (install -m, dpkg-deb, apt) live in the callers. Source this; it
# defines functions and touches no global state.
#
# POSIX sh — the maintainer scripts run under /bin/sh, not bash.

# --- version_ok <string> -----------------------------------------------------
# The version becomes a FILESYSTEM PATH component and a dpkg field. Anything
# outside a strict allowlist is a security bug, not a style question.
version_ok() {
    case "${1-}" in
        "") return 1 ;;
        *[!A-Za-z0-9.+~-]*) return 1 ;;   # allowlist: Debian version chars only
        *) : ;;
    esac
    # No leading dash (would parse as an option), no path traversal.
    case "$1" in
        -*|*..*) return 1 ;;
    esac
    return 0
}

# --- apt_plan_ok <plan-file> -------------------------------------------------
# Reads `LC_ALL=C apt-get -s install ...` output. A Depends: declaration does
# NOT stop apt from removing or replacing a protected package to satisfy
# constraints — that is what this guard is for.
#
# Real line shapes:
#   Inst cowsay (3.03+dfsg2-8 Ubuntu:22.04/jammy [all])
#   Remv somepkg [1.2-3]
apt_plan_ok() {
    plan="${1-}"
    [ -r "$plan" ] || { echo "apt-plan: cannot read plan file: $plan" >&2; return 1; }

    # ANY removal is refused, unconditionally. We are an add-on package; nothing
    # we install has any business removing something already on the appliance.
    if grep -q '^Remv ' "$plan"; then
        echo "apt-plan: REFUSED — the plan removes packages:" >&2
        grep '^Remv ' "$plan" >&2
        return 1
    fi

    # Protected families: the JetPack stack. Matching on the exact package name
    # field (2nd token), never a substring of the whole line — a version string
    # containing "cuda12.5" must not trigger this.
    while read -r verb pkg _rest; do
        case "$verb" in Inst|Conf) : ;; *) continue ;; esac
        case "$pkg" in
            nvidia-l4t-*|cuda-*|libnvinfer*|libnvonnxparsers*|tensorrt*)
                echo "apt-plan: REFUSED — the plan touches a protected JetPack package: $pkg" >&2
                return 1 ;;
        esac
        # Ubuntu's OpenCV (libopencv-*4.5d, libopencv-dev from the Ubuntu repo)
        # must never displace NVIDIA's /usr/local 4.8.0 build, which is the one
        # the app is linked against. NVIDIA's own package is named exactly
        # "libopencv", so match the Ubuntu-style names only.
        case "$pkg" in
            libopencv-*)
                echo "apt-plan: REFUSED — the plan installs Ubuntu OpenCV ($pkg); NVIDIA's libopencv 4.8 must not be displaced" >&2
                return 1 ;;
        esac
    done < "$plan"
    return 0
}

# --- seed_decision <src> <dst> ----------------------------------------------
# absent -> seed | identical -> same | different -> differs (NEVER silently
# overwrite: the operator may have installed their own engine).
seed_decision() {
    src="${1-}"; dst="${2-}"
    if [ ! -e "$dst" ]; then echo "seed"; return 0; fi
    if [ "$(sha256sum "$src" | cut -d' ' -f1)" = "$(sha256sum "$dst" | cut -d' ' -f1)" ]; then
        echo "same"
    else
        echo "differs"
    fi
}

# --- install_pair <engine-src> <sidecar-src> <dst-dir> -----------------------
# Ordered and crash-resistant, NOT atomic: two flat files cannot be made atomic
# with two renames. The ENGINE's appearance is the commit marker — sidecar first,
# engine last — so a newly visible engine always has its sidecar. (TrtEngine's
# ctor reads <stem>.names.json and throws without it, so the reverse order could
# strand a loadable-looking engine with no names.)
install_pair() {
    eng="${1-}"; side="${2-}"; dst="${3-}"
    [ -f "$eng" ] && [ -f "$side" ] && [ -d "$dst" ] || return 1
    stem="$(basename "$eng" .engine)"

    cp "$side" "$dst/.$stem.names.json.tmp" || return 1
    sync
    mv "$dst/.$stem.names.json.tmp" "$dst/$stem.names.json" || return 1

    cp "$eng" "$dst/.$stem.engine.tmp" || return 1
    sync
    mv "$dst/.$stem.engine.tmp" "$dst/$stem.engine" || return 1

    sync
    return 0
}

# --- gdm_set_autologin <conf> <user> ----------------------------------------
# Edits ONLY the [daemon] section's two keys. Never templates the file: GDM's
# config carries admin settings we must not touch. Idempotent — dpkg may
# re-invoke a maintainer script during recovery.
gdm_set_autologin() {
    conf="${1-}"; user="${2-}"
    [ -w "$conf" ] || return 1
    [ -n "$user" ] || return 1
    tmp="$conf.denso.tmp"
    awk -v user="$user" '
        BEGIN { in_daemon=0; done_enable=0; done_user=0 }
        /^\[daemon\]/ {
            print; in_daemon=1; next
        }
        /^\[/ && !/^\[daemon\]/ {
            # Leaving [daemon]: emit anything we did not find inside it.
            if (in_daemon) {
                if (!done_enable) { print "AutomaticLoginEnable=true"; done_enable=1 }
                if (!done_user)   { print "AutomaticLogin=" user;      done_user=1 }
                in_daemon=0
            }
            print; next
        }
        in_daemon && /^[#[:space:]]*AutomaticLoginEnable[[:space:]]*=/ {
            if (!done_enable) { print "AutomaticLoginEnable=true"; done_enable=1 }
            next
        }
        in_daemon && /^[#[:space:]]*AutomaticLogin[[:space:]]*=/ {
            if (!done_user) { print "AutomaticLogin=" user; done_user=1 }
            next
        }
        { print }
        END {
            if (in_daemon) {
                if (!done_enable) print "AutomaticLoginEnable=true"
                if (!done_user)   print "AutomaticLogin=" user
            }
        }
    ' "$conf" > "$tmp" || { rm -f "$tmp"; return 1; }
    cat "$tmp" > "$conf" || { rm -f "$tmp"; return 1; }   # preserve mode/owner
    rm -f "$tmp"
    return 0
}

# --- gdm_restore_autologin <conf> <orig-enable> <orig-user> ------------------
# Restores ONLY our two keys, and ONLY if they still hold what we set. If an
# admin changed them afterwards, refuse — a blind restore would silently revert
# their intent.
gdm_restore_autologin() {
    conf="${1-}"; orig_enable="${2-}"; orig_user="${3-}"
    [ -w "$conf" ] || return 1

    cur_user="$(awk '/^\[daemon\]/{d=1;next} /^\[/{d=0} d&&/^AutomaticLogin=/{sub(/^AutomaticLogin=/,"");print;exit}' "$conf")"
    denso_user="$(cat /opt/denso/install-state/autologin.user 2>/dev/null || echo "$cur_user")"
    if [ -n "$cur_user" ] && [ "$cur_user" != "$denso_user" ]; then
        echo "gdm: refusing to restore — AutomaticLogin is now '$cur_user', not the '$denso_user' we set" >&2
        return 1
    fi

    tmp="$conf.denso.tmp"
    awk -v en="$orig_enable" -v us="$orig_user" '
        BEGIN { in_daemon=0 }
        /^\[daemon\]/ { print; in_daemon=1; next }
        /^\[/ && !/^\[daemon\]/ { in_daemon=0; print; next }
        in_daemon && /^AutomaticLoginEnable[[:space:]]*=/ {
            if (en != "") print "AutomaticLoginEnable=" en
            next
        }
        in_daemon && /^AutomaticLogin[[:space:]]*=/ {
            if (us != "") print "AutomaticLogin=" us
            next
        }
        { print }
    ' "$conf" > "$tmp" || { rm -f "$tmp"; return 1; }
    cat "$tmp" > "$conf" || { rm -f "$tmp"; return 1; }
    rm -f "$tmp"
    return 0
}
```

- [ ] **Step 4: Run to verify it passes**

```sh
./tests/packaging/run.sh; echo "harness exit = $?"
```
Expected: every line `ok`, `failed: 0`, exit 0.

Run it on the Jetson too — `awk`/`grep` differ between MSYS2 and Ubuntu, and this code ships to Ubuntu:
```sh
scp -r packaging tests/packaging modela@192.168.1.15:/tmp/pol/ && \
  ssh modela@192.168.1.15 'cd /tmp/pol && ./packaging/../tests/packaging/run.sh'
```
(If the relative source path in the harness fights that layout, run it from a full checkout on the Jetson at Task 7 instead — but do not skip Ubuntu verification of `gdm_set_autologin`.)

- [ ] **Step 5: Commit**

```bash
git add packaging/lib/policy.sh tests/packaging/run.sh
git commit -m "feat(packaging): pure policy functions + a dependency-free harness

These four decisions are the only wrong-able logic in the slice, so they are the
only things worth testing: the version allowlist (it becomes a path component),
the apt-plan guard (a Depends: declaration does NOT stop apt from removing a
protected JetPack package), the seeding decision (never silently overwrite an
operator's engine), and the GDM [daemon]-only edit (never template a file
carrying admin settings; refuse to clobber a later admin change).

install_pair puts the ENGINE last as the commit marker: two flat files can't be
made atomic with two renames, so a newly visible engine always having its
sidecar is the guarantee we can actually offer."
```

---

### Task 3: `debian/control.in` + the mechanical dependency check

**Files:**
- Create: `packaging/debian/control.in`
- Test: exercised by Task 6's `build_package.sh` on the Jetson

**Interfaces:**
- Consumes: nothing.
- Produces: `packaging/debian/control.in` with `@VERSION@` / `@ARCH@` placeholders; Task 6 substitutes and verifies.

**Read the DEVIATION section above first** — `${shlibs:Depends}` is not achievable here, so `Depends:` is hand-declared and `build_package.sh` verifies it mechanically.

- [ ] **Step 1: Write the control template**

Create `packaging/debian/control.in`:

```
Package: denso-digitalreader
Version: @VERSION@
Architecture: @ARCH@
Maintainer: Modela AI <modela.common@gmail.com>
Section: utils
Priority: optional
Depends: libqt6core6 (>= 6.2.4),
         libqt6gui6 (>= 6.2.4),
         libqt6widgets6 (>= 6.2.4),
         libqt6network6 (>= 6.2.4),
         libqt6sql6 (>= 6.2.4),
         libqt6sql6-sqlite (>= 6.2.4),
         libqt6multimedia6 (>= 6.2.4),
         qt6-qpa-plugins (>= 6.2.4),
         libopencv (>= 4.8.0),
         cuda-cudart-12-6,
         gstreamer1.0-plugins-base,
         gstreamer1.0-plugins-good,
         gstreamer1.0-plugins-bad,
         gstreamer1.0-libav,
         network-manager
Description: Denso Digital Reader
 Reads 4-digit 7-segment displays from live cameras and reports each ROI's
 value to a backend. Qt Widgets GUI with per-camera TensorRT detection.
 .
 Requires a JetPack 6.2 (L4T R36.5) Jetson: CUDA 12.6, TensorRT 10.3 and
 NVIDIA's OpenCV 4.8 come from the JetPack image and are never installed or
 upgraded by this package.
```

Why each non-obvious entry:
- `libopencv (>= 4.8.0)` and `cuda-cudart-12-6` — **hand-declared because they cannot be derived**: `libopencv` ships no `.shlibs`/`.symbols` and `dpkg-shlibdeps` hard-errors on `libcudart.so.12`.
- `qt6-qpa-plugins`, `libqt6sql6-sqlite`, the `gstreamer1.0-*` set — **dlopened**, so no dependency scanner can ever see them: the xcb platform plugin, the Qt SQLite driver, and the GStreamer elements the capture ladder builds by name.
- `network-manager` — the app shells out to `nmcli`.
- **`libnvinfer10` is deliberately ABSENT** even though the exe links it: it is `cuda-*`/`libnvinfer*` family, i.e. JetPack-owned and on the protected list. Declaring a versioned dep on it invites apt to "satisfy" it by touching the JetPack stack — the exact thing the guard forbids. Task 6's check must special-case it: present on the box, never declared. **State this in the check's output so the exemption is visible, not silent.**

- [ ] **Step 2: Sanity-check the template**

```sh
grep -c '@VERSION@\|@ARCH@' packaging/debian/control.in   # expect 2
awk '/^Depends:/,/^Description:/' packaging/debian/control.in | grep -c ','  # expect 14
```

- [ ] **Step 3: Commit**

```bash
git add packaging/debian/control.in
git commit -m "feat(packaging): debian control template with a hand-declared Depends

DEVIATION from the spec's \${shlibs:Depends}, and why: dpkg-shlibdeps hard-errors
on libcudart.so.12 ('no dependency information found') and libopencv ships no
shlibs/symbols, so the substvar can only be produced with --ignore-missing-info
-- which the spec forbids precisely because it drops deps silently while looking
derived. Depends: is therefore declared in full and build_package.sh VERIFIES it
mechanically (every NEEDED soname must map to a declared package, or the build
fails). Same guarantee the spec asked for, via a mechanism that can actually run
on this platform.

libnvinfer10 is deliberately NOT declared despite being linked: it is
JetPack-owned (protected family), and a versioned dep on it invites apt to touch
the JetPack stack."
```

---

### Task 4: Maintainer scripts

**Files:**
- Create: `packaging/debian/postinst`, `packaging/debian/prerm`, `packaging/debian/postrm`

**Interfaces:**
- Consumes: `denso --check-running` (Slice 1; exit 0/1/4) and `/usr/bin/denso-setup unconfigure` (Task 5). These scripts do **not** source `policy.sh` — seeding is `denso-setup`'s job, not `postinst`'s.
- Produces: the dpkg lifecycle (`prerm` refusal, `postinst` structure, `postrm purge`).

**These scripts are deliberately boring.** A failing `postinst` leaves the package unconfigured and **blocks every later apt operation**, so it does structural work only — nothing that can reasonably fail after a successful build. All judgement lives in `denso-setup` (Task 5).

- [ ] **Step 1: Write `prerm`**

Create `packaging/debian/prerm`:

```sh
#!/bin/sh
# Refuse to upgrade or remove while an instance is running. Failing here leaves
# the OLD package installed — exactly what we want.
set -e

TARGET_USER_FILE=/opt/denso/install-state/user
DENSO=/opt/denso/bin/denso

# Revert autostart/autologin HERE, while our files still exist: on `remove`/
# `purge` dpkg deletes /usr/bin/denso-setup before postrm runs, so postrm cannot
# do it. `upgrade` must NOT revert — the new version keeps the same setup.
case "$1" in
  remove)
    if [ -x /usr/bin/denso-setup ]; then
        /usr/bin/denso-setup unconfigure || true   # never block a removal
    fi
    ;;
esac

if [ -x "$DENSO" ] && [ -r "$TARGET_USER_FILE" ]; then
    user="$(cat "$TARGET_USER_FILE")"
    # --check-running is TRI-STATE and MUST be read as such:
    #   0 = running            -> refuse
    #   1 = definitely not     -> proceed          <-- the ONLY safe-to-proceed code
    #   4 = cannot determine   -> refuse
    # `if denso --check-running; then refuse; fi` would be WRONG: it proceeds on
    # BOTH 1 and 4, i.e. it treats "couldn't tell" as "safe", which is exactly
    # the unsafe upgrade (replacing a release under a live app) the tri-state
    # exists to prevent.
    #
    # Run as the target user: --check-running is the one mode that takes the lock
    # (answering requires tryLock), and as root it would leave a root-owned lock
    # artifact in an operator-owned data dir — after which every later check
    # returns 4.
    set +e
    runuser -u "$user" -- env DENSO_DATA_DIR=/opt/denso/data "$DENSO" --check-running >/dev/null 2>&1
    rc=$?
    set -e
    case "$rc" in
        1) : ;;  # definitely not running — proceed
        0) echo "denso: the application is running; close it before upgrading or removing." >&2
           exit 1 ;;
        *) echo "denso: cannot establish whether the application is running (rc=$rc); refusing." >&2
           echo "denso: check /opt/denso/data ownership and permissions." >&2
           exit 1 ;;
    esac
fi

#DEBHELPER#
exit 0
```

- [ ] **Step 2: Write `postinst`**

Create `packaging/debian/postinst`:

```sh
#!/bin/sh
# Structural only, and idempotent: dpkg may re-invoke this during recovery, and
# a failure here leaves the package unconfigured and blocks later apt runs.
# Everything with judgement (target user, autostart, autologin, validation)
# lives in `denso-setup`, which the operator runs explicitly afterwards.
set -e

case "$1" in
  configure)
    DATA=/opt/denso/data
    STATE=/opt/denso/install-state

    mkdir -p "$DATA/models" "$STATE"
    chmod 0755 "$STATE"

    # If denso-setup has already recorded a target user (i.e. this is an
    # UPGRADE), keep the data dir owned by them. On a FIRST install nobody has
    # chosen a user yet, so leave ownership alone — `denso-setup configure
    # --user <u>` sets it. Guessing here (e.g. from SUDO_USER) is exactly the
    # trust mistake the spec forbids.
    if [ -r "$STATE/user" ]; then
        user="$(cat "$STATE/user")"
        if id "$user" >/dev/null 2>&1; then
            chown -R "$user":"$user" "$DATA" || true
        fi
    fi

    echo "denso: installed. Next:"
    echo "  sudo denso-setup configure --user <username> [--autostart] [--enable-autologin]"
    echo "  denso-setup verify"
    ;;
  abort-upgrade|abort-remove|abort-deconfigure)
    ;;
esac

#DEBHELPER#
exit 0
```

- [ ] **Step 3: Write `postrm`**

Create `packaging/debian/postrm`:

```sh
#!/bin/sh
# `remove` keeps ALL operator data. Only `purge` deletes it, behind a
# resolved-path guard.
set -e

case "$1" in
  purge)
    DATA=/opt/denso/data
    STATE=/opt/denso/install-state

    # NOTE: autologin/autostart are reverted in `prerm remove`, NOT here. By the
    # time postrm purge runs, dpkg has already deleted /usr/bin/denso-setup and
    # /opt/denso/lib/policy.sh — calling them here would silently do nothing and
    # leave the box auto-logging-in forever after a purge.

    # Resolved-path guard: refuse to delete anything that is not EXACTLY the
    # canonical data dir (a symlinked /opt/denso/data must not turn `purge` into
    # rm -rf of somewhere else).
    if [ -d "$DATA" ]; then
        real="$(readlink -f "$DATA" 2>/dev/null || echo "")"
        if [ "$real" = "/opt/denso/data" ]; then
            echo "denso: purging $DATA"
            rm -rf "$DATA"
        else
            echo "denso: NOT purging $DATA — it resolves to '$real', not /opt/denso/data" >&2
        fi
    fi
    rm -rf "$STATE"
    rmdir /opt/denso 2>/dev/null || true
    ;;
  remove|upgrade|failed-upgrade|abort-install|abort-upgrade|disappear)
    ;;
esac

#DEBHELPER#
exit 0
```

- [ ] **Step 4: Verify syntax**

```sh
for f in packaging/debian/postinst packaging/debian/prerm packaging/debian/postrm; do
  sh -n "$f" && echo "$f: syntax ok" || echo "$f: SYNTAX ERROR"
done
```
Expected: three `syntax ok`.

- [ ] **Step 5: Commit**

```bash
git add packaging/debian/postinst packaging/debian/prerm packaging/debian/postrm
git commit -m "feat(packaging): minimal idempotent maintainer scripts

Deliberately boring: a failing postinst leaves the package unconfigured and
blocks every later apt operation, so it does structural work only. All judgement
lives in denso-setup.

prerm reads --check-running as the TRI-STATE it is and proceeds ONLY on exit 1.
The obvious 'if denso --check-running; then refuse; fi' is wrong -- it proceeds
on both 1 (not running) and 4 (couldn't tell), treating 'couldn't tell' as safe,
which is the unsafe upgrade the tri-state exists to prevent. It runs as the
target user because --check-running takes the lock, and a root-owned lock
artifact would make every later check return 4.

postinst does NOT guess the target user from SUDO_USER (the spec forbids trusting
it); first install leaves ownership to denso-setup, upgrades reuse the recorded
user. postrm purges only behind a resolved-path guard."
```

---

### Task 5: `denso-setup`

**Files:**
- Create: `packaging/denso-setup`

**Interfaces:**
- Consumes: `policy.sh` (`apt_plan_ok`, `gdm_set_autologin`, `gdm_restore_autologin`, `seed_decision`, `install_pair`); `denso --check`, `--check-migrations` (Slice 1).
- Produces: `/usr/bin/denso-setup` with `configure --user <u> [--autostart] [--enable-autologin]`, `verify`, `unconfigure`, `replace-model <stem>`.

**Why this exists separately from `apt install`:** a maintainer script cannot accept `--user`/`--autostart` options, and interactive prompts in one are wrong. So configuration is an explicit second command the operator runs.

- [ ] **Step 1: Write it**

Create `packaging/denso-setup`:

```sh
#!/bin/sh
# Denso appliance setup: everything `apt install` cannot do.
#
#   denso-setup configure --user <u> [--autostart] [--enable-autologin]
#   denso-setup verify
#   denso-setup unconfigure
#   denso-setup replace-model <stem>
set -eu

. /opt/denso/lib/policy.sh

DENSO=/opt/denso/bin/denso
DATA=/opt/denso/data
STATE=/opt/denso/install-state
PKG_MODELS=/opt/denso/models
GDM_CONF=/etc/gdm3/custom.conf
AUTOSTART_NAME=com.denso.DigitalReader.desktop

die() { echo "denso-setup: $*" >&2; exit 1; }
need_root() { [ "$(id -u)" = "0" ] || die "must run as root (sudo denso-setup ...)"; }

# Run the app as the target user, never root: anything touching $DATA as root
# leaves root-owned artifacts (trt_cache, log, lock) that the app then cannot
# write — and a root-owned lock makes every --check-running return 4.
as_user() {
    u="$1"; shift
    runuser -u "$u" -- env DENSO_DATA_DIR="$DATA" "$@"
}

cmd_configure() {
    need_root
    user=""; autostart=0; autologin=0
    while [ $# -gt 0 ]; do
        case "$1" in
            --user) user="${2-}"; shift 2 ;;
            --autostart) autostart=1; shift ;;
            --enable-autologin) autologin=1; shift ;;
            *) die "unknown option: $1" ;;
        esac
    done
    # $SUDO_USER is NOT trusted: absent under automation, wrong from a root shell.
    [ -n "$user" ] || die "--user <username> is required"
    id "$user" >/dev/null 2>&1 || die "no such user: $user"
    home="$(getent passwd "$user" | cut -d: -f6)"
    [ -n "$home" ] && [ -d "$home" ] || die "user $user has no valid home directory"

    mkdir -p "$STATE"
    printf '%s\n' "$user" > "$STATE/user"

    mkdir -p "$DATA/models"
    chown -R "$user":"$user" "$DATA"
    echo "denso-setup: data dir $DATA owned by $user"

    # Seed the packaged engines. Absent -> seed; identical -> leave; DIFFERENT ->
    # refuse (the operator may have built their own).
    for eng in "$PKG_MODELS"/*.engine; do
        [ -e "$eng" ] || continue
        stem="$(basename "$eng" .engine)"
        side="$PKG_MODELS/$stem.names.json"
        [ -f "$side" ] || die "packaged engine $stem.engine has no $stem.names.json sidecar"
        case "$(seed_decision "$eng" "$DATA/models/$stem.engine")" in
            seed)
                install_pair "$eng" "$side" "$DATA/models" || die "failed to seed $stem"
                chown "$user":"$user" "$DATA/models/$stem.engine" "$DATA/models/$stem.names.json"
                echo "denso-setup: seeded $stem.engine" ;;
            same)    echo "denso-setup: $stem.engine already present (identical)" ;;
            differs) echo "denso-setup: $stem.engine present but DIFFERENT — keeping yours."
                     echo "             run 'denso-setup replace-model $stem' to overwrite." ;;
        esac
    done

    if [ "$autostart" = "1" ]; then
        dir="$home/.config/autostart"
        mkdir -p "$dir"
        cp "/usr/share/applications/$AUTOSTART_NAME" "$dir/$AUTOSTART_NAME"
        chown -R "$user":"$user" "$home/.config/autostart"
        echo "denso-setup: autostart enabled for $user ($dir/$AUTOSTART_NAME)"
        echo "denso-setup: NOTE — XDG autostart only fires AFTER a graphical login."
        echo "             Without --enable-autologin this box still stops at the"
        echo "             GDM greeter on power-on and the app will NOT start."
    fi

    if [ "$autologin" = "1" ]; then
        [ -f "$GDM_CONF" ] || die "$GDM_CONF not found — is GDM the display manager?"
        [ "$(systemctl get-default)" = "graphical.target" ] || \
            die "systemctl get-default is not graphical.target; refusing to change it (do it explicitly)"
        systemctl is-enabled gdm3 >/dev/null 2>&1 || die "gdm3 is not enabled; refusing to change display managers"

        # Record the ORIGINAL values ONCE. Never rebase on upgrade: re-recording
        # the CURRENT value would store our own 'true' as the "original", and
        # unconfigure could never restore the real prior state.
        if [ ! -f "$STATE/autologin.orig" ]; then
            cp -p "$GDM_CONF" "$STATE/custom.conf.orig"
            oe="$(awk '/^\[daemon\]/{d=1;next} /^\[/{d=0} d&&/^AutomaticLoginEnable=/{sub(/^AutomaticLoginEnable=/,"");print;exit}' "$GDM_CONF")"
            ou="$(awk '/^\[daemon\]/{d=1;next} /^\[/{d=0} d&&/^AutomaticLogin=/{sub(/^AutomaticLogin=/,"");print;exit}' "$GDM_CONF")"
            printf '%s\n' "${oe:-false}" > "$STATE/autologin.orig"
            printf '%s\n' "${ou:-}"      > "$STATE/autologin.origuser"
            echo "denso-setup: recorded original autologin state (enable='${oe:-false}' user='${ou:-}')"
        else
            echo "denso-setup: original autologin state already recorded — not rebasing"
        fi
        printf '%s\n' "$user" > "$STATE/autologin.user"
        gdm_set_autologin "$GDM_CONF" "$user" || die "failed to edit $GDM_CONF"
        echo "denso-setup: autologin ENABLED for $user"
        echo "denso-setup: WARNING — anyone with physical access now gets $user's desktop."
    fi

    echo "denso-setup: configured. Run 'denso-setup verify' next."
}

cmd_verify() {
    rc=0
    user="$(cat "$STATE/user" 2>/dev/null || echo "")"
    [ -n "$user" ] || die "not configured yet — run 'denso-setup configure --user <u>' first"

    echo "== platform"
    [ "$(uname -m)" = "aarch64" ] || { echo "  FAIL arch is $(uname -m), expected aarch64"; rc=1; }
    l4t="$(sed -n 's/^# R\([0-9]*\).*REVISION: \([0-9.]*\).*/\1.\2/p' /etc/nv_tegra_release 2>/dev/null | head -1)"
    echo "  L4T: ${l4t:-unknown} (supported baseline: 36.5)"
    [ "$l4t" = "36.5.0" ] || echo "  WARN L4T is '${l4t:-unknown}', not the tested 36.5.0 — engines are TRT/arch-pinned"

    echo "== apt plan (protected JetPack packages must not be touched)"
    plan="$(mktemp)"
    LC_ALL=C apt-get -s install --no-install-recommends denso-digitalreader > "$plan" 2>/dev/null || true
    if apt_plan_ok "$plan"; then echo "  ok  apt would not touch a protected package"; else rc=1; fi
    rm -f "$plan"

    echo "== migration smoke test (on a THROWAWAY copy — never the live DB)"
    tmp="$(mktemp -d)"; chown "$user":"$user" "$tmp"
    if [ -f "$DATA/denso.db" ]; then
        cp "$DATA/denso.db" "$tmp/copy.db"; chown "$user":"$user" "$tmp/copy.db"
    fi
    # No live DB -> an empty temp file exercises the full v0 chain the app will
    # run on first launch.
    if as_user "$user" env HOME="$tmp" TMPDIR="$tmp" "$DENSO" --check-migrations "$tmp/copy.db"; then
        echo "  ok  migrations apply cleanly"
    else
        echo "  FAIL migrations would fail on this database — do NOT launch"; rc=1
    fi
    rm -rf "$tmp"

    echo "== runtime + engines (--check, as $user)"
    engines=""
    for eng in "$PKG_MODELS"/*.engine; do
        [ -e "$eng" ] || continue
        engines="$engines --engine $(basename "$eng")"
    done
    tmp="$(mktemp -d)"; chown "$user":"$user" "$tmp"
    # shellcheck disable=SC2086
    if as_user "$user" env HOME="$tmp" TMPDIR="$tmp" CUDA_CACHE_PATH="$tmp/cuda" "$DENSO" --check $engines; then
        echo "  ok  runtime + engines validate"
    else
        echo "  FAIL --check did not pass"; rc=1
    fi
    rm -rf "$tmp"

    echo "== instance"
    set +e
    as_user "$user" "$DENSO" --check-running >/dev/null 2>&1; crc=$?
    set -e
    case "$crc" in
        0) echo "  note the app is currently RUNNING" ;;
        1) echo "  ok  not running" ;;
        *) echo "  FAIL cannot determine liveness (rc=$crc) — check $DATA ownership"; rc=1 ;;
    esac

    [ "$rc" = "0" ] && echo "verify: PASS" || echo "verify: FAIL"
    return $rc
}

cmd_unconfigure() {
    need_root
    user="$(cat "$STATE/user" 2>/dev/null || echo "")"
    if [ -n "$user" ]; then
        home="$(getent passwd "$user" | cut -d: -f6 || echo "")"
        [ -n "$home" ] && rm -f "$home/.config/autostart/$AUTOSTART_NAME" && \
            echo "denso-setup: autostart removed for $user"
    fi
    if [ -f "$STATE/autologin.orig" ] && [ -f "$GDM_CONF" ]; then
        oe="$(cat "$STATE/autologin.orig")"
        ou="$(cat "$STATE/autologin.origuser" 2>/dev/null || echo "")"
        if gdm_restore_autologin "$GDM_CONF" "$oe" "$ou"; then
            echo "denso-setup: autologin restored (enable='$oe' user='$ou')"
        else
            echo "denso-setup: autologin NOT restored — an admin changed it after us; leaving their setting." >&2
        fi
    fi
    echo "denso-setup: unconfigured (data in $DATA kept)"
}

cmd_replace_model() {
    need_root
    stem="${1-}"; [ -n "$stem" ] || die "usage: denso-setup replace-model <stem>"
    user="$(cat "$STATE/user" 2>/dev/null)" || die "not configured"
    eng="$PKG_MODELS/$stem.engine"; side="$PKG_MODELS/$stem.names.json"
    [ -f "$eng" ] && [ -f "$side" ] || die "no packaged model '$stem'"
    install_pair "$eng" "$side" "$DATA/models" || die "failed to install $stem"
    chown "$user":"$user" "$DATA/models/$stem.engine" "$DATA/models/$stem.names.json"
    echo "denso-setup: replaced $stem.engine with the packaged one"
}

case "${1-}" in
    configure)     shift; cmd_configure "$@" ;;
    verify)        shift; cmd_verify "$@" ;;
    unconfigure)   shift; cmd_unconfigure "$@" ;;
    replace-model) shift; cmd_replace_model "$@" ;;
    *) echo "usage: denso-setup {configure --user <u> [--autostart] [--enable-autologin] | verify | unconfigure | replace-model <stem>}" >&2
       exit 2 ;;
esac
```

- [ ] **Step 2: Verify syntax**

```sh
sh -n packaging/denso-setup && echo "denso-setup: syntax ok"
```

- [ ] **Step 3: Commit**

```bash
git add packaging/denso-setup
git commit -m "feat(packaging): denso-setup — configure / verify / unconfigure / replace-model

apt install cannot accept --user/--autostart and interactive maintainer-script
prompts are wrong, so configuration is an explicit second command.

Everything touching /opt/denso/data runs as the target user via runuser: as root
it leaves root-owned trt_cache/log/lock the app can't write -- and a root-owned
lock makes every later --check-running return 4.

--user is required; SUDO_USER is never trusted (absent under automation, wrong
from a root shell). Autologin records the ORIGINAL values ONCE and never rebases
on upgrade (re-recording the current value would store our own 'true' as the
'original' and unconfigure could never restore the truth), warns that physical
access now grants a desktop, and refuses to change the default target or display
manager on the operator's behalf. --autostart warns that XDG autostart is inert
without autologin, since this box stops at the GDM greeter on power-on.

verify runs the migration smoke on a THROWAWAY copy, so a bad migration is
caught before it ever touches the live DB."
```

---

### Task 6: `build_package.sh` — build, verify deps, emit the `.deb`

**Files:**
- Create: `tools/build_package.sh`, `packaging/models.approved`
- Test: run on the Jetson (Task 7)

**Interfaces:**
- Consumes: `packaging/**`, `policy.sh`'s `version_ok`.
- Produces: `denso-digitalreader_<version>_arm64.deb` + `.sha256` + an embedded `MANIFEST`.

- [ ] **Step 1: Write the approval manifest**

Create `packaging/models.approved`:

```
# Approved production models. build_package.sh --model <path> REFUSES anything
# not listed here with a matching SHA-256.
#
# This is why model selection is not a models/*.engine glob: a glob is accidental
# directory-content selection, and models/ is git-ignored, so a forgotten
# experimental engine with a valid sidecar would silently reach production.
#
# Format: <stem> <sha256-of-.engine> <trtexec recipe used to build it>
#
# To approve a new engine: build it on-device, sha256sum it, add the line.
digitv2 29b24a69fcee995485b075715c0008e8e1ab938c4d0ace3c9d560bc3278a4356 trtexec --onnx=digitv2.onnx --saveEngine=digitv2.engine --fp16
```

That is the **real** SHA-256 of the engine currently on the Jetson (`sha256sum models/digitv2.engine`, 2026-07-17), not a placeholder. If the build reports a mismatch, the engine on the box was rebuilt — record the new hash deliberately rather than loosening the check.

- [ ] **Step 2: Write the builder**

Create `tools/build_package.sh`:

```sh
#!/usr/bin/env bash
# Build the Denso .deb. MUST run on an aarch64 JetPack 6.2 Jetson — there is no
# cross-toolchain, and the engines are sm_87/TRT-10.3 pinned.
#
#   tools/build_package.sh --model models/digitv2.engine [--allow-dirty]
set -euo pipefail

HERE="$(cd "$(dirname "$0")/.." && pwd)"
. "$HERE/packaging/lib/policy.sh"

MODELS=()
ALLOW_DIRTY=0
while [ $# -gt 0 ]; do
    case "$1" in
        --model) MODELS+=("$2"); shift 2 ;;
        --allow-dirty) ALLOW_DIRTY=1; shift ;;
        *) echo "usage: $0 --model <path/to.engine> [--model ...] [--allow-dirty]" >&2; exit 2 ;;
    esac
done
[ "${#MODELS[@]}" -gt 0 ] || { echo "at least one --model is required (never a glob — see packaging/models.approved)" >&2; exit 2; }
[ "$(uname -m)" = "aarch64" ] || { echo "must build on aarch64 (engines are sm_87-pinned)" >&2; exit 1; }

cd "$HERE"

# ── version: 0.1.0+g<sha>, dirty-marked. It becomes a dpkg field AND a path.
APP_VERSION="$(sed -n 's/.*APP_VERSION="\([^"]*\)".*/\1/p' src/app/CMakeLists.txt | head -1)"
SHA="$(git rev-parse --short HEAD)"
DIRTY=""
if ! git diff --quiet || ! git diff --cached --quiet; then
    [ "$ALLOW_DIRTY" = "1" ] || { echo "refusing to package a dirty tree (use --allow-dirty to override)" >&2; exit 1; }
    DIRTY="+dirty"
fi
VERSION="${APP_VERSION}+g${SHA}${DIRTY}"
version_ok "$VERSION" || { echo "computed version is not a safe path component: $VERSION" >&2; exit 1; }
echo ">> version: $VERSION"

# ── models: explicit, and hash-checked against the tracked approval manifest.
for m in "${MODELS[@]}"; do
    [ -f "$m" ] || { echo "no such engine: $m" >&2; exit 1; }
    stem="$(basename "$m" .engine)"
    side="$(dirname "$m")/$stem.names.json"
    [ -f "$side" ] || { echo "engine $stem has no sidecar: $side" >&2; exit 1; }
    want="$(awk -v s="$stem" '$1==s {print $2}' packaging/models.approved)"
    [ -n "$want" ] || { echo "engine '$stem' is not in packaging/models.approved — approve it explicitly" >&2; exit 1; }
    got="$(sha256sum "$m" | cut -d' ' -f1)"
    [ "$want" = "$got" ] || { echo "engine '$stem' HASH MISMATCH:" >&2; echo "  approved: $want" >&2; echo "  actual:   $got" >&2; exit 1; }
    echo ">> model approved: $stem ($got)"
done

# ── build
echo ">> building Release"
cmake -S . -B build-pkg -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build build-pkg --target denso -j"$(nproc)" >/dev/null
EXE=build-pkg/src/app/denso
[ -f "$EXE" ] || { echo "build produced no exe" >&2; exit 1; }

# ── stage the package tree
STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT
install -d "$STAGE/DEBIAN" "$STAGE/opt/denso/bin" "$STAGE/opt/denso/models" \
           "$STAGE/opt/denso/lib" "$STAGE/usr/bin" "$STAGE/usr/share/applications" \
           "$STAGE/usr/share/icons/hicolor/256x256/apps"

install -m 0755 "$EXE"                              "$STAGE/opt/denso/bin/denso"
install -m 0644 packaging/lib/policy.sh             "$STAGE/opt/denso/lib/policy.sh"
install -m 0755 packaging/denso-digitalreader       "$STAGE/usr/bin/denso-digitalreader"
install -m 0755 packaging/denso-setup               "$STAGE/usr/bin/denso-setup"
install -m 0644 packaging/com.denso.DigitalReader.desktop \
                                                    "$STAGE/usr/share/applications/com.denso.DigitalReader.desktop"
install -m 0644 assets/icon.png                     "$STAGE/usr/share/icons/hicolor/256x256/apps/denso-digitalreader.png"
for m in "${MODELS[@]}"; do
    stem="$(basename "$m" .engine)"
    install -m 0644 "$m" "$STAGE/opt/denso/models/$stem.engine"
    install -m 0644 "$(dirname "$m")/$stem.names.json" "$STAGE/opt/denso/models/$stem.names.json"
done
for s in postinst prerm postrm; do install -m 0755 "packaging/debian/$s" "$STAGE/DEBIAN/$s"; done

ARCH="$(dpkg --print-architecture)"
sed -e "s/@VERSION@/$VERSION/" -e "s/@ARCH@/$ARCH/" packaging/debian/control.in > "$STAGE/DEBIAN/control"

# ── DEPENDENCY VERIFICATION (the spec's "the build must fail, never paper over")
#
# dpkg-shlibdeps cannot be used here: it hard-errors on
# /usr/local/cuda/lib64/libcudart.so.12 ("no dependency information found") and
# libopencv ships no .shlibs/.symbols, so the only way to run it is
# --ignore-missing-info, which silently DROPS deps while looking derived.
#
# Instead: derive the owning-package set mechanically and assert the declared
# Depends: covers it.
echo ">> verifying declared Depends: against the exe's actual NEEDED sonames"
DECLARED="$(awk '/^Depends:/,/^Description:/' "$STAGE/DEBIAN/control" | tr ',' '\n' | sed -e 's/^Depends://' -e 's/(.*)//' -e 's/[[:space:]]//g' | grep -v '^$' | grep -v '^Description:')"
missing=0
for soname in $(objdump -p "$EXE" | awk '/NEEDED/{print $2}'); do
    lib="$(ldd "$EXE" | awk -v s="$soname" '$1==s {print $3}')"
    [ -n "$lib" ] && [ -e "$lib" ] || { echo "  UNRESOLVED $soname" >&2; missing=1; continue; }
    pkg="$(dpkg -S "$(readlink -f "$lib")" 2>/dev/null | cut -d: -f1 | head -1)"
    if [ -z "$pkg" ]; then
        echo "  UNOWNED    $soname -> $lib (no dpkg package owns it — review before shipping)" >&2
        missing=1; continue
    fi
    case "$pkg" in
        # JetPack-owned. Deliberately NOT declared: they are on the protected
        # list, and a versioned dep invites apt to touch the JetPack stack. They
        # ship with the image; --check proves they work at install time.
        libnvinfer*|cuda-*|nvidia-l4t-*|libc6|libstdc++6|libgcc-s1|zlib1g)
            echo "  exempt     $soname -> $pkg" ; continue ;;
    esac
    if echo "$DECLARED" | grep -qx "$pkg"; then
        echo "  ok         $soname -> $pkg"
    else
        echo "  UNDECLARED $soname -> $pkg  (add it to packaging/debian/control.in)" >&2
        missing=1
    fi
done
[ "$missing" = "0" ] || { echo "dependency verification FAILED — refusing to build a package with undeclared deps" >&2; exit 1; }

# ── MANIFEST: what this artifact IS, for after-the-fact diagnosis.
{
    echo "package: denso-digitalreader"
    echo "version: $VERSION"
    echo "arch: $ARCH"
    echo "source-sha: $SHA${DIRTY:+ (DIRTY TREE)}"
    echo "built: $(date -Is)"
    echo "builder: $(uname -sr) $(uname -m)"
    echo "l4t: $(sed -n 's/^# R\([0-9]*\).*REVISION: \([0-9.]*\).*/\1.\2/p' /etc/nv_tegra_release 2>/dev/null | head -1)"
    echo "cmake: $(cmake --version | head -1)"
    echo "gcc: $(gcc --version | head -1)"
    echo "qt: $(dpkg-query -W -f='${Version}' libqt6core6 2>/dev/null)"
    echo "tensorrt: $(dpkg-query -W -f='${Version}' libnvinfer10 2>/dev/null)"
    echo "cuda-cudart: $(dpkg-query -W -f='${Version}' cuda-cudart-12-6 2>/dev/null)"
    echo "opencv: $(dpkg-query -W -f='${Version}' libopencv 2>/dev/null)"
    echo "exe-sha256: $(sha256sum "$EXE" | cut -d' ' -f1)"
    for m in "${MODELS[@]}"; do
        stem="$(basename "$m" .engine)"
        echo "model: $stem engine-sha256=$(sha256sum "$m" | cut -d' ' -f1) sidecar-sha256=$(sha256sum "$(dirname "$m")/$stem.names.json" | cut -d' ' -f1)"
        echo "model-recipe: $stem $(awk -v s="$stem" '$1==s {$1="";$2="";print}' packaging/models.approved | sed 's/^  *//')"
    done
    echo "--- ldd report (diagnostic evidence, not a dependency source) ---"
    ldd "$EXE"
} > "$STAGE/opt/denso/MANIFEST"

# ── build the .deb
OUT="denso-digitalreader_${VERSION}_${ARCH}.deb"
dpkg-deb --build --root-owner-group "$STAGE" "$OUT" >/dev/null
sha256sum "$OUT" > "$OUT.sha256"

echo
echo ">> built $OUT"
dpkg-deb --info "$OUT" | sed -n '1,12p'
echo ">> install with:  sudo apt install ./$OUT     (NEVER dpkg -i — it does not resolve deps)"
```

- [ ] **Step 3: Verify syntax**

```sh
chmod +x tools/build_package.sh && bash -n tools/build_package.sh && echo "build_package.sh: syntax ok"
```

- [ ] **Step 4: Commit**

```bash
git add tools/build_package.sh packaging/models.approved
git commit -m "feat(packaging): build_package.sh + the tracked model approval manifest

Models are named explicitly and hash-checked against packaging/models.approved.
A models/*.engine glob would be accidental directory-content selection, and
models/ is git-ignored, so a forgotten experimental engine with a valid sidecar
would silently reach production. Hash mismatch is a hard failure.

Dependency verification replaces \${shlibs:Depends}: dpkg-shlibdeps hard-errors on
libcudart.so.12 and libopencv has no shlibs, so the substvar is only reachable
via --ignore-missing-info -- which drops deps silently while looking derived. We
instead walk the exe's NEEDED sonames, resolve each to its owning package, and
FAIL the build on anything undeclared or unowned. JetPack-owned libs are exempt
and the exemption is PRINTED, so it is visible rather than silent."
```

---

### Task 7: On-device — build, install, verify, upgrade, refuse

**Files:** none (verification only)

**Interfaces:** consumes everything above.

**This is the first real test of the slice.** Everything before it was syntax checks and pure-function tests.

- [ ] **Step 1: Confirm the approved hash still matches the box**

```sh
ssh modela@192.168.1.15 'cd ~/project/Denso-DigitalReader && sha256sum models/digitv2.engine'
```
Expected: `29b24a69fcee995485b075715c0008e8e1ab938c4d0ace3c9d560bc3278a4356`, matching `packaging/models.approved`. If it differs, the engine was rebuilt on-device — update the manifest line **deliberately** (and say so in the commit); never relax the check to make a build pass.

- [ ] **Step 2: Ship the branch to the Jetson and build the .deb**

Use a git bundle — do NOT push to GitHub:
```sh
git bundle create /tmp/slice2.bundle <jetson-current-commit>..HEAD
scp /tmp/slice2.bundle modela@192.168.1.15:/tmp/
ssh modela@192.168.1.15 'cd ~/project/Denso-DigitalReader &&
  git fetch /tmp/slice2.bundle HEAD:slice2 && git checkout slice2 &&
  ./tools/build_package.sh --model models/digitv2.engine'
```
Expected: version line, `model approved`, every soname `ok`/`exempt`, then `>> built denso-digitalreader_0.1.0+g<sha>_arm64.deb`.
**If any soname prints `UNDECLARED`, the build fails — that is the check working.** Add the named package to `control.in`, commit, rebuild.

- [ ] **Step 3: Install and verify**

```sh
ssh modela@192.168.1.15 'cd ~/project/Denso-DigitalReader &&
  sudo apt install -y ./denso-digitalreader_*.deb &&
  dpkg -l denso-digitalreader &&
  sudo denso-setup configure --user modela &&
  denso-setup verify'
```
Expected: `dpkg -l` shows `ii denso-digitalreader`; `configure` seeds `digitv2.engine` into `/opt/denso/data/models`; `verify` prints `verify: PASS`.

- [ ] **Step 4: The gates that matter**

```sh
ssh modela@192.168.1.15 '
set -x
# data dir is operator-owned, NOT root — the whole ownership contract
ls -ld /opt/denso/data /opt/denso/data/models
# the launcher works and is on PATH
which denso-digitalreader
# prerm REFUSES while the app runs (start it headless-ish, then try to remove)
# (run this one from the AnyDesk desktop session, not SSH — it needs a display)
# apt remove KEEPS data; apt purge removes it
sudo apt remove -y denso-digitalreader
ls /opt/denso/data/models    # engines MUST still be here
sudo apt install -y ./denso-digitalreader_*.deb   # reinstall for the purge test
'
```
Expected: `/opt/denso/data` owned by `modela:modela`; `which` → `/usr/bin/denso-digitalreader`; after `apt remove` the engines survive.

- [ ] **Step 5: Upgrade preserves state**

```sh
ssh modela@192.168.1.15 '
# Write a marker into the DB dir, upgrade, confirm it survived.
touch /opt/denso/data/UPGRADE_MARKER
sudo apt install -y --reinstall ./denso-digitalreader_*.deb
ls /opt/denso/data/UPGRADE_MARKER && echo "ok: data survived the upgrade"
ls /opt/denso/data/models/digitv2.engine && echo "ok: engine survived the upgrade"
'
```

- [ ] **Step 6: Commit any fixes, then record results**

Append the observed output to the plan's execution notes or the ledger. Do not mark Task 7 done on anything less than: build ok, `verify: PASS`, data operator-owned, remove-keeps-data, upgrade-preserves-state.

---

### Task 8: On-device — autostart, autologin, power-cycle

**Files:** none (verification only)

**This is the appliance's actual promise: power on, app runs, nobody touches it.** It cannot be tested any other way.

- [ ] **Step 1: Enable autostart + autologin**

Run from the **AnyDesk desktop session** (details in `d:\workspace\devices.md`), not SSH:
```sh
sudo denso-setup configure --user modela --autostart --enable-autologin
```
Expected: autostart entry written to `~/.config/autostart/`, the recorded-original message, the physical-access warning.

- [ ] **Step 2: Confirm what was recorded**

```sh
sudo cat /opt/denso/install-state/autologin.orig      # expect: false (the pre-Denso value)
sudo cat /opt/denso/install-state/autologin.user      # expect: modela
sudo grep -A3 '^\[daemon\]' /etc/gdm3/custom.conf     # expect AutomaticLoginEnable=true, AutomaticLogin=modela
ls ~/.config/autostart/com.denso.DigitalReader.desktop
```

- [ ] **Step 3: Power-cycle — the real gate**

```sh
ssh modela@192.168.1.15 'sudo reboot'
```
Wait ~60s, then from the **AnyDesk session** confirm:
- the box reached a desktop **without anyone typing a password**;
- Denso Digital Reader is **running**;
- `loginctl show-session $XDG_SESSION_ID -p Type` — **record whether it is x11 or wayland.** This is the open question the spec flagged: the greeter is X11, but the user session type was never observed. If it is `wayland`, decide then whether the launcher must pin `QT_QPA_PLATFORM=xcb` — do not pin it before observing.
- `denso --check-running` → **exit 0** (it is running);
- `/opt/denso/data/denso.log` has a fresh `SESSION start` line;
- the camera grid recovers (cameras + network come up after the app does — the app must tolerate their absence, it does not wait for them).

- [ ] **Step 4: Verify the second-instance guard on the real appliance**

With the app running from autostart, click the menu icon.
Expected: a message box "Denso DigitalReader is already running." and no second process (`pgrep -c denso` → 1). This is the guard Slice 1 built, in the exact scenario it was built for.

- [ ] **Step 5: Verify unconfigure restores the box**

```sh
sudo denso-setup unconfigure
sudo grep -A3 '^\[daemon\]' /etc/gdm3/custom.conf   # AutomaticLoginEnable back to false
ls ~/.config/autostart/ | grep -c denso             # 0
```
Then reboot once more and confirm the box stops at the **GDM greeter** again — proving the change is genuinely reversible and we left no trace.

- [ ] **Step 6: Record the session type in the docs**

Whatever Step 3 observed, write it into `docs/ARCHITECTURE.md`'s deployment notes and the spec's D8, replacing "the user session type is unknown until autologin is exercised". Commit.

---

## Slice 2 exit criteria

- [ ] `./tests/packaging/run.sh` passes on **both** the dev box and the Jetson.
- [ ] `build_package.sh` fails loudly on: a dirty tree, an unapproved model, a hash mismatch, and an undeclared dependency.
- [ ] `sudo apt install ./denso-digitalreader_*.deb` resolves deps and `dpkg -l` lists it.
- [ ] `denso-setup verify` → `verify: PASS` on the Jetson.
- [ ] `/opt/denso/data` is operator-owned; no root-owned artifacts anywhere in it.
- [ ] `apt remove` keeps data; `apt purge` removes it behind the resolved-path guard.
- [ ] An upgrade preserves the database and the operator's engines.
- [ ] `prerm` refuses while the app is running.
- [ ] **Power-cycle → autologin → autostart → GUI up, unattended.**
- [ ] The second-instance guard fires on the real appliance.
- [ ] `unconfigure` + reboot → the box is back to the greeter, no trace left.
- [ ] The user session type (x11 vs wayland) is **observed and recorded**.

## Explicitly NOT in this slice

A hosted apt repo / package signing (phase 2 — for a handful of hand-managed boxes, `apt install ./file.deb` is enough) · `systemd` units · crash supervision · the l4t-jetpack container diagnostic (supplemental evidence only, never a gate — the first real-target install IS the acceptance test) · any change to the Slice 1 app code.
