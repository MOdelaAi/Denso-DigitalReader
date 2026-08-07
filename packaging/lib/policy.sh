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

# --- operator_user_ok <passwd-file> <name> -----------------------------------
# Is <name> acceptable as THE Denso operator account?
#
# Takes the passwd file as a parameter rather than calling getent: it makes the
# rule drivable from a fixture in tests/packaging/run.sh, and it is the more
# correct reading of the requirement anyway — the operator must be a LOCAL user,
# and /etc/passwd is exactly the local database.
#
# This checks ACCOUNT SHAPE only. Whether the home directory actually exists on
# disk is checked by denso-setup's cmd_configure, which is where the filesystem
# lives; keeping that out of here is what lets this rule stay pure.
#
# Rejected, roughly in order of how badly each would end:
#   root      running the GUI and the database as root is the worst outcome —
#             it poisons an operator-owned data dir permanently
#   nobody    the classic "safe" default that is not a person and owns nothing
#   uid <1000 or >60000 — system/service accounts, and nobody(65534), per the
#             target's own /etc/login.defs UID_MIN/UID_MAX
#   nologin / false shells — a service account whatever its uid
#   missing or /nonexistent home — nowhere to put an autostart entry
#   unknown   not in the local database at all
operator_user_ok() (
    pw="${1-}"; name="${2-}"
    [ -n "$pw" ] && [ -n "$name" ] || return 1
    [ -r "$pw" ] || return 1

    # Named outright, not left to the uid range below: the range already
    # excludes root(0) and nobody(65534), but a box with a renumbered nobody or
    # a second uid-0 account under another name must still be refused.
    case "$name" in
        root|nobody|nogroup|daemon|bin|sys|"") return 1 ;;
        *:*|*" "*|-*) return 1 ;;          # not a plausible login name
    esac

    line="$(awk -F: -v n="$name" '$1 == n { print; exit }' "$pw")"
    [ -n "$line" ] || return 1              # nonexistent

    uid="$(printf '%s' "$line" | cut -d: -f3)"
    home="$(printf '%s' "$line" | cut -d: -f6)"
    ushell="$(printf '%s' "$line" | cut -d: -f7)"

    case "$uid" in ''|*[!0-9]*) return 1 ;; esac
    [ "$uid" -ge 1000 ]  || return 1        # system/service account
    [ "$uid" -le 60000 ] || return 1        # excludes nobody (65534)

    case "$ushell" in
        ""|*/nologin|*/false|*/sync) return 1 ;;
    esac
    case "$home" in
        ""|/|/nonexistent|/dev/null|/usr/sbin) return 1 ;;
    esac
    return 0
)

# --- resolve_operator_user <passwd-file> <recorded> <sudo-user> [session…] ----
# Decide THE one operator account, or fail. Echoes "<user> <rule>" so the caller
# can report which precedence rule fired — worth printing in the apt output,
# because "which user did it pick, and why" is the first question asked when it
# ever picks wrong.
#
# Precedence:
#   1. the already-recorded user — an UPGRADE must never be re-pointed at
#      whoever happens to be running sudo today. If a user IS recorded but is no
#      longer valid, that is a HARD FAILURE, not a fall-through to rule 2:
#      falling through is precisely how an upgrade would silently steal an
#      appliance from its operator.
#   2. SUDO_USER — the normal interactive `sudo apt install` case.
#   3. exactly ONE acceptable local, active, non-remote session user.
#
# Never guesses. Zero candidates and several candidates are both failures, and
# the caller is expected to fail the installation rather than pick one.
resolve_operator_user() (
    pw="${1-}"; recorded="${2-}"; sudo_user="${3-}"
    shift 3 2>/dev/null || true

    if [ -n "$recorded" ]; then
        if operator_user_ok "$pw" "$recorded"; then
            echo "$recorded recorded"
            return 0
        fi
        echo "operator-user: this installation is recorded for '$recorded', which is no" >&2
        echo "operator-user: longer a valid operator account. Refusing to silently adopt a" >&2
        echo "operator-user: different user — that would hand the appliance to whoever ran" >&2
        echo "operator-user: sudo. Fix the account, or run 'sudo denso-setup unconfigure'." >&2
        return 1
    fi

    if [ -n "$sudo_user" ] && operator_user_ok "$pw" "$sudo_user"; then
        echo "$sudo_user sudo_user"
        return 0
    fi

    cands=""
    for u in "$@"; do
        [ -n "$u" ] || continue
        operator_user_ok "$pw" "$u" || continue
        case " $cands " in *" $u "*) continue ;; esac    # de-duplicate
        cands="$cands $u"
    done

    # shellcheck disable=SC2086
    set -- $cands
    case "$#" in
        1) echo "$1 session"; return 0 ;;
        0) echo "operator-user: no acceptable operator account could be determined." >&2
           echo "operator-user: SUDO_USER was unset or unusable, and no single local active" >&2
           echo "operator-user: session belongs to a normal user. Re-run under sudo as the" >&2
           echo "operator-user: operator, or run 'sudo denso-setup configure --user <name>'." >&2
           return 1 ;;
        *) echo "operator-user: ambiguous — more than one local session user qualifies:$cands" >&2
           echo "operator-user: refusing to guess which one owns this appliance. Run" >&2
           echo "operator-user: 'sudo denso-setup configure --user <name>' to say explicitly." >&2
           return 1 ;;
    esac
)

