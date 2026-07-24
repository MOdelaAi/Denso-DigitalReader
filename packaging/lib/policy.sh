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
# A subshell body ( ... ) instead of { ... }: POSIX shell variables are global
# by default, so a brace-bodied function's locals clobber the CALLER's
# variables of the same name. denso-setup sources this file and uses
# $plan/$tmp/$user/$eng/$side itself — a live collision, not theoretical
# (demonstrated: caller's plan="MY IMPORTANT VALUE" came back as
# plan=/tmp/p.txt after calling apt_plan_ok). A subshell still propagates
# stdout/stderr and exit status; only assignments are contained.
version_ok() (
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
)

# --- check_verdict <exit-code> ----------------------------------------------
# Map `denso --check`'s exit code to a verify action. The readiness contract
# (src/core/health/integrity.cpp exit_code_for) is:
#   0  = Ready               -> "ok"       verify continues
#   10 = Degraded serviceable -> "degraded" verify WARNS but continues (the
#        production Jetson runs unmanifested engines by design — a warning, not a
#        blocker; see the §2.3 compatibility decision)
#   78 = Blocked (EX_CONFIG)  -> "blocked"  verify STOPS: a configuration fault
#   *  = anything else        -> "failed"   an UNEXPECTED check failure -> stop
# Fail closed on unknown codes: never silently continue on a code we do not model.
# Echoes the token; the caller branches on it. Subshell body — see version_ok.
check_verdict() (
    case "${1-}" in
        0)  echo ok ;;
        10) echo degraded ;;
        78) echo blocked ;;
        *)  echo failed ;;
    esac
)

# --- apt_plan_ok <plan-file> -------------------------------------------------
# Reads `LC_ALL=C apt-get -s install ...` output. A Depends: declaration does
# NOT stop apt from removing or replacing a protected package to satisfy
# constraints — that is what this guard is for.
#
# Real line shapes:
#   Inst cowsay (3.03+dfsg2-8 Ubuntu:22.04/jammy [all])
#   Remv somepkg [1.2-3]
apt_plan_ok() (
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
            nvidia-l4t-*|cuda-*|libnvinfer*|libnvonnxparsers*|libcudla*|tensorrt*)
                # We legitimately DEPEND on some of these (we link libcudart and
                # libnvinfer). On a correctly-provisioned box they are already
                # installed, so a plan mentions them only when the box is MISSING
                # a JetPack component — which means it is outside the supported
                # baseline and the app could not run anyway. Refusing is correct;
                # auto-repairing would mix repo and JetPack components, the exact
                # risk this guard exists for. So say THAT, not "protected".
                echo "apt-plan: REFUSED — required JetPack component '$pkg' is missing from this box." >&2
                echo "  Denso never installs or modifies CUDA, TensorRT or L4T packages." >&2
                echo "  Restore the supported JetPack 6.2 / L4T R36.5.0 stack, then retry." >&2
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
)

# --- seed_decision_pair <eng-src> <side-src> <eng-dst> <side-dst> ------------
# The decision is about the PAIR, never the engine alone: an identical engine
# whose sidecar is missing or corrupt is BROKEN, not "same" — TrtEngine's ctor
# reads <stem>.names.json and throws without it, so classifying that as "same"
# would leave a camera permanently unable to start.
#
#   both absent           -> seed
#   both present+identical -> same
#   anything else (partial pair, either side differing) -> differs
#
# "differs" NEVER overwrites silently: the operator may have built their own
# engine, and only an explicit `denso-setup replace-model` may replace it.
seed_decision_pair() (
    esrc="${1-}"; ssrc="${2-}"; edst="${3-}"; sdst="${4-}"
    if [ ! -e "$edst" ] && [ ! -e "$sdst" ]; then echo "seed"; return 0; fi
    if [ ! -e "$edst" ] || [ ! -e "$sdst" ]; then echo "differs"; return 0; fi  # partial pair
    e1="$(sha256sum "$esrc" | cut -d' ' -f1)"; e2="$(sha256sum "$edst" | cut -d' ' -f1)"
    s1="$(sha256sum "$ssrc" | cut -d' ' -f1)"; s2="$(sha256sum "$sdst" | cut -d' ' -f1)"
    if [ "$e1" = "$e2" ] && [ "$s1" = "$s2" ]; then echo "same"; else echo "differs"; fi
)

