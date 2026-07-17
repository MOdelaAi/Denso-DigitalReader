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
    chown --reference="$conf" "$tmp" 2>/dev/null || true
    chmod --reference="$conf" "$tmp" 2>/dev/null || true
    sync
    mv -f "$tmp" "$conf" || { rm -f "$tmp"; return 1; }
    sync
    return 0
)

# --- gdm_restore_autologin <conf> <denso-user> <orig-enable> <orig-user> -----
# Restores ONLY our two keys, and ONLY if BOTH still hold exactly what we set.
# If an admin changed either afterwards, refuse — a blind restore would silently
# revert their intent.
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
        return 1
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
    chown --reference="$conf" "$tmp" 2>/dev/null || true
    chmod --reference="$conf" "$tmp" 2>/dev/null || true
    sync
    mv -f "$tmp" "$conf" || { rm -f "$tmp"; return 1; }
    sync
    return 0
)