# --- user_unit_wants_link <home> <unit-name> ---------------------------------
# Where `systemctl --user enable` puts the symlink for a unit whose [Install]
# says WantedBy=graphical-session.target. One definition, so the enable path,
# the disable path and the tests can never disagree about it.
user_unit_wants_link() (
    echo "${1-}/.config/systemd/user/graphical-session.target.wants/${2-}"
)

# --- enable_user_unit <unit-path> <home> <user> <group> ----------------------
# Enable a systemd USER unit for an operator who is very likely NOT logged in.
#
# It creates the .wants symlink directly instead of calling
# `systemctl --user enable`, because during `apt install` the target user
# usually has no running `systemd --user` instance and no session bus to talk
# to. This was measured on the target, not assumed: a hand-made symlink is
# reported `enabled` by `systemctl --user is-enabled`, is removed by
# `systemctl --user disable`, and can be re-created by `systemctl --user
# enable` afterwards — so the operator's normal commands keep working.
#
# Echoes the created link path.
enable_user_unit() (
    unit="${1-}"; home="${2-}"; user="${3-}"; group="${4-}"
    [ -n "$unit" ] && [ -n "$home" ] && [ -n "$user" ] && [ -n "$group" ] \
        || { echo "enable-unit: missing argument" >&2; return 1; }
    [ -f "$unit" ] || { echo "enable-unit: no such unit file: $unit" >&2; return 1; }

    name="$(basename "$unit")"
    link="$(user_unit_wants_link "$home" "$name")"

    # Create ONLY what is missing, and never chmod/chown a directory that
    # already exists: ~/.config may legitimately be 0700, and an `install -d`
    # over the whole path would silently widen it to 0755. Ownership is set as
    # part of the create — write-then-chown would leave a root-owned directory
    # in the operator's home if interrupted between the two steps.
    for d in "$home/.config" "$home/.config/systemd" "$home/.config/systemd/user" \
             "$(dirname "$link")"; do
        [ -d "$d" ] && continue
        install -d -o "$user" -g "$group" -m 0755 "$d" \
            || { echo "enable-unit: cannot create $d" >&2; return 1; }
    done

    ln -sfn "$unit" "$link" || { echo "enable-unit: cannot link $link" >&2; return 1; }
    # -h: chown the SYMLINK, not the /usr/lib unit file it points at.
    chown -h "$user":"$group" "$link" 2>/dev/null || true
    echo "$link"
)

# --- disable_user_unit <unit-name> <home> ------------------------------------
# The inverse. Removes only Denso's own .wants symlink; any other unit the
# operator has enabled into graphical-session.target is left alone, and the
# .wants directory itself is kept (systemctl leaves it too).
disable_user_unit() (
    name="${1-}"; home="${2-}"
    [ -n "$name" ] && [ -n "$home" ] || { echo "disable-unit: missing argument" >&2; return 1; }
    rm -f "$(user_unit_wants_link "$home" "$name")"
)