# --- install_pair <engine-src> <sidecar-src> <dst-dir> -----------------------
# Ordered and crash-resistant, NOT atomic: two flat files cannot be made atomic
# with two renames. The ENGINE's appearance is the commit marker — sidecar first,
# engine last — so a newly visible engine always has its sidecar. (TrtEngine's
# ctor reads <stem>.names.json and throws without it, so the reverse order could
# strand a loadable-looking engine with no names.)
install_pair() (
    eng="${1-}"; side="${2-}"; dst="${3-}"
    [ -f "$eng" ] && [ -f "$side" ] && [ -d "$dst" ] || return 1
    stem="$(basename "$eng" .engine)"

    cp "$side" "$dst/.$stem.names.json.tmp" || return 1
    sync || return 1
    mv "$dst/.$stem.names.json.tmp" "$dst/$stem.names.json" || return 1

    cp "$eng" "$dst/.$stem.engine.tmp" || return 1
    sync || return 1   # flushes the sidecar's rename before the engine's rename
    mv "$dst/.$stem.engine.tmp" "$dst/$stem.engine" || return 1

    sync || return 1
    return 0
)

# --- manifests_equivalent <a.json> <b.json> ----------------------------------
# Canonical, SEMANTIC equivalence of two model manifests — NOT a byte compare.
# A reformatted-but-equivalent manifest (whitespace, key order) is "equivalent";
# any real content difference, malformed JSON, a duplicate key, or a non-finite
# number is NOT — and the caller (seed-manifest) then refuses rather than
# overwrites. This is the one place the "already current vs conflict" decision is
# made, so it must be strict where Python's own `==` is loose:
#   * duplicate object keys are REJECTED (the default parser would silently keep
#     the last, so a tampered manifest with a duplicated engine hash could read
#     as equivalent);
#   * NaN / Infinity are REJECTED (Qt's parser rejects them too, so accepting one
#     here would call a file "current" that the app treats as ManifestCorrupt);
#   * types are compared IDENTICALLY — Python treats True == 1 and 1 == 1.0, but
#     the manifest schema does not, so a bool where an int belongs must differ.
# No compatibility policy or mode data is consulted — this is pure structure.
#   exit 0 = canonically equivalent
#   exit 1 = both parse but differ
#   exit 2 = either side is unreadable / malformed / not a strict manifest value
manifests_equivalent() (
    a="${1-}"; b="${2-}"
    python3 - "$a" "$b" <<'PY'
import json, sys

def no_dupes(pairs):
    seen = {}
    for k, v in pairs:
        if k in seen:
            raise ValueError("duplicate key: %r" % k)
        seen[k] = v
    return seen

def load(path):
    with open(path, "r", encoding="utf-8") as fh:
        # parse_constant fires on NaN/Infinity/-Infinity; object_pairs_hook
        # rejects duplicate keys. Either raises -> unreadable -> refuse.
        return json.load(fh, object_pairs_hook=no_dupes,
                         parse_constant=lambda c: (_ for _ in ()).throw(
                             ValueError("non-finite number: %s" % c)))

def eq(x, y):
    # Type identity first: bool is a subclass of int, and 1 == 1.0, so a plain
    # `==` would conflate values the schema keeps distinct.
    if type(x) is not type(y):
        return False
    if isinstance(x, dict):
        if x.keys() != y.keys():
            return False
        return all(eq(x[k], y[k]) for k in x)
    if isinstance(x, list):
        return len(x) == len(y) and all(eq(i, j) for i, j in zip(x, y))
    return x == y

try:
    a = load(sys.argv[1]); b = load(sys.argv[2])
except Exception as exc:
    sys.stderr.write("manifests_equivalent: unreadable/malformed: %s\n" % exc)
    sys.exit(2)
sys.exit(0 if eq(a, b) else 1)
PY
)

