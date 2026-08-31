#!/usr/bin/env bash
# Fresh-install manifest seeding, end to end, on real hardware with the REAL
# packaged TensorRT engines — the gate tests/packaging/run.sh cannot be.
#
# run.sh proves the DECISION logic with fixture bytes on any box. It cannot prove
# the thing the incident was actually about: that after a fresh install the
# RUNTIME reports READY instead of DEGRADED. Only a real engine deserialising on
# a real GPU answers that, so this script exists and is Jetson-only.
#
# ISOLATION: everything happens in a throwaway $DENSO_DATA_DIR. The packaged
# artifacts under /opt/denso are READ, never written. The live appliance data dir
# /opt/denso/data is never touched, and no service is started or stopped. Safe to
# run on the appliance that is also the dev box (see AGENTS.md on the two roles).
#
#   usage: tests/manual/fresh_install_manifest.sh [path-to-denso-binary]
#          default binary: build/src/app/denso (this working tree)
set -u

REPO="$(cd "$(dirname "$0")/../.." && pwd)"
DENSO_BIN="${1:-$REPO/build/src/app/denso}"
PKG_MODELS=/opt/denso/models
PKG_MANIFEST=/opt/denso/lib/manifest.json

pass=0; fail_n=0
ok()  { echo "ok   - $1"; pass=$((pass+1)); }
bad() { echo "FAIL - $1"; fail_n=$((fail_n+1)); }
is()  { if [ "$2" = "$3" ]; then ok "$1"; else bad "$1 (want '$3', got '$2')"; fi; }

[ "$(uname -m)" = "aarch64" ] || { echo "skip - not aarch64: this gate needs the real engines"; exit 0; }
[ -x "$DENSO_BIN" ]   || { echo "FAIL - no denso binary at $DENSO_BIN"; exit 1; }
[ -d "$PKG_MODELS" ]  || { echo "FAIL - no packaged models at $PKG_MODELS (install the .deb first)"; exit 1; }
[ -f "$PKG_MANIFEST" ]|| { echo "FAIL - no packaged manifest at $PKG_MANIFEST (install the .deb first)"; exit 1; }

. "$REPO/packaging/lib/policy.sh"

SB="$(mktemp -d)"; trap 'rm -rf "$SB"' EXIT
DATA="$SB/data"
export DATA PKG_MODELS PKG_MANIFEST

echo "== sandbox: $DATA   binary: $DENSO_BIN"

# The stub stands in for the INSTALLED denso-setup, whose act path is already
# covered by tests/packaging/run.sh. It does what cmd_seed_manifest does, minus
# need_root and the runuser hop (this runs unprivileged, as one user, by design).
mkdir -p "$SB/bin"
cat > "$SB/bin/denso-setup" <<'STUBEOF'
#!/bin/sh
[ "$1" = "seed-manifest" ] || { echo "stub: unexpected subcommand $1" >&2; exit 64; }
echo called >> "$SB_CALLS"
. /opt/denso/lib/policy.sh
d="$(seed_manifest_decide "$PKG_MANIFEST" "$PKG_MODELS" "$DATA/models" "$DATA/models/manifest.json")"
case "$d" in
    seed)    install_manifest_atomic "$PKG_MANIFEST" "$DATA/models/manifest.json" ;;
    current) exit 0 ;;
    *)       echo "stub: refusing, decision=$d" >&2; exit 1 ;;
esac
STUBEOF
chmod 0755 "$SB/bin/denso-setup"
SB_CALLS="$SB/stub-calls"; export SB_CALLS
PATH="$SB/bin:$PATH"; export PATH

# ── step 1: what postinst does — create the data dir ─────────────────────────
mkdir -p "$DATA/models"