# --- migrate_xdg_autostart <state-dir> <home> <user> <group> <unit-path> -----
# One-time migration from the old XDG-autostart architecture to the systemd
# user unit. Echoes what it did: "enabled" | "preserved-disabled" | "already".
#
# The marker is the whole point. Without it, every future .deb upgrade would
# re-enable the service, silently undoing a deliberate
# `systemctl --user disable` — an upgrade must never re-arm something the
# operator switched off.
#
# The legacy entry's ABSENCE is read as intent, not as an accident: an existing
# installation that has no Denso autostart entry is one where autostart was
# turned off, so the new unit is left disabled. (A FRESH install is a different
# case entirely and does not come through here — it always enables.)
#
# Exactly one named file is ever deleted, so unrelated entries in
# ~/.config/autostart are untouched.
migrate_xdg_autostart() (
    state="${1-}"; home="${2-}"; user="${3-}"; group="${4-}"; unit="${5-}"
    [ -n "$state" ] && [ -n "$home" ] && [ -n "$user" ] && [ -n "$group" ] && [ -n "$unit" ] \
        || { echo "migrate-autostart: missing argument" >&2; return 1; }

    marker="$state/autostart-migrated"
    if [ -e "$marker" ]; then
        echo "already"
        return 0
    fi

    legacy="$home/.config/autostart/com.denso.DigitalReader.desktop"
    if [ -f "$legacy" ]; then
        rm -f "$legacy" || { echo "migrate-autostart: cannot remove $legacy" >&2; return 1; }
        enable_user_unit "$unit" "$home" "$user" "$group" >/dev/null || return 1
        result="enabled"
    else
        result="preserved-disabled"
    fi

    # Written LAST, as the commit marker: a failure above must leave the
    # migration un-marked so the next `dpkg --configure -a` retries it.
    printf '%s\n' "$result" > "$marker" \
        || { echo "migrate-autostart: cannot write $marker" >&2; return 1; }
    echo "$result"
)

# --- canonical_model_stems ---------------------------------------------------
# The canonical production model set, in REVIEWED MANIFEST ORDER.
#
# Order is not cosmetic: gen_model_manifest.py emits generations in the order it
# is given them, so the order is part of the manifest bytes and therefore part of
# the pinned Release-B identity (build_package.sh RELEASE_B_MANIFEST_SHA256).
# Emitting these stems in any other order would produce a manifest nobody
# reviewed and silently skip the identity assert.
#
# Both product modes are represented, and the split is fixed by the C++ registry
# (src/core/models/compatibility.cpp), NOT by this list:
#   digitv3                  -> digit_numeric -> Digital Number Reader
#   float-small, float-big   -> float_ball    -> Floating Ball Leveler
# Nothing here widens a model's authorization; the registry is the only thing
# that decides what a model may do.
canonical_model_stems() (
    echo "digitv3 float-small float-big"
)