# --- manifest_matches_models_dir <manifest.json> <models-dir> ----------------
# Prove a manifest declares EXACTLY the TensorRT engine/sidecar pairs present in
# a models directory, with hashes that agree — the set-consistency check
# seed-manifest runs before it will seed anything. A manifest that describes a
# different, extra or missing engine, or a stale hash, must never be installed as
# the appliance's identity authority.
#   exit 0 = every declared TRT engine matches an on-disk engine+sidecar by
#            filename AND sha256, and every on-disk *.engine is declared
#   exit 1 = a mismatch, a missing/extra engine, or a sidecar hash disagreement
#   exit 2 = the manifest is unreadable / malformed
manifest_matches_models_dir() (
    manifest="${1-}"; dir="${2-}"
    python3 - "$manifest" "$dir" <<'PY'
import hashlib, json, os, sys

def no_dupes(pairs):
    seen = {}
    for k, v in pairs:
        if k in seen:
            raise ValueError("duplicate key: %r" % k)
        seen[k] = v
    return seen

def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()

manifest, models_dir = sys.argv[1], sys.argv[2]
try:
    with open(manifest, "r", encoding="utf-8") as fh:
        m = json.load(fh, object_pairs_hook=no_dupes)
    gens = m["generations"]
    assert isinstance(gens, list)
except Exception as exc:
    sys.stderr.write("manifest unreadable: %s\n" % exc)
    sys.exit(2)

declared = {}   # engine filename -> (engine_sha, sidecar filename, sidecar_sha)
for g in gens:
    trt = (g.get("runtime") or {}).get("tensorrt")
    if not trt:
        continue
    declared[trt["engine"]] = (trt["engine_sha256"], trt["sidecar"],
                               trt["sidecar_sha256"])

on_disk = sorted(f for f in os.listdir(models_dir) if f.endswith(".engine"))
if set(on_disk) != set(declared):
    sys.stderr.write("engine set mismatch: on-disk=%s declared=%s\n"
                     % (on_disk, sorted(declared)))
    sys.exit(1)

for eng, (eng_sha, side, side_sha) in declared.items():
    epath = os.path.join(models_dir, eng)
    spath = os.path.join(models_dir, side)
    if not os.path.isfile(epath) or not os.path.isfile(spath):
        sys.stderr.write("missing on-disk artifact for %s\n" % eng)
        sys.exit(1)
    if sha256(epath) != eng_sha or sha256(spath) != side_sha:
        sys.stderr.write("hash mismatch for %s\n" % eng)
        sys.exit(1)
sys.exit(0)
PY
)

# --- install_manifest_atomic <src-manifest> <dst-manifest> -------------------
# MECHANICS (not a decision): publish a manifest into a data directory without a
# race and without ever clobbering a different existing file. The caller has
# already decided the destination should be absent; this closes the window
# between that decision and the write:
#   * a UNIQUE temporary (mktemp) beside the destination, never a fixed hidden
#     name two concurrent runs would share;
#   * mode pinned 0644 (never umask-inherited via cp);
#   * flush to disk, then publish with `ln` — an ATOMIC create-if-absent, so a
#     destination that appeared after the caller's check is NOT overwritten;
#   * the temporary is always removed, so no stray *.tmp survives.
# Run this AS THE TARGET USER (the caller wraps it in runuser) so the data-dir
# write is never transiently root-owned.
#   exit 0 = published (destination created from src)
#   exit 4 = destination already exists (a concurrent run won the race, or the
#            caller's absence check was stale) — the caller must re-compare
#   exit 1 = a real failure; nothing was published
install_manifest_atomic() (
    src="${1-}"; dst="${2-}"
    [ -f "$src" ] || return 1
    dstdir="$(dirname "$dst")"
    [ -d "$dstdir" ] || return 1
    tmp="$(mktemp "$dstdir/.manifest.json.XXXXXX")" || return 1
    # From here on, always clean up the temp.
    if ! cp "$src" "$tmp"; then rm -f "$tmp"; return 1; fi
    if ! chmod 0644 "$tmp"; then rm -f "$tmp"; return 1; fi
    sync || { rm -f "$tmp"; return 1; }
    # `ln` is create-if-absent and atomic: it fails if $dst exists. Distinguish
    # "it already existed" (race, exit 4) from a genuine error by re-testing.
    if ln "$tmp" "$dst" 2>/dev/null; then
        rm -f "$tmp"; sync; return 0
    fi
    rm -f "$tmp"
    [ -e "$dst" ] && return 4
    return 1
)