# ── step 2: what `denso-setup configure` does — seed the packaged pairs ──────
seeded=0
for eng in "$PKG_MODELS"/*.engine; do
    [ -e "$eng" ] || continue
    stem="$(basename "$eng" .engine)"
    install_pair "$eng" "$PKG_MODELS/$stem.names.json" "$DATA/models" || bad "could not seed $stem"
    seeded=$((seeded+1))
done
[ "$seeded" -ge 1 ] && ok "configure seeded $seeded packaged pair(s)" || bad "no packaged pairs to seed"

# ── the BUG, reproduced: engines present, manifest absent ────────────────────
[ -f "$DATA/models/manifest.json" ] \
    && bad "precondition: no manifest before seeding" \
    || ok  "precondition: engines are present and manifest.json is absent"
DENSO_DATA_DIR="$DATA" QT_QPA_PLATFORM=offscreen "$DENSO_BIN" --check >"$SB/before.txt" 2>&1
BEFORE_RC=$?
is "BEFORE the fix, the runtime reports DEGRADED (exit 10)" "$BEFORE_RC" "10"
grep -q 'degraded' "$SB/before.txt" \
    && ok  "BEFORE: --check names the degraded engines" \
    || bad "BEFORE: --check names the degraded engines"

# ── step 3: the fix — postinst's own seeding block, extracted verbatim ───────
awk '/── manifest seeding ─/{inb=1} /── setup verification ─/{inb=0} inb{print}' \
    "$REPO/packaging/debian/postinst" > "$SB/seed-block.sh"
[ -s "$SB/seed-block.sh" ] && ok "postinst's seeding block extracted" || bad "postinst's seeding block extracted"

FAILSTR="$(sh -c '
    set -e
    . /opt/denso/lib/policy.sh
    . '"$REPO"'/packaging/lib/policy.sh
    . '"$SB"'/seed-block.sh
    printf "%s" "$fail"
' 2>"$SB/seed-err.txt")"
is "the seeding block reports no failure" "$FAILSTR" ""
[ -f "$SB_CALLS" ] \
    && ok  "the fresh-install path invoked seed-manifest by itself" \
    || bad "the fresh-install path invoked seed-manifest by itself"

# ── the contract the operator was promised ───────────────────────────────────
[ -f "$DATA/models/manifest.json" ] \
    && ok  "manifest.json exists with NO manual seed-manifest run" \
    || bad "manifest.json exists with NO manual seed-manifest run"
for f in digitv3.engine digitv3.names.json float-big.engine float-big.names.json \
         float-small.engine float-small.names.json manifest.json; do
    [ -f "$DATA/models/$f" ] && ok "present: $f" || bad "present: $f"
done
manifest_matches_models_dir "$DATA/models/manifest.json" "$DATA/models" >/dev/null 2>&1 \
    && ok  "every artifact in the data dir is DECLARED by the seeded manifest" \
    || bad "every artifact in the data dir is DECLARED by the seeded manifest"

# ── AFTER: the runtime verdict, through real TensorRT ────────────────────────
DENSO_DATA_DIR="$DATA" QT_QPA_PLATFORM=offscreen "$DENSO_BIN" --check >"$SB/after.txt" 2>&1
AFTER_RC=$?
is "AFTER the fix, the runtime reports READY (exit 0)" "$AFTER_RC" "0"
grep -q 'degraded' "$SB/after.txt" \
    && bad "AFTER: no engine is left degraded" \
    || ok  "AFTER: no engine is left degraded"

# A bare --check validates only what the DB references, and this sandbox has no
# database yet — exactly like a real fresh install before its first launch. So
# READY above proves the DECLARATION reached the runtime, not that the plans
# load. `--check --engine` exists for precisely this: force every artifact
# through real TensorRT deserialisation even when nothing references it.
ENGARGS=""
for eng in "$DATA"/models/*.engine; do ENGARGS="$ENGARGS --engine $(basename "$eng")"; done
# shellcheck disable=SC2086
DENSO_DATA_DIR="$DATA" QT_QPA_PLATFORM=offscreen "$DENSO_BIN" --check $ENGARGS \
    >"$SB/after-deep.txt" 2>&1
DEEP_RC=$?
is "AFTER: a forced deep check of every engine is READY (exit 0)" "$DEEP_RC" "0"
grep -q 'engines load ok' "$SB/after-deep.txt" \
    && ok  "AFTER: every engine deserialised and validated on the GPU" \
    || bad "AFTER: every engine deserialised and validated on the GPU"

# ── idempotence: dpkg --configure -a, or a second apt install ────────────────
MB="$(sha256sum "$DATA/models/manifest.json" | cut -d' ' -f1)"
FAILSTR2="$(sh -c '
    set -e
    . /opt/denso/lib/policy.sh
    . '"$REPO"'/packaging/lib/policy.sh
    . '"$SB"'/seed-block.sh
    printf "%s" "$fail"
' 2>/dev/null)"
is "a second run reports no failure"          "$FAILSTR2" ""
is "a second run leaves the manifest bytes unchanged" \
   "$(sha256sum "$DATA/models/manifest.json" | cut -d' ' -f1)" "$MB"

echo
echo "--- BEFORE ---"; sed 's/^/    /' "$SB/before.txt"
echo "--- AFTER  ---"; sed 's/^/    /' "$SB/after.txt"
echo "--- AFTER (forced deep engine check) ---"; sed 's/^/    /' "$SB/after-deep.txt"
echo
echo "passed: $pass   failed: $fail_n"
[ "$fail_n" -eq 0 ]