# --- resolve_models_dir <dir> ------------------------------------------------
# Echo the canonical engine paths, one per line, in reviewed manifest order, so
# `build_package.sh --models-dir <dir>` cannot assemble a partial release.
#
# Why this exists: `--model` takes one engine at a time, which makes a PARTIAL
# release set the easy mistake — a package built with digitv3 alone is valid,
# installable, and silently missing the entire Floating Ball Leveler mode. This
# refuses anything that is not exactly the canonical set.
#
# It deliberately does NOT glob the directory into the model list (see
# packaging/models.approved): the directory is CHECKED against the canonical set,
# never used to discover it, so a stray engine is an error rather than a silent
# addition. Hash approval still happens downstream, per pair.
#
# Returns 0 and prints the ordered engine paths; returns 1 with a message on
# stderr otherwise. Subshell body — see version_ok.
resolve_models_dir() (
    dir="${1-}"
    [ -n "$dir" ] || { echo "models-dir: no directory given" >&2; return 1; }
    [ -d "$dir" ] || { echo "models-dir: not a directory: $dir" >&2; return 1; }

    # Production packaging is TensorRT-engine only. A checkpoint or an ONNX
    # export sitting beside the engines is a sign the directory is a working
    # area, not a release input — refuse rather than quietly ignore it.
    for bad in "$dir"/*.pt "$dir"/*.onnx; do
        [ -e "$bad" ] || continue
        echo "models-dir: refusing '$bad' — packaging is TensorRT-engine only" >&2
        return 1
    done

    stems="$(canonical_model_stems)"

    # Every canonical PAIR must be present. The pair is the unit: TrtEngine's
    # ctor reads <stem>.names.json, so an engine without its sidecar is not a
    # shippable model.
    for s in $stems; do
        [ -f "$dir/$s.engine" ] \
            || { echo "models-dir: missing engine: $dir/$s.engine" >&2; return 1; }
        [ -f "$dir/$s.names.json" ] \
            || { echo "models-dir: missing sidecar: $dir/$s.names.json" >&2; return 1; }
    done

    # No engine outside the canonical set. This is the "unexpected fourth
    # engine" case: models/ is git-ignored, so a forgotten experimental engine
    # with a valid sidecar would otherwise be a candidate for production.
    for e in "$dir"/*.engine; do
        [ -e "$e" ] || continue
        st="$(basename "$e" .engine)"
        case " $stems " in
            *" $st "*) : ;;
            *) echo "models-dir: unexpected engine '$st' in $dir" >&2
               echo "models-dir: the canonical set is exactly: $stems" >&2
               return 1 ;;
        esac
    done

    for s in $stems; do echo "$dir/$s.engine"; done
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

# --- migrate_verdict <exit-code> ---------------------------------------------
# Map `denso --apply-migrations` (src/app/cli/run_headless.cpp) to a postinst
# action. Deliberately NOT check_verdict: the two commands do not share an exit
# contract, and reusing it would silently accept 10 — which --apply-migrations
# never returns and which would mean "carry on" on a code we do not model.
#   0  = at the supported schema (migrated, already current, or fresh) -> "ok"
#   78 = Blocked: unreadable, newer-than-supported, or the chain FAILED
#        -> "blocked"  postinst STOPS, leaves the app stopped, exits non-zero
#   *  = anything else (crash, 127 missing binary, 2 bad usage) -> "failed"
# There is no non-fatal verdict here: unlike --check's Degraded, a partly
# migrated database has no serviceable middle state.
migrate_verdict() (
    case "${1-}" in
        0)  echo ok ;;
        78) echo blocked ;;
        *)  echo failed ;;
    esac
)

# --- user_version_ok <string> ------------------------------------------------
# Validate what `denso-db-helper user-version` printed BEFORE it is used to build
# a backup filename or compared as a number. Fail closed on anything that is not
# a plain non-negative integer: an empty string (helper missing, DB unreadable,
# or the PRAGMA silently failing) would otherwise produce the backup name
# "denso.db.pre-v" and quietly collapse every schema version onto ONE file — so
# the second failed upgrade would find that name already present, skip the
# backup, and migrate with no recovery point at all.
user_version_ok() (
    case "${1-}" in
        "") return 1 ;;
        *[!0-9]*) return 1 ;;    # digits only: no sign, no space, no newline
        *) return 0 ;;
    esac
)

# --- backup_basename <user-version> ------------------------------------------
# The pre-migration backup filename, keyed on the schema version being LEFT.
#
# Deliberately deterministic — no timestamp. Two properties depend on it:
#   1. `dpkg --configure -a` re-runs postinst after a failed upgrade. A
#      timestamped name would take a SECOND backup, this time of the
#      half-migrated database, and (worse) the operator's real recovery point
#      would no longer be the newest file in the directory.
#   2. Reinstalling the same package repeatedly cannot grow the directory: the
#      name only changes when the schema version does, so the backup count is
#      bounded by the number of schema versions the appliance has passed
#      through. That is what makes "no automatic pruning" safe to promise.
# The caller must never overwrite an existing one — see postinst.
backup_basename() (
    echo "denso.db.pre-v${1-}"
)

# --- db_upgrade_gate <data-dir> <db> <denso-bin> <runner> <helper> -----------
# The forward-only upgrade gate: confirm stopped, back up, PROVE the backup,
# migrate, confirm the database survived, verify. Returns 0 to let the upgrade
# proceed, 1 to HALT it (postinst then exits non-zero, leaving the package
# unconfigured and the application stopped).
#
# `runner` is the command prefix every database operation runs through, passed
# as ONE word-split string, e.g. "runuser -u denso --". It is a parameter and
# not a hardcoded `runuser` for two reasons: none of this may run as root
# (opening a WAL database creates -wal/-shm beside it, and `--check` writes a
# probe file into the data dir — a root-owned artifact in an operator-owned data
# dir is the documented way this appliance breaks; see prerm, where a root-owned
# lock makes every later --check-running return 4), and the tests need to drive
# the real logic as an unprivileged user. An EMPTY runner would silently mean
# "as root" and is refused outright.
#
# `helper` is denso-db-helper, which wraps the Python 3 stdlib sqlite3 module.
# There is no `sqlite3` CLI dependency: python3 is already required, and adding
# a package would put an apt fetch in the middle of an offline .deb upgrade.
#
# ORDER IS THE DESIGN: back up FIRST, prove the backup is a usable recovery
# point SECOND, and only then let anything write to the live database. A backup
# taken after a failed migration is not a backup.
#
# It never restores, never rolls back, and never deletes or overwrites an
# existing backup. Recovery is a deliberate manual act, by requirement.
db_upgrade_gate() (
    data="${1-}"; db="${2-}"; denso="${3-}"; runner="${4-}"; helper="${5-}"

    backup=""
    halt() {
        echo "denso: UPGRADE HALTED: $1" >&2
        echo "denso:" >&2
        echo "denso: The application has NOT been started and must stay stopped." >&2
        if [ -n "$backup" ] && [ -e "$backup" ]; then
            echo "denso: Pre-migration backup (NOT restored automatically):" >&2
            echo "denso:   $backup" >&2
            echo "denso: To recover by hand, with the application stopped, as the" >&2
            echo "denso: account that owns the data dir:" >&2
            echo "denso:   cp -p '$backup' '$db'" >&2
            echo "denso:   rm -f '$db-wal' '$db-shm'" >&2
        else
            echo "denso: No pre-migration backup was taken; the database is untouched." >&2
        fi
        echo "denso:" >&2
        echo "denso: After fixing the cause, re-run:  sudo dpkg --configure -a" >&2
        return 1
    }

    [ -n "$runner" ] || { halt "internal error: no privilege-dropping runner was
 supplied, so database operations would run as root."; return 1; }
    [ -n "$data" ] && [ -n "$db" ] && [ -n "$denso" ] && [ -n "$helper" ] \
        || { halt "internal error: db_upgrade_gate called with missing paths."; return 1; }
    [ -x "$denso" ] || { halt "$denso is missing or not executable."; return 1; }
    [ -f "$helper" ] || { halt "$helper is missing."; return 1; }
    command -v python3 >/dev/null 2>&1 || { halt "python3 is not installed. It is
 a package dependency; run 'sudo apt-get -f install' and retry."; return 1; }

    # --- the application must be stopped --------------------------------------
    # prerm already refused the upgrade if it was running, but that check ran
    # before unpacking; this one runs immediately before we touch the database.
    # --check-running is TRI-STATE and read as such: 1 (definitely not running)
    # is the ONLY safe-to-proceed code. Treating 4 ("cannot determine") as safe
    # is exactly the unsafe upgrade the tri-state exists to prevent.
    $runner env DENSO_DATA_DIR="$data" "$denso" --check-running >/dev/null 2>&1
    rc=$?
    case "$rc" in
        1) : ;;
        0) halt "the application is running. Stop it, then re-run
 'sudo dpkg --configure -a'."; return 1 ;;
        *) halt "cannot establish whether the application is running (rc=$rc);
 refusing to migrate. Check /opt/denso/data ownership and permissions."; return 1 ;;
    esac

    # --- the schema version currently on disk ---------------------------------
    cur="$($runner python3 "$helper" user-version "$db" 2>/dev/null)"
    rc=$?
    [ "$rc" -eq 0 ] || { halt "cannot read the schema version from $db (helper
 rc=$rc); the database may be missing, unreadable or corrupt."; return 1; }
    user_version_ok "$cur" || { halt "the schema version read from $db is not a
 plain integer (got '$cur'); refusing to guess a backup name."; return 1; }

    # --- exactly one pre-migration backup -------------------------------------
    backup="$data/$(backup_basename "$cur")"
    if [ -e "$backup" ]; then
        # A retry after a failed upgrade. Keeping the ORIGINAL is the whole point
        # of the deterministic name: overwriting now would replace the operator's
        # only recovery point with a copy of the half-migrated database.
        echo "denso: a pre-migration backup for schema v$cur already exists;"
        echo "denso: keeping it unchanged: $backup"
    else
        part="$backup.partial"
        rm -f "$part"
        echo "denso: backing up the database at schema v$cur -> $backup"
        # The helper uses the SQLite online backup API (WAL-consistent) and
        # verifies the snapshot's own integrity_check and user_version before
        # returning 0. It refuses a destination that already exists.
        $runner python3 "$helper" backup "$db" "$part"
        rc=$?
        if [ "$rc" -ne 0 ]; then
            rm -f "$part"
            halt "the pre-migration backup could not be created and verified
 (helper rc=$rc)."
            return 1
        fi
        # Rename only once verified: a crash mid-copy must not leave a truncated
        # file at $backup that the retry above would trust and skip.
        mv "$part" "$backup" || { halt "cannot move the verified backup into place."; return 1; }
        echo "denso: backup verified (integrity_check ok, schema v$cur)."
    fi

    # --- migrate the live database --------------------------------------------
    # The one production migration primitive. It refuses a database written by a
    # NEWER build before opening it for writing (forward-only), and returns 78
    # for every blocked state.
    echo "denso: applying migrations..."
    $runner env DENSO_DATA_DIR="$data" "$denso" --apply-migrations
    rc=$?
    case "$(migrate_verdict "$rc")" in
        ok) : ;;
        blocked) halt "migration was refused or failed (rc=$rc). This includes a
 database written by a NEWER version of the application than the one just
 installed — downgrades are not supported."; return 1 ;;
        *) halt "the migration command failed unexpectedly (rc=$rc)."; return 1 ;;
    esac

    # --- the database must still be there -------------------------------------
    # A database that is ABSENT when the gate first classifies the install is a
    # fresh install: nothing to migrate, and postinst never calls this function.
    # A database that is absent HERE is a different event entirely — it existed
    # moments ago, we backed it up, and it has since gone. `--apply-migrations`
    # cannot tell those apart (an absent database is a legitimate no-op for a
    # fresh install, so it returns 0 and says so), which is precisely why the
    # distinction has to be drawn here, where the earlier observation is known.
    # Accepting the exit code alone would report a successful upgrade of a
    # database that no longer exists.
    [ -f "$db" ] || { halt "the database $db disappeared during migration.
 It existed at schema v$cur moments ago and was backed up.
 This is not a fresh install and must not be treated as one."; return 1; }

    now="$($runner python3 "$helper" user-version "$db" 2>/dev/null)"
    rc=$?
    [ "$rc" -eq 0 ] || { halt "the database $db is unreadable immediately after
 migration (helper rc=$rc)."; return 1; }
    user_version_ok "$now" || { halt "the schema version after migration is not a
 plain integer (got '$now')."; return 1; }
    [ "$now" -ge "$cur" ] || { halt "the schema version went BACKWARDS during
 migration: v$cur -> v$now."; return 1; }
    echo "denso: schema v$cur -> v$now."

    # --- integrity verification ------------------------------------------------
    # Exit 10 (Degraded) is NOT an upgrade failure: it means a per-camera fault
    # such as a rejected model attachment. Halting a four-camera appliance
    # because one camera has a bad attachment would invert the per-zone
    # fail-closed contract the readiness verdict exists to express.
    echo "denso: verifying integrity..."
    $runner env DENSO_DATA_DIR="$data" "$denso" --check
    rc=$?
    case "$(check_verdict "$rc")" in
        ok) echo "denso: integrity ok." ;;
        degraded)
            echo "denso: integrity DEGRADED (rc=10) — serviceable; the application" >&2
            echo "denso: will start. Run 'sudo denso-setup verify' for per-camera detail." >&2 ;;
        blocked) halt "integrity verification reported a blocking configuration
 fault (rc=78)."; return 1 ;;
        *) halt "integrity verification failed unexpectedly (rc=$rc)."; return 1 ;;
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