# --- seed_manifest_decide <pkg-manifest> <pkg-models> <data-models> <data-manifest>
# The READ-ONLY seed-manifest decision, factored out of denso-setup so
# tests/packaging/run.sh can drive every branch with fixtures (denso-setup itself
# needs root and /opt/denso). Changes NOTHING; only reads. Echoes a decision
# token and returns 0 for an actionable state, non-zero for a refusal:
#   seed                       (exit 0) — target absent, everything consistent: write it
#   current                    (exit 0) — target present and canonically equivalent: no-op
#   no-packaged-manifest       (exit 1) — no template installed
#   packaged-manifest-mismatch (exit 1) — template does not describe the packaged models
#   data-artifact-absent:<s>   (exit 1) — a packaged engine is missing from the data dir
#   data-artifact-differs:<s>  (exit 1) — a data-dir pair differs from the packaged/approved one
#   data-artifact-orphan:<s>   (exit 1) — a data-dir engine the manifest would not describe
#   packaged-pair-broken:<s>   (exit 1) — a packaged engine has no sidecar
#   target-symlink             (exit 1) — the data-dir manifest is a symlink
#   target-not-regular         (exit 1) — present but not a regular file
#   target-differs             (exit 1) — present and canonically different: never overwrite
#   target-unreadable          (exit 1) — present but malformed: never overwrite
seed_manifest_decide() (
    pkg_manifest="${1-}"; pkg_models="${2-}"; data_models="${3-}"; data_manifest="${4-}"
    [ -f "$pkg_manifest" ] || { echo "no-packaged-manifest"; return 1; }
    manifest_matches_models_dir "$pkg_manifest" "$pkg_models" >/dev/null 2>&1 \
        || { echo "packaged-manifest-mismatch"; return 1; }

    # Every packaged pair must be byte-identical in the data dir (the operator's
    # own engine must not get a manifest that declares the packaged one).
    for eng in "$pkg_models"/*.engine; do
        [ -e "$eng" ] || continue
        stem="$(basename "$eng" .engine)"; side="$pkg_models/$stem.names.json"
        [ -f "$side" ] || { echo "packaged-pair-broken:$stem"; return 1; }
        case "$(seed_decision_pair "$eng" "$side" "$data_models/$stem.engine" "$data_models/$stem.names.json")" in
            same)    : ;;
            seed)    echo "data-artifact-absent:$stem"; return 1 ;;
            differs) echo "data-artifact-differs:$stem"; return 1 ;;
            *)       echo "data-artifact-differs:$stem"; return 1 ;;
        esac
    done
    # ...and no data-dir engine may be one the packaged manifest does not describe.
    for eng in "$data_models"/*.engine; do
        [ -e "$eng" ] || continue
        stem="$(basename "$eng" .engine)"
        [ -f "$pkg_models/$stem.engine" ] || { echo "data-artifact-orphan:$stem"; return 1; }
    done

    if [ -L "$data_manifest" ]; then echo "target-symlink"; return 1; fi
    if [ -e "$data_manifest" ]; then
        [ -f "$data_manifest" ] || { echo "target-not-regular"; return 1; }
        manifests_equivalent "$pkg_manifest" "$data_manifest" >/dev/null 2>&1; mrc=$?
        case "$mrc" in
            0) echo "current"; return 0 ;;
            1) echo "target-differs"; return 1 ;;
            *) echo "target-unreadable"; return 1 ;;
        esac
    fi
    echo "seed"; return 0
)

# --- gdm_set_autologin <conf> <user> ----------------------------------------
# Edits ONLY the [daemon] section's two keys. Never templates the file: GDM's
# config carries admin settings we must not touch. Idempotent — dpkg may
# re-invoke a maintainer script during recovery.
gdm_set_autologin() (
    conf="${1-}"; user="${2-}"
    [ -w "$conf" ] || return 1
    [ -n "$user" ] || return 1
    tmp="$conf.denso.$$"
    awk -v user="$user" '
        BEGIN { in_daemon=0; done_enable=0; done_user=0; saw_daemon=0 }
        /^\[daemon\]/ {
            saw_daemon=1
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
            # A config with no [daemon] section at all is malformed/unexpected —
            # the naive version copied the input through and returned 0, so a
            # box with a weird custom.conf would report "autologin enabled"
            # while still stopping at the greeter on power-on. Refuse instead.
            if (!saw_daemon) exit 1
            if (in_daemon) {
                if (!done_enable) print "AutomaticLoginEnable=true"
                if (!done_user)   print "AutomaticLogin=" user
            }
        }
    ' "$conf" > "$tmp" || { rm -f "$tmp"; return 1; }

    # Never expose a partial config: `cat "$tmp" > "$conf"` truncates the LIVE
    # file before writing it — a disk-full or I/O error mid-copy leaves this
    # file (the operator's only way in, via the greeter) empty or partial on a
    # remote box. Build beside the original, carry its ownership + mode onto
    # the replacement (the rename swaps the inode), flush, then atomically
    # rename over it, then flush the directory entry too.
    # Fail HARD, never `|| true`: silently ignoring these would atomically
    # install /etc/gdm3/custom.conf with the wrong ownership or mode — a
    # correctly-shaped file that GDM may refuse, on the box whose desktop is the
    # operator's only way in. Verified available on both Ubuntu and MSYS2.
    chown --reference="$conf" "$tmp" || { rm -f "$tmp"; return 1; }
    chmod --reference="$conf" "$tmp" || { rm -f "$tmp"; return 1; }
    sync || { rm -f "$tmp"; return 1; }
    mv -f "$tmp" "$conf" || { rm -f "$tmp"; return 1; }
    sync || return 1
    return 0
)

# --- gdm_restore_autologin <conf> <denso-user> <orig-enable> <orig-user> -----
# Restores ONLY our two keys, and ONLY if BOTH still hold exactly what we set.
# If an admin changed either afterwards, refuse — a blind restore would silently
# revert their intent.
#
# Three distinct return codes — collapsing them to a boolean is the bug this
# fixes: the caller (denso-setup unconfigure) must never treat a genuine write
# failure the same as a benign admin divergence, or it will happily delete
# /opt/denso/install-state/ (the only record of the original GDM settings) after
# a restore that never actually happened.
#   0 = restored successfully
#   2 = admin divergence — current [daemon] values are not the ones we set, so
#       NOTHING was changed. Correct behavior, not a failure.
#   any other non-zero = real failure (unwritable conf, awk failed, temp file
#       or rename failed) — the caller must refuse to proceed on this, not
#       shrug it off like case 2.
#
# <denso-user> is passed EXPLICITLY. An earlier draft secretly read
# /opt/denso/install-state/autologin.user from inside this "pure" function; with
# that file absent it fell back to the CURRENT value, which made the
# admin-changed-it test compare a value to itself and pass — i.e. it would
# happily overwrite the admin's new user. Impure and wrong.
gdm_restore_autologin() (
    conf="${1-}"; denso_user="${2-}"; orig_enable="${3-}"; orig_user="${4-}"
    [ -w "$conf" ] || return 1

    cur_user="$(awk '/^\[daemon\]/{d=1;next} /^\[/{d=0} d&&/^AutomaticLogin=/{sub(/^AutomaticLogin=/,"");print;exit}' "$conf")"
    cur_en="$(awk '/^\[daemon\]/{d=1;next} /^\[/{d=0} d&&/^AutomaticLoginEnable=/{sub(/^AutomaticLoginEnable=/,"");print;exit}' "$conf")"
    # BOTH must still be ours. Checking only the user let an admin's change to
    # the ENABLE key be silently overwritten.
    if [ "$cur_user" != "$denso_user" ] || [ "$cur_en" != "true" ]; then
        echo "gdm: refusing to restore — [daemon] now has AutomaticLogin='$cur_user' Enable='$cur_en', not the '$denso_user'/true we set" >&2
        return 2
    fi

    tmp="$conf.denso.$$"
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

    # Same atomic-replace as gdm_set_autologin — never expose a partial config.
    # Fail HARD, never `|| true`: silently ignoring these would atomically
    # install /etc/gdm3/custom.conf with the wrong ownership or mode — a
    # correctly-shaped file that GDM may refuse, on the box whose desktop is the
    # operator's only way in. Verified available on both Ubuntu and MSYS2.
    chown --reference="$conf" "$tmp" || { rm -f "$tmp"; return 1; }
    chmod --reference="$conf" "$tmp" || { rm -f "$tmp"; return 1; }
    sync || { rm -f "$tmp"; return 1; }
    mv -f "$tmp" "$conf" || { rm -f "$tmp"; return 1; }
    sync || return 1
    return 0
)
