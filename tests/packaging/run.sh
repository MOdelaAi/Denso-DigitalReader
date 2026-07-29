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
apt_plan_ok "$T/cuda" >/dev/null; rc_is "apt: a MISSING JetPack component is refused, not auto-installed" $? 1
apt_plan_ok "$T/cuda" 2>&1 | grep -q "Restore the supported JetPack" \
  && ok "apt: the refusal tells the operator what to DO" || bad "apt: the refusal tells the operator what to DO"

printf 'Inst nvidia-l4t-core (36.4.0 [arm64])\n' > "$T/l4t"
apt_plan_ok "$T/l4t" >/dev/null; rc_is "apt: touching nvidia-l4t-* is refused" $? 1

printf 'Inst libopencv-core4.5d (4.5.4+dfsg-9ubuntu4 Ubuntu:22.04/jammy [arm64])\n' > "$T/ubuntucv"
apt_plan_ok "$T/ubuntucv" >/dev/null; rc_is "apt: Ubuntu OpenCV is refused (must not displace NVIDIA 4.8)" $? 1

printf 'Inst libopencv (4.8.0-1-g6371ee1 [arm64])\n' > "$T/nvcv"
apt_plan_ok "$T/nvcv" >/dev/null; rc_is "apt: NVIDIA libopencv itself is NOT the Ubuntu one" $? 0

# ── check_verdict: map `denso --check`'s exit code to a verify action. The
# readiness contract is 0 Ready / 10 Degraded-serviceable / 78 Blocked; verify
# must warn-and-continue on Degraded (the unmanifested-engines production Jetson),
# STOP on a Blocked configuration fault, and treat any other non-zero as an
# unexpected check failure — never silently continue on an unknown code.
is "check: 0 -> ok (continue)"                 "$(check_verdict 0)"  ok
is "check: 10 -> degraded (warn, continue)"    "$(check_verdict 10)" degraded
is "check: 78 -> blocked (stop)"               "$(check_verdict 78)" blocked
is "check: 1 -> failed (unexpected)"           "$(check_verdict 1)"  failed
is "check: 2 -> failed (unexpected)"           "$(check_verdict 2)"  failed
is "check: 127 -> failed (unexpected)"         "$(check_verdict 127)" failed

# ── seed_decision: never silently overwrite an operator's engine.
mkdir -p "$T/s" "$T/d"
printf 'aaa' > "$T/s/m.engine"; printf '{"0":"a"}' > "$T/s/m.names.json"
SD() { seed_decision_pair "$T/s/m.engine" "$T/s/m.names.json" "$T/d/m.engine" "$T/d/m.names.json"; }
is "seed: both absent -> seed" "$(SD)" "seed"
printf 'aaa' > "$T/d/m.engine"; printf '{"0":"a"}' > "$T/d/m.names.json"
is "seed: pair identical -> leave it alone" "$(SD)" "same"
printf 'bbb' > "$T/d/m.engine"
is "seed: engine DIFFERENT -> never overwrite silently" "$(SD)" "differs"
printf 'aaa' > "$T/d/m.engine"; printf '{"0":"WRONG"}' > "$T/d/m.names.json"
is "seed: engine same but SIDECAR differs -> differs (not 'same')" "$(SD)" "differs"
rm -f "$T/d/m.names.json"
is "seed: PARTIAL pair (engine, no sidecar) -> differs, never 'same'" "$(SD)" "differs"
# Finding 5: the REVERSE partial pair (sidecar present, engine absent) was
# untested — same guard, opposite side.
rm -f "$T/d/m.engine"; printf '{"0":"a"}' > "$T/d/m.names.json"
is "seed: PARTIAL pair (sidecar, no engine) -> differs, never 'same'" "$(SD)" "differs"

# ── install_pair: the engine must appear LAST, so a newly visible engine always
# has its sidecar (power can fail between two renames — this is ordering, not
# atomicity).
rm -rf "$T/p"; mkdir -p "$T/p/src" "$T/p/dst"
printf 'ENGINE' > "$T/p/src/m.engine"; printf '{"0":"a"}' > "$T/p/src/m.names.json"
install_pair "$T/p/src/m.engine" "$T/p/src/m.names.json" "$T/p/dst"
rc_is "pair: installs" $? 0
[ -f "$T/p/dst/m.engine" ] && [ -f "$T/p/dst/m.names.json" ] && ok "pair: both landed" || bad "pair: both landed"
is "pair: engine content intact" "$(cat "$T/p/dst/m.engine")" "ENGINE"

# Finding 5: prove the ORDERING claim, not just final state. The whole point
# of install_pair is "engine appears LAST, so a newly-visible engine always
# has its sidecar" — nothing above actually tested that. Intercept `mv` with
# a fake that logs its args before delegating to the real one, and check the
# sidecar's rename is logged strictly before the engine's.
rm -rf "$T/order"; mkdir -p "$T/order/bin" "$T/order/src" "$T/order/dst"
REAL_MV="$(command -v mv)"
cat > "$T/order/bin/mv" <<EOF
#!/bin/sh
echo "\$*" >> "$T/order/mv.log"
exec "$REAL_MV" "\$@"
EOF
chmod +x "$T/order/bin/mv"
printf 'ENGINE2' > "$T/order/src/m.engine"; printf '{"0":"a"}' > "$T/order/src/m.names.json"
: > "$T/order/mv.log"
PATH="$T/order/bin:$PATH" install_pair "$T/order/src/m.engine" "$T/order/src/m.names.json" "$T/order/dst"
rc_is "pair: installs (ordering test)" $? 0
SIDE_LINE="$(grep -n 'm\.names\.json$' "$T/order/mv.log" | head -1 | cut -d: -f1)"
ENG_LINE="$(grep -n 'm\.engine$' "$T/order/mv.log" | head -1 | cut -d: -f1)"
if [ -n "$SIDE_LINE" ] && [ -n "$ENG_LINE" ] && [ "$SIDE_LINE" -lt "$ENG_LINE" ]; then
    ok "pair: sidecar rename is logged BEFORE the engine rename"
else
    bad "pair: sidecar rename is logged BEFORE the engine rename (side=$SIDE_LINE eng=$ENG_LINE)"
fi

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

# Finding 2: a config with no [daemon] section at all is malformed/unexpected.
# The naive version copied the input through and returned 0 — silently no
# autologin, reported as success. Must refuse AND must not touch the file.
cat > "$T/gdm_nodaemon.conf" <<'EOF'
# GDM configuration storage
[security]
AllowRoot=true
EOF
NODAEMON_BEFORE="$(sha256sum "$T/gdm_nodaemon.conf" | cut -d' ' -f1)"
gdm_set_autologin "$T/gdm_nodaemon.conf" modela
rc_is "gdm: refuses when [daemon] section is absent" $? 1
NODAEMON_AFTER="$(sha256sum "$T/gdm_nodaemon.conf" | cut -d' ' -f1)"
is "gdm: file byte-identical to before when [daemon] absent" "$NODAEMON_AFTER" "$NODAEMON_BEFORE"

# Finding 3: the write must never expose a partial/truncated config, and the
# atomic replace must carry ownership + mode across (chown/chmod --reference
# are GNU coreutils — real semantics only on the Linux target; MSYS2's `chmod`
# doesn't model POSIX permission bits the same way, so this assertion is
# Linux-only and skipped-with-a-note on the dev box, per the task's guidance).
case "$(uname -s 2>/dev/null)" in
    Linux)
        chmod 640 "$T/gdm.conf"
        MODE_BEFORE="$(stat -c %a "$T/gdm.conf")"
        gdm_set_autologin "$T/gdm.conf" modela >/dev/null
        MODE_AFTER="$(stat -c %a "$T/gdm.conf")"
        is "gdm: file mode preserved across atomic replace (Linux only)" "$MODE_AFTER" "$MODE_BEFORE"
        ;;
    *)
        echo "skip - gdm: mode-preservation check (chown/chmod --reference are GNU/Linux-only; not meaningful on $(uname -s 2>/dev/null || echo unknown))"
        ;;
esac

# Idempotent: a second set must not duplicate the keys (dpkg re-invokes scripts).
gdm_set_autologin "$T/gdm.conf" modela
is "gdm: idempotent (no duplicate keys)" "$(grep -c '^AutomaticLogin=' "$T/gdm.conf")" "1"

# Restore puts back ONLY what we changed, and only if it still holds our value.
gdm_restore_autologin "$T/gdm.conf" modela "false" ""
rc_is "gdm: restore succeeds" $? 0
grep -q '^AutomaticLoginEnable=false$' "$T/gdm.conf" && ok "gdm: enable restored" || bad "gdm: enable restored"

# Finding 1: gdm_restore_autologin must return THREE distinct codes, not a
# boolean. denso-setup's cmd_unconfigure branches on 0 (restored) vs 2 (benign
# admin divergence -- nothing changed, correct as-is) vs anything else (a REAL
# failure it must refuse on, so it never lets prerm/postrm delete the only
# record of the original GDM settings after a restore that never happened).

# If an admin changed the user AFTER us, restore must refuse rather than
# clobber -- and that refusal is the BENIGN divergence code (2), not a failure.
gdm_set_autologin "$T/gdm.conf" modela
sed -i 's/^AutomaticLogin=modela$/AutomaticLogin=someoneelse/' "$T/gdm.conf"
gdm_restore_autologin "$T/gdm.conf" modela "false" ""
rc_is "gdm: refuses to clobber an admin's later USER change (rc=2, benign divergence)" $? 2
grep -q '^AutomaticLogin=someoneelse$' "$T/gdm.conf" && ok "gdm: admin's value survives" || bad "gdm: admin's value survives"

# Same divergence class, the OTHER key: an admin flipping ENABLE (not USER)
# after us must also refuse with rc=2, not be silently overwritten.
cat > "$T/gdm_en.conf" <<'EOF'
[daemon]
AutomaticLoginEnable=true
AutomaticLogin=modela
EOF
sed -i 's/^AutomaticLoginEnable=true$/AutomaticLoginEnable=false/' "$T/gdm_en.conf"
gdm_restore_autologin "$T/gdm_en.conf" modela "false" "olduser"
rc_is "gdm: refuses to clobber an admin's later ENABLE-key change (rc=2, benign divergence)" $? 2
grep -q '^AutomaticLoginEnable=false$' "$T/gdm_en.conf" && ok "gdm: admin's enable value survives" || bad "gdm: admin's enable value survives"

# A genuine failure (unwritable conf) must be neither 0 (restored) nor the
# benign-divergence 2 -- collapsing this into 2 is exactly the bug that let
# denso-setup treat "I couldn't write the file" the same as "an admin changed
# it on purpose", proceeding to delete the only recorded original state.
cat > "$T/gdm_ro.conf" <<'EOF'
[daemon]
AutomaticLoginEnable=true
AutomaticLogin=modela
EOF
chmod 444 "$T/gdm_ro.conf"
gdm_restore_autologin "$T/gdm_ro.conf" modela "false" "olduser"
RO_RC=$?
chmod 644 "$T/gdm_ro.conf"
if [ "$RO_RC" != "0" ] && [ "$RO_RC" != "2" ]; then
    ok "gdm: unwritable conf is a real failure (rc=$RO_RC, not 0 and not benign-divergence 2)"
else
    bad "gdm: unwritable conf is a real failure (rc=$RO_RC, not 0 and not benign-divergence 2)"
fi

# Finding 1 regression: every function must run in a SUBSHELL so its locals
# cannot leak into (or clobber) the caller's variables. denso-setup sources
# this file and uses $plan/$tmp/$user/$eng/$side itself — demonstrated live:
# a caller's plan="MY IMPORTANT VALUE" came back as plan=/tmp/p.txt after
# calling apt_plan_ok, before the subshell fix. Set sentinels covering every
# name each function assigns internally, call all six functions, and assert
# none of the caller's variables moved.
plan="SENTINEL_plan"; tmp="SENTINEL_tmp"; user="SENTINEL_user"
eng="SENTINEL_eng"; side="SENTINEL_side"; dst="SENTINEL_dst"; stem="SENTINEL_stem"
conf="SENTINEL_conf"; denso_user="SENTINEL_denso_user"
esrc="SENTINEL_esrc"; ssrc="SENTINEL_ssrc"; edst="SENTINEL_edst"; sdst="SENTINEL_sdst"
e1="SENTINEL_e1"; e2="SENTINEL_e2"; s1="SENTINEL_s1"; s2="SENTINEL_s2"
orig_enable="SENTINEL_orig_enable"; orig_user="SENTINEL_orig_user"
cur_user="SENTINEL_cur_user"; cur_en="SENTINEL_cur_en"

version_ok "0.1.0" >/dev/null
apt_plan_ok "$T/plain" >/dev/null
SD >/dev/null
install_pair "$T/p/src/m.engine" "$T/p/src/m.names.json" "$T/p/dst" >/dev/null
gdm_set_autologin "$T/gdm.conf" modela >/dev/null 2>&1
gdm_restore_autologin "$T/gdm.conf" modela "false" "" >/dev/null 2>&1

is "leak: plan unchanged after apt_plan_ok"          "$plan"        "SENTINEL_plan"
is "leak: eng unchanged after install_pair"          "$eng"         "SENTINEL_eng"
is "leak: side unchanged after install_pair"         "$side"        "SENTINEL_side"
is "leak: dst unchanged after install_pair"          "$dst"         "SENTINEL_dst"
is "leak: stem unchanged after install_pair"         "$stem"        "SENTINEL_stem"
is "leak: tmp unchanged after gdm_set_autologin"     "$tmp"         "SENTINEL_tmp"
is "leak: user unchanged after gdm_set_autologin"    "$user"        "SENTINEL_user"
is "leak: conf unchanged after gdm_restore_autologin" "$conf"       "SENTINEL_conf"
is "leak: denso_user unchanged after gdm_restore_autologin" "$denso_user" "SENTINEL_denso_user"
is "leak: orig_enable unchanged after gdm_restore_autologin" "$orig_enable" "SENTINEL_orig_enable"
is "leak: orig_user unchanged after gdm_restore_autologin" "$orig_user" "SENTINEL_orig_user"
is "leak: cur_user unchanged after gdm_restore_autologin" "$cur_user" "SENTINEL_cur_user"
is "leak: cur_en unchanged after gdm_restore_autologin" "$cur_en"   "SENTINEL_cur_en"
is "leak: esrc unchanged after seed_decision_pair"   "$esrc"        "SENTINEL_esrc"
is "leak: ssrc unchanged after seed_decision_pair"   "$ssrc"        "SENTINEL_ssrc"
is "leak: edst unchanged after seed_decision_pair"   "$edst"        "SENTINEL_edst"
is "leak: sdst unchanged after seed_decision_pair"   "$sdst"        "SENTINEL_sdst"
is "leak: e1 unchanged after seed_decision_pair"     "$e1"          "SENTINEL_e1"
is "leak: e2 unchanged after seed_decision_pair"     "$e2"          "SENTINEL_e2"
is "leak: s1 unchanged after seed_decision_pair"     "$s1"          "SENTINEL_s1"
is "leak: s2 unchanged after seed_decision_pair"     "$s2"          "SENTINEL_s2"

# ── standalone preflight-denso.sh vs the library apt_plan_ok: prove they
# cannot drift. `denso-setup preflight` cannot protect a FIRST install (it
# doesn't exist on the box until the .deb containing it already is), so
# build_package.sh also emits a standalone twin next to the .deb, generated
# by the ONE shared emitter in packaging/lib/gen_preflight.sh. This proves
# the standalone script and the library function agree on the SAME fixture
# plans used above, without requiring a full `tools/build_package.sh` run
# (which needs aarch64 + the Jetson toolchain).
. "$HERE/../../packaging/lib/gen_preflight.sh"
PF="$T/pf"
mkdir -p "$PF/bin"

# A real (non-empty, content-distinguishable) fake .deb so its SHA-256 means
# something for the binding tests below -- plus a second, DIFFERENT fake .deb
# to prove a mismatched artifact is refused.
printf 'not a real deb, just needs a stable sha256\n' > "$PF/fake.deb"
FAKE_DEB_SHA="$(sha256sum "$PF/fake.deb" | cut -d' ' -f1)"
printf 'a DIFFERENT fake deb -- must never be accepted by a guard generated for fake.deb\n' > "$PF/other.deb"

emit_preflight_script "$HERE/../../packaging/lib/policy.sh" "$PF/preflight-denso.sh" "$FAKE_DEB_SHA"
[ -x "$PF/preflight-denso.sh" ] && ok "preflight: generated standalone script is executable" \
    || bad "preflight: generated standalone script is executable"

# Finding 7: emit_preflight_script must run in a SUBSHELL so its locals
# (policy/out/deb_sha/tmp) cannot leak into the caller -- the exact defect
# already fixed across every function in packaging/lib/policy.sh, and
# build_package.sh sources both files into the same shell.
policy="SENTINEL_policy"; out="SENTINEL_out"
emit_preflight_script "$HERE/../../packaging/lib/policy.sh" "$PF/leak-check.sh" "$FAKE_DEB_SHA" >/dev/null
is "leak: policy unchanged after emit_preflight_script" "$policy" "SENTINEL_policy"
is "leak: out unchanged after emit_preflight_script"    "$out"    "SENTINEL_out"

# Fake `id` (the standalone driver root-checks) and a fake `apt-get -s install`
# that just replays a FIXTURE plan file's content -- this is how a plan file
# (not a real .deb, per the task's "no full package build" guidance) drives
# the standalone script through the SAME apt_plan_ok logic the library test
# above already exercised on that exact file, making the two verdicts
# directly comparable.
cat > "$PF/bin/id" <<'EOF'
#!/bin/sh
echo 0
EOF
cat > "$PF/bin/apt-get" <<'EOF'
#!/bin/sh
# args: -s install --no-install-recommends <deb>  (deb is ignored: the
# fixture plan named by $FAKE_APT_PLAN stands in for a real apt-get -s.)
[ -n "${FAKE_APT_PLAN-}" ] && [ -f "$FAKE_APT_PLAN" ] || { echo "fake apt-get: FAKE_APT_PLAN not set" >&2; exit 1; }
cat "$FAKE_APT_PLAN"
exit 0
EOF
chmod +x "$PF/bin/id" "$PF/bin/apt-get"

compare_guards() {
    label="$1"; fixture="$2"; want_rc="$3"
    apt_plan_ok "$fixture" >/dev/null 2>&1; lib_rc=$?
    FAKE_APT_PLAN="$fixture" PATH="$PF/bin:$PATH" "$PF/preflight-denso.sh" "$PF/fake.deb" >/dev/null 2>&1
    standalone_rc=$?
    # Each side is graded against the EXPECTED verdict independently -- a bug
    # that made both guards agreeably wrong (e.g. both always exit 0) would
    # still be caught here, not hidden by only checking mutual agreement.
    rc_is "$label: library apt_plan_ok verdict"          "$lib_rc"        "$want_rc"
    rc_is "$label: standalone preflight-denso.sh verdict" "$standalone_rc" "$want_rc"
    is   "$label: both guards agree"                     "$lib_rc"        "$standalone_rc"
}

compare_guards "preflight-drift: plain install"       "$T/plain"  0
compare_guards "preflight-drift: plan with a removal" "$T/remove" 1
compare_guards "preflight-drift: plan touching cuda-*" "$T/cuda"  1

# Finding 6: the generated preflight is bound to the ONE .deb it was
# generated for by an EMBEDDED SHA-256, not just a filename convention -- an
# operator must not be able to pair an old guard with a new .deb. FAKE_APT_PLAN
# is set to the PLAIN (allowed) fixture for both calls below, so if the
# checksum gate did not run before the apt simulation, "$PF/other.deb" would
# sail through to a false PASS -- proving the hash check gates the simulation,
# not just that a mismatch is somehow eventually noticed.
FAKE_APT_PLAN="$T/plain" PATH="$PF/bin:$PATH" "$PF/preflight-denso.sh" "$PF/fake.deb" >/dev/null 2>&1
rc_is "preflight-binding: accepts the .deb it was generated for" $? 0

FAKE_APT_PLAN="$T/plain" PATH="$PF/bin:$PATH" "$PF/preflight-denso.sh" "$PF/other.deb" >"$PF/mismatch.out" 2>&1
rc_is "preflight-binding: refuses a .deb whose SHA-256 does not match" $? 1
grep -q "is not the .deb this preflight script was generated for" "$PF/mismatch.out" \
    && ok "preflight-binding: refusal names the mismatch" \
    || bad "preflight-binding: refusal names the mismatch"

# ── transport bundle: the .deb + its guard as ONE movable file. Same reason
# the preflight is generated by a sourceable emitter — tools/build_package.sh
# hard-refuses to run off an aarch64 JetPack box, so the ONLY place this shape
# can be proven on the dev box is here, against packaging/lib/gen_bundle.sh.
. "$HERE/../../packaging/lib/gen_bundle.sh"
B="$T/bundle"
mkdir -p "$B/out"
BNAME="denso-digitalreader_0.1.0+r400.gabc1234_arm64"
BOUT="$B/out/$BNAME.tar.gz"
# A FIXED epoch, never `date +%s`: the whole point is that the archive is a
# function of its inputs, so the test's own inputs must be fixed too.
EPOCH=1700000000
emit_bundle "$PF/fake.deb" "$PF/preflight-denso.sh" "$BNAME" "$BOUT" "$EPOCH"
rc_is "bundle: emits" $? 0
[ -f "$BOUT" ] && ok "bundle: archive exists" || bad "bundle: archive exists"

# An archive an operator unpacks with elevated intent must not be able to write
# outside the directory they unpacked it into, and must not scatter files into
# their cwd (a "tar bomb").
ENTRIES="$(tar tzf "$BOUT")"
is "bundle: exactly ONE top-level entry" \
   "$(printf '%s\n' "$ENTRIES" | sed 's|/.*||' | sort -u | wc -l | tr -d ' ')" "1"
is "bundle: that entry is the versioned dir" \
   "$(printf '%s\n' "$ENTRIES" | sed 's|/.*||' | sort -u)" "$BNAME"
printf '%s\n' "$ENTRIES" | grep -q '^/' \
    && bad "bundle: no absolute paths" || ok "bundle: no absolute paths"
printf '%s\n' "$ENTRIES" | grep -q '\.\.' \
    && bad "bundle: no parent-dir traversal" || ok "bundle: no parent-dir traversal"

# An unsafe name must be REFUSED, not sanitised: the caller passing one is a
# bug, and quietly rewriting it would hide that while still shipping.
emit_bundle "$PF/fake.deb" "$PF/preflight-denso.sh" "../escape" "$B/out/x.tar.gz" "$EPOCH"
rc_is "bundle: refuses a traversing bundle name" $? 1
emit_bundle "$PF/fake.deb" "$PF/preflight-denso.sh" "a/b" "$B/out/x.tar.gz" "$EPOCH"
rc_is "bundle: refuses a bundle name with a slash" $? 1
emit_bundle "$PF/nonesuch.deb" "$PF/preflight-denso.sh" "$BNAME" "$B/out/x.tar.gz" "$EPOCH"
rc_is "bundle: refuses a missing .deb" $? 1

mkdir -p "$B/x" && ( cd "$B/x" && tar xzf "$BOUT" )
rc_is "bundle: extracts" $? 0
X="$B/x/$BNAME"
for f in fake.deb preflight-denso.sh SHA256SUMS INSTALL.txt; do
    [ -f "$X/$f" ] && ok "bundle: contains $f" || bad "bundle: contains $f"
done

# THE point of regenerating SHA256SUMS instead of copying the build's own
# <deb>.sha256: that one records the path `dist/denso-...deb`, so `-c` from
# inside an unpacked bundle would hunt for a dist/ subdir that does not exist
# and fail on an INTACT artifact. A checksum that cries wolf trains operators
# to skip it.
printf '%s\n' "$ENTRIES" | grep -q 'dist/' \
    && bad "bundle: SHA256SUMS uses bare names (no dist/ paths)" \
    || ok "bundle: SHA256SUMS uses bare names (no dist/ paths)"
grep -q ' dist/' "$X/SHA256SUMS" \
    && bad "bundle: SHA256SUMS entries are bare filenames" \
    || ok "bundle: SHA256SUMS entries are bare filenames"
is "bundle: SHA256SUMS covers the guard too, not just the payload" \
   "$(grep -c 'preflight-denso.sh$' "$X/SHA256SUMS")" "1"
( cd "$X" && sha256sum -c SHA256SUMS >/dev/null 2>&1 )
rc_is "bundle: sha256sum -c PASSES from inside the extracted dir" $? 0

# ...and actually detects tampering — a check that passes unconditionally is
# indistinguishable from no check at all.
printf 'tampered' >> "$X/fake.deb"
( cd "$X" && sha256sum -c SHA256SUMS >/dev/null 2>&1 )
rc_is "bundle: sha256sum -c FAILS on a modified .deb" $? 1

# INSTALL.txt is the runbook for an operator who has nothing else. The guard
# must come BEFORE apt, apt must carry --no-install-recommends (the preflight
# simulates that exact transaction — a different invocation is unvetted), and
# `verify` must be the sudo form (cmd_verify calls need_root).
grep -q -- '--no-install-recommends' "$X/INSTALL.txt" \
    && ok "install-txt: apt carries --no-install-recommends" || bad "install-txt: apt carries --no-install-recommends"
grep -q "dpkg -i" "$X/INSTALL.txt" \
    && ok "install-txt: warns against dpkg -i" || bad "install-txt: warns against dpkg -i"
grep -q "sudo denso-setup verify" "$X/INSTALL.txt" \
    && ok "install-txt: verify is the sudo form (cmd_verify needs root)" || bad "install-txt: verify is the sudo form (cmd_verify needs root)"
PRE_LINE="$(grep -n 'preflight-denso.sh' "$X/INSTALL.txt" | head -1 | cut -d: -f1)"
APT_LINE="$(grep -n 'apt install' "$X/INSTALL.txt" | head -1 | cut -d: -f1)"
if [ -n "$PRE_LINE" ] && [ -n "$APT_LINE" ] && [ "$PRE_LINE" -lt "$APT_LINE" ]; then
    ok "install-txt: the guard is ordered BEFORE apt install"
else
    bad "install-txt: the guard is ordered BEFORE apt install (pre=$PRE_LINE apt=$APT_LINE)"
fi

# The guard is the first thing the runbook tells someone to run as root; it has
# to arrive runnable. (MSYS2 does not model POSIX permission bits, so the mode
# assertion is Linux-only — same carve-out as the gdm mode check above.)
case "$(uname -s 2>/dev/null)" in
    Linux)
        is "bundle: preflight extracts executable" "$(stat -c %a "$X/preflight-denso.sh")" "755"
        is "bundle: .deb extracts 0644"            "$(stat -c %a "$X/fake.deb")"           "644"
        # Not group-writable: SHA256SUMS and INSTALL.txt are written by a `>`
        # redirect, which takes the BUILDER's umask — on the Jetson (umask 002)
        # they shipped 0664 until this was pinned.
        is "bundle: SHA256SUMS extracts 0644 (umask-independent)" "$(stat -c %a "$X/SHA256SUMS")" "644"
        is "bundle: INSTALL.txt extracts 0644"     "$(stat -c %a "$X/INSTALL.txt")"        "644"
        # The loose dist/ guard and the bundled copy must agree: mktemp+`chmod
        # +x` used to make the loose one 0711.
        is "preflight: emitted guard is 0755, not 0711" "$(stat -c %a "$PF/preflight-denso.sh")" "755"
        ;;
    *) echo "skip - bundle: extracted file modes (POSIX bits are not modelled on $(uname -s 2>/dev/null || echo unknown))" ;;
esac

# ── REPRODUCIBILITY. The clean bundle name carries no content hash, so the
# name is only a truthful identifier if identical inputs give identical bytes.
# This was FALSE before: the .deb embedded a wall-clock build time and gzip
# stamped its own header, so a rebuild silently replaced an earlier artifact
# under the same filename. Proven here at the emitter level (the full
# build-twice proof needs a Jetson — tests/manual/repro_build.sh).
R1="$B/out/repro1.tar.gz"; R2="$B/out/repro2.tar.gz"
emit_bundle "$PF/fake.deb" "$PF/preflight-denso.sh" "$BNAME" "$R1" "$EPOCH" >/dev/null
emit_bundle "$PF/fake.deb" "$PF/preflight-denso.sh" "$BNAME" "$R2" "$EPOCH" >/dev/null
is "repro: same inputs + same epoch -> BYTE-IDENTICAL archive" \
   "$(sha256sum < "$R1")" "$(sha256sum < "$R2")"

# ...and the epoch is genuinely applied, not merely accepted. Without this a
# hardcoded/ignored --mtime would pass the test above for the wrong reason:
# two archives built in the same second are identical by luck.
R3="$B/out/repro3.tar.gz"
emit_bundle "$PF/fake.deb" "$PF/preflight-denso.sh" "$BNAME" "$R3" "1600000000" >/dev/null
if [ "$(sha256sum < "$R1")" != "$(sha256sum < "$R3")" ]; then
    ok "repro: a DIFFERENT epoch changes the bytes (the mtime is really applied)"
else
    bad "repro: a DIFFERENT epoch changes the bytes (the mtime is really applied)"
fi
is "repro: entry mtime is the epoch, not the staging time" \
   "$(TZ=UTC tar tzvf "$R1" | awk '/INSTALL.txt$/{print $4}')" \
   "$(TZ=UTC date -u -d @$EPOCH +%Y-%m-%d)"

# gzip writes a TIMESTAMP into its own header, so a `tar -czf` archive differs
# between runs even when every tar entry is pinned — the easy-to-miss half of
# this fix. Byte 5..8 of a gzip stream is that MTIME field; `gzip -n` zeroes it.
is "repro: gzip header timestamp is zeroed (gzip -n)" \
   "$(od -An -tx1 -j4 -N4 "$R1" | tr -d ' ')" "00000000"

# The archive's top-level DIRECTORY has a mode too, and `mkdir` takes the
# builder's umask — 0775 under the Jetson's 002, 0755 under 022. That changed
# the archive bytes under an identical clean filename, on a file holding no
# data at all.
is "repro: the top-level directory is 0755, not umask-dependent" \
   "$(tar tzvf "$R1" | awk 'NR==1{print $1}')" "drwxr-xr-x"

# A tar FAILURE must not be masked. `{ tar || rc=$?; } | gzip` cannot work — a
# pipeline component runs in a subshell, so rc never reaches the caller and
# gzip happily compresses a truncated stream into a valid-looking archive.
# Verified by making tar fail (a source path that does not exist) and asserting
# both the status AND that no archive is left behind.
FAKEBIN="$B/failbin"; mkdir -p "$FAKEBIN"
cat > "$FAKEBIN/tar" <<'EOF'
#!/bin/sh
# Emit a plausible partial stream, then fail — the exact shape that used to
# sail through as success.
printf 'partial-tar-stream'
exit 3
EOF
chmod +x "$FAKEBIN/tar"
FAILOUT="$B/out/should-not-exist.tar.gz"
PATH="$FAKEBIN:$PATH" emit_bundle "$PF/fake.deb" "$PF/preflight-denso.sh" "$BNAME" "$FAILOUT" "$EPOCH" >/dev/null 2>&1
rc_is "repro: a FAILING tar is reported, not masked by gzip" $? 1
[ -f "$FAILOUT" ] && bad "repro: a failed tar leaves no archive behind" \
                  || ok "repro: a failed tar leaves no archive behind"

# An unusable epoch must be REFUSED, never defaulted to "now": silently
# falling back is exactly how the non-reproducibility would creep back.
emit_bundle "$PF/fake.deb" "$PF/preflight-denso.sh" "$BNAME" "$B/out/x.tar.gz" ""
rc_is "repro: refuses an empty epoch (never defaults to now)" $? 1
emit_bundle "$PF/fake.deb" "$PF/preflight-denso.sh" "$BNAME" "$B/out/x.tar.gz" "not-a-number"
rc_is "repro: refuses a non-numeric epoch" $? 1

# Finding-1 regression class: emit_bundle must run in a SUBSHELL like every
# other function here — build_package.sh sources policy.sh, gen_preflight.sh
# and gen_bundle.sh into ONE shell, and it uses $deb/$out/$tmp/$stage itself.
deb="SENTINEL_deb"; pre="SENTINEL_pre"; name="SENTINEL_name"; out="SENTINEL_out"
stage="SENTINEL_stage"; root="SENTINEL_root"; tmp="SENTINEL_tmp"
deb_base="SENTINEL_deb_base"; pre_base="SENTINEL_pre_base"
emit_bundle "$PF/fake.deb" "$PF/preflight-denso.sh" "$BNAME" "$B/out/leak.tar.gz" "$EPOCH" >/dev/null
for v in deb pre name out stage root tmp deb_base pre_base; do
    eval "got=\$$v"
    is "leak: $v unchanged after emit_bundle" "$got" "SENTINEL_$v"
done

# ═══════════════════════════════════════════════════════════════════════════
# SLICE 5 — schema-2 manifest delivery, seed-manifest, non-mutating verify,
# the Release-A "no Float artifact" guarantee, and the ordering assertion.
#
# tools/build_package.sh cannot run off aarch64, so — exactly like the bundle and
# preflight above — the model/manifest PAYLOAD staging is a sourceable emitter
# (packaging/lib/gen_payload.sh) driven here with fixtures. The seed-manifest
# runtime decision + atomic write are sourceable policy.sh functions, so every
# refusal branch is exercised without root or /opt/denso.
# ═══════════════════════════════════════════════════════════════════════════
REPO="$HERE/../.."
# Never let a Python import drop tools/__pycache__ into the tree — build_package.sh
# refuses a dirty tree, so a stray *.pyc would block the next release build.
export PYTHONDONTWRITEBYTECODE=1
. "$REPO/packaging/lib/gen_payload.sh"

# Skip the whole block cleanly if python3 is unavailable (seed-manifest declares
# python3 as a package dependency; the harness should say so, not silently pass).
if ! command -v python3 >/dev/null 2>&1; then
    echo "skip - slice5: python3 not available (seed-manifest/verify require it; declared in control.in)"
else

M="$T/slice5"; mkdir -p "$M/src" "$M/desc" "$M/stage"
# A self-contained digitv3-SHAPED fixture: fake engine/sidecar bytes + a matching
# fixture models.approved + a fixture descriptor. The generated manifest is a real
# schema-2 digitv3 declaration (10 classes from the sidecar), just with fixture
# hashes — enough to prove staging, modes, SHA256SUMS, and the no-Float rule.
printf 'FAKE-ENGINE-BYTES-for-testing-only-not-a-real-plan' > "$M/src/digitv3.engine"
printf '["0","1","2","3","4","5","6","7","8","9"]' > "$M/src/digitv3.names.json"
EH="$(sha256sum "$M/src/digitv3.engine" | cut -d' ' -f1)"
SH="$(sha256sum "$M/src/digitv3.names.json" | cut -d' ' -f1)"
printf 'digitv3 %s %s trtexec --onnx=digitv3.onnx --saveEngine=digitv3.engine --fp16\n' "$EH" "$SH" > "$M/models.approved"
python3 - "$M/desc/digitv3.descriptor.json" "$EH" "$SH" <<'PY'
import json, sys
p, eh, sh = sys.argv[1:4]
json.dump({
    "name": "digitv3", "canonical_id": "digitv3", "family": "digit_numeric",
    "task": "detect", "input_size": 640, "state": "installed",
    "installed_utc": "2026-07-23T00:00:00Z",
    "tensorrt": {"expected_engine_sha256": eh, "expected_sidecar_sha256": sh,
                 "built_for": {"trt": "10.3", "cuda": "12.6", "sm": "87"}},
    "provenance": {"source_pt_sha256": "0"*64, "onnx_sha256": "1"*64,
                   "export_ultralytics": "8.4.33", "precision": "fp16",
                   "export_engine_command": "trtexec --onnx=digitv3.onnx --saveEngine=digitv3.engine --fp16"},
    "provenance_evidence": {"source_pt_sha256": "fixture", "onnx_sha256": "fixture",
                            "export_ultralytics": "fixture", "precision": "fixture",
                            "export_engine_command": "fixture"},
}, open(p, "w"), indent=2)
PY

stage_model_payload "$M/stage" "$REPO" "$M/models.approved" "$M/desc" "-" "$M/src/digitv3.engine" >/dev/null 2>&1
rc_is "slice5: stage_model_payload succeeds" $? 0
OD="$M/stage/opt/denso"

# (1) manifest in payload; (2) mode 0644; (3) in SHA256SUMS
[ -f "$OD/lib/manifest.json" ] && ok "slice5: manifest.json is in the payload" || bad "slice5: manifest.json is in the payload"
[ -f "$OD/lib/SHA256SUMS" ] && ok "slice5: SHA256SUMS is in the payload" || bad "slice5: SHA256SUMS is in the payload"
grep -q ' lib/manifest.json$' "$OD/lib/SHA256SUMS" && ok "slice5: manifest appears in SHA256SUMS" || bad "slice5: manifest appears in SHA256SUMS"
( cd "$OD" && sha256sum -c lib/SHA256SUMS >/dev/null 2>&1 ) && ok "slice5: SHA256SUMS -c passes over the model payload" || bad "slice5: SHA256SUMS -c passes over the model payload"

# (4) manifest parses AND validates under the real (python-mirror) validator
python3 - "$REPO" "$OD/lib/manifest.json" <<'PY'
import json, sys
sys.path.insert(0, sys.argv[1] + "/tools")
import gen_model_manifest as g
g.validate_manifest_object(json.load(open(sys.argv[2], encoding="utf-8")))
PY
rc_is "slice5: manifest parses and validates" $? 0

# (5) digitv3 only; (6) no Float generation; (7) no allowed_modes
is "slice5: manifest declares exactly one canonical_id (digitv3)" \
   "$(python3 -c 'import json,sys; d=json.load(open(sys.argv[1],encoding="utf-8")); print(",".join(g["canonical_id"] for g in d["generations"]))' "$OD/lib/manifest.json")" "digitv3"
grep -qi 'float' "$OD/lib/manifest.json" && bad "slice5: no Float name/generation in the manifest" || ok "slice5: no Float name/generation in the manifest"
grep -q 'allowed_modes' "$OD/lib/manifest.json" && bad "slice5: no allowed_modes in the manifest" || ok "slice5: no allowed_modes in the manifest"

# (8,9) models.approved installed at lib/, mode 0644
[ -f "$OD/lib/models.approved" ] && ok "slice5: models.approved installed at opt/denso/lib/models.approved" || bad "slice5: models.approved installed at opt/denso/lib/models.approved"

# (10) NO Float artifact anywhere in the whole payload; (11) no staging dir
FLOATS="$(find "$M/stage" -iname 'float-*' | wc -l | tr -d ' ')"
is "slice5: no Float artifact anywhere in the payload" "$FLOATS" "0"
# Check directory BASENAMES (a fixture path may itself contain 'stage').
STAGEDIRS="$(find "$OD" -type d \( -iname '*stag*' -o -iname '*float*' \) | wc -l | tr -d ' ')"
is "slice5: no staging/float directory under opt/denso" "$STAGEDIRS" "0"
# only lib/ and models/ subtrees exist under opt/denso in this fixture
is "slice5: opt/denso holds only lib + models here" \
   "$(cd "$OD" && LC_ALL=C find . -mindepth 1 -maxdepth 1 -type d | LC_ALL=C sort | sed 's|^\./||' | tr '\n' ',')" "lib,models,"

# (Linux-only) exact payload modes — POSIX bits are not modelled on MSYS2.
case "$(uname -s 2>/dev/null)" in
    Linux)
        is "slice5: manifest.json mode is 0644"     "$(stat -c %a "$OD/lib/manifest.json")"    "644"
        is "slice5: models.approved mode is 0644"   "$(stat -c %a "$OD/lib/models.approved")"  "644"
        is "slice5: SHA256SUMS mode is 0644"        "$(stat -c %a "$OD/lib/SHA256SUMS")"       "644"
        is "slice5: staged engine mode is 0644"     "$(stat -c %a "$OD/models/digitv3.engine")" "644"
        is "slice5: staged sidecar mode is 0644"    "$(stat -c %a "$OD/models/digitv3.names.json")" "644"
        ;;
    *) echo "skip - slice5: payload file modes (POSIX bits not modelled on $(uname -s 2>/dev/null || echo unknown))" ;;
esac

# Determinism: identical inputs -> byte-identical manifest + SHA256SUMS.
mkdir -p "$M/stage2"
stage_model_payload "$M/stage2" "$REPO" "$M/models.approved" "$M/desc" "-" "$M/src/digitv3.engine" >/dev/null 2>&1
is "slice5: manifest is byte-reproducible" "$(sha256sum < "$OD/lib/manifest.json")" "$(sha256sum < "$M/stage2/opt/denso/lib/manifest.json")"
is "slice5: SHA256SUMS is byte-reproducible" "$(sha256sum < "$OD/lib/SHA256SUMS")" "$(sha256sum < "$M/stage2/opt/denso/lib/SHA256SUMS")"

# set-consistency: the generated manifest declares EXACTLY the staged pairs.
manifest_matches_models_dir "$OD/lib/manifest.json" "$OD/models" >/dev/null 2>&1
rc_is "slice5: manifest declares exactly the staged model pairs" $? 0

# ── ordering assertion (spec 8.7.2): a Float approval requires the Slice-7 symbol.
ORD="$T/ordering"; mkdir -p "$ORD/root/src/core/models" "$ORD/root/src/app/ui"
cp "$M/models.approved" "$ORD/appr"
# (26) a temporary Float approval with no symbol -> build fails
printf 'float-small %s %s trtexec\n' "$EH" "$SH" >> "$ORD/appr"
assert_float_seeding_guarded "$ORD/appr" "$ORD/root" >/dev/null 2>&1
rc_is "slice5: ordering assertion FAILS on a Float approval without the symbol" $? 1
# (27) a mere COMMENT mentioning the symbol does NOT satisfy it
printf '// loadable_model_files goes here in Slice 7\nint x(){return 0;}\n' > "$ORD/root/src/core/models/compatibility.cpp"
printf 'void s(){/* loadable_model_files */}\n' > "$ORD/root/src/app/ui/startup.cpp"
assert_float_seeding_guarded "$ORD/appr" "$ORD/root" >/dev/null 2>&1
rc_is "slice5: a comment mentioning loadable_model_files does NOT satisfy the assertion" $? 1
# a bare DECLARATION also does not satisfy it
printf 'std::vector<std::string> loadable_model_files(TargetMode m);\n' > "$ORD/root/src/core/models/compatibility.cpp"
assert_float_seeding_guarded "$ORD/appr" "$ORD/root" >/dev/null 2>&1
rc_is "slice5: a bare declaration does NOT satisfy the ordering assertion" $? 1
# a real DEFINITION + a real USE -> passes (proves it is self-releasing for Slice 7/12)
cat > "$ORD/root/src/core/models/compatibility.cpp" <<'EOF'
std::vector<std::string> loadable_model_files(const std::vector<DetectionModel>& m,
                                              TargetMode mode, const ManifestView& v) {
    return {};
}
EOF
printf 'void launch(){ auto a = loadable_model_files(m,mode,v); (void)a; }\n' > "$ORD/root/src/app/ui/startup.cpp"
assert_float_seeding_guarded "$ORD/appr" "$ORD/root" >/dev/null 2>&1
rc_is "slice5: a real definition+use satisfies the ordering assertion" $? 0
# the USE must be a genuine call — a declaration, an uncalled dummy definition,
# or a #if-0'd call in startup.cpp must NOT satisfy it (compatibility.cpp keeps
# its real definition throughout).
printf 'std::vector<std::string> loadable_model_files(TargetMode m);\n' > "$ORD/root/src/app/ui/startup.cpp"
assert_float_seeding_guarded "$ORD/appr" "$ORD/root" >/dev/null 2>&1
rc_is "slice5: a mere declaration in startup.cpp does NOT satisfy 'use'" $? 1
printf 'std::vector<std::string> loadable_model_files(int a){ return {}; }\n' > "$ORD/root/src/app/ui/startup.cpp"
assert_float_seeding_guarded "$ORD/appr" "$ORD/root" >/dev/null 2>&1
rc_is "slice5: an uncalled dummy definition in startup.cpp does NOT satisfy 'use'" $? 1
printf '#if 0\nvoid l(){ auto a=loadable_model_files(m); }\n#endif\nvoid r(){}\n' > "$ORD/root/src/app/ui/startup.cpp"
assert_float_seeding_guarded "$ORD/appr" "$ORD/root" >/dev/null 2>&1
rc_is "slice5: a #if-0'd call in startup.cpp does NOT satisfy 'use'" $? 1
printf 'void launch(){ auto a = loadable_model_files(m,mode,v); (void)a; }\n' > "$ORD/root/src/app/ui/startup.cpp"
# (28) removing the Float approval -> passes regardless of the symbol
assert_float_seeding_guarded "$M/models.approved" "$ORD/root" >/dev/null 2>&1
rc_is "slice5: no Float approved -> ordering assertion passes" $? 0

# ── Slice 12 — the ordering assertion is now LOAD-BEARING on the real tree.
# Release A's counterpart here asserted that the committed models.approved
# approved no Float stem and so passed the assertion trivially. As of the
# Release B artifact cut both Float stems ARE approved, so the same call passes
# for a substantive reason: the Slice-7 allow-list really is defined and really
# is called from startup. Both halves are pinned — that the approvals exist, and
# that they would be REFUSED against a tree without the symbol.

# First, the call shape the REAL startup.cpp uses: namespace-qualified, on the
# continuation line of a wrapped assignment. At a line start that reads exactly
# like `<type tokens> symbol(args);` — the declaration shape — and it is a CALL.
# This is the case that made the gate refuse the real tree the first time a Float
# stem was approved, so it is pinned here in both directions.
cat > "$ORD/root/src/app/ui/startup.cpp" <<'EOF'
void launch() {
    std::set<std::string> allow_list =
        denso::models::loadable_model_files(mode, metadata);
    (void)allow_list;
}
EOF
assert_float_seeding_guarded "$ORD/appr" "$ORD/root" >/dev/null 2>&1
rc_is "slice12: a namespace-qualified call on a wrapped line satisfies 'use'" $? 0
# ...and the declaration rule is NOT weakened by that: a declarator name is
# preceded by whitespace, never by '::', so a wrapped bare declaration is still
# stripped and still fails.
cat > "$ORD/root/src/app/ui/startup.cpp" <<'EOF'
void launch() { }
std::set<std::string>
    loadable_model_files(denso::models::TargetMode m);
EOF
assert_float_seeding_guarded "$ORD/appr" "$ORD/root" >/dev/null 2>&1
rc_is "slice12: a wrapped bare DECLARATION still does NOT satisfy 'use'" $? 1
# The one DECLARATION whose declarator-id is legitimately qualified. Admitting it
# as a call is the dangerous direction — it would authorise shipping a Float
# engine on the strength of a declaration that calls nothing.
cat > "$ORD/root/src/app/ui/startup.cpp" <<'EOF'
class C {
    friend void denso::models::loadable_model_files(TargetMode);
};
void launch() { }
EOF
assert_float_seeding_guarded "$ORD/appr" "$ORD/root" >/dev/null 2>&1
rc_is "slice12: a QUALIFIED friend declaration does NOT satisfy 'use'" $? 1
# ...and a statement keyword is not a type: `return symbol(a);` is a call. (Left
# unhandled this fails closed, but as a refusal nobody could diagnose.) The BARE
# form is the discriminating one — it flips if `return` is dropped from the
# keyword exclusion. The qualified form is covered by `(?<!:)` either way, so it
# is general coverage of the return shape, not proof of the exclusion.
printf 'std::set<std::string> launch(){ return denso::models::loadable_model_files(m,v); }\n' \
    > "$ORD/root/src/app/ui/startup.cpp"
assert_float_seeding_guarded "$ORD/appr" "$ORD/root" >/dev/null 2>&1
rc_is "slice12: a qualified call in a return statement satisfies 'use'" $? 0
printf 'std::set<std::string> launch(){ return loadable_model_files(m,v); }\n' \
    > "$ORD/root/src/app/ui/startup.cpp"
assert_float_seeding_guarded "$ORD/appr" "$ORD/root" >/dev/null 2>&1
rc_is "slice12: a bare call in a return statement satisfies 'use'" $? 0

RBAPPR="$(awk 'NF && $1 !~ /^#/ && $1 ~ /^float-/ { print $1 }' "$REPO/packaging/models.approved" | LC_ALL=C sort | tr '\n' ',')"
is "slice12: the committed models.approved approves both Float stems" "$RBAPPR" "float-big,float-small,"
# EXACTLY these three — not "at least". An extra approved stem, or a lost
# digitv3 row, is a change to what may be seeded and must fail this cut.
RBALL="$(awk 'NF && $1 !~ /^#/ { print $1 }' "$REPO/packaging/models.approved" | LC_ALL=C sort | tr '\n' ',')"
is "slice12: the committed models.approved approves EXACTLY the three release stems" \
   "$RBALL" "digitv3,float-big,float-small,"
assert_float_seeding_guarded "$REPO/packaging/models.approved" "$REPO" >/dev/null 2>&1
rc_is "slice12: the committed Float-approved models.approved passes against the real tree" $? 0
# ...and it passes ONLY because of the symbol. Same file, a tree whose
# compatibility.cpp/startup.cpp exist but define/call nothing -> refusal.
mkdir -p "$ORD/bare/src/core/models" "$ORD/bare/src/app/ui"
printf 'int unrelated(){ return 0; }\n' > "$ORD/bare/src/core/models/compatibility.cpp"
printf 'void launch(){ }\n'             > "$ORD/bare/src/app/ui/startup.cpp"
assert_float_seeding_guarded "$REPO/packaging/models.approved" "$ORD/bare" >/dev/null 2>&1
rc_is "slice12: the committed models.approved is REFUSED against a tree lacking the allow-list" $? 1

# ── Slice 12 — the Release B payload: three approved pairs, three generations.
# Fixture bytes, real descriptors' shape, the REAL repo root (so the ordering
# assertion runs against the tree that must authorise this cut).
RB="$T/slice12"; mkdir -p "$RB/src" "$RB/desc" "$RB/stage" "$RB/stage2"
: > "$RB/models.approved"
for s in digitv3 float-small float-big; do
    case "$s" in
        digitv3)     names='["0","1","2","3","4","5","6","7","8","9"]'; fam=digit_numeric ;;
        float-small) names='["Small"]'; fam=float_ball ;;
        float-big)   names='["Big"]';   fam=float_ball ;;
    esac
    printf 'FAKE-ENGINE-BYTES-%s-not-a-real-plan' "$s" > "$RB/src/$s.engine"
    printf '%s' "$names" > "$RB/src/$s.names.json"
    reh="$(sha256sum "$RB/src/$s.engine" | cut -d' ' -f1)"
    rsh="$(sha256sum "$RB/src/$s.names.json" | cut -d' ' -f1)"
    printf '%s %s %s trtexec --onnx=%s.onnx --saveEngine=%s.engine --fp16\n' \
           "$s" "$reh" "$rsh" "$s" "$s" >> "$RB/models.approved"
    python3 - "$RB/desc/$s.descriptor.json" "$s" "$fam" "$reh" "$rsh" <<'PY'
import json, sys
p, name, fam, eh, sh = sys.argv[1:6]
json.dump({
    "name": name, "canonical_id": name, "family": fam,
    "task": "detect", "input_size": 640, "state": "installed",
    "installed_utc": "2026-07-29T00:00:00Z",
    "tensorrt": {"expected_engine_sha256": eh, "expected_sidecar_sha256": sh,
                 "built_for": {"trt": "10.3", "cuda": "12.6", "sm": "87"}},
    "provenance": {"source_pt_sha256": "0"*64, "onnx_sha256": "1"*64,
                   "export_ultralytics": "8.4.33", "precision": "fp16",
                   "export_engine_command":
                       "trtexec --onnx=%s.onnx --saveEngine=%s.engine --fp16" % (name, name)},
    "provenance_evidence": {"source_pt_sha256": "fixture", "onnx_sha256": "fixture",
                            "export_ultralytics": "fixture", "precision": "fixture",
                            "export_engine_command": "fixture"},
}, open(p, "w"), indent=2)
PY
done

# The canonical Release-B order: digitv3 first, then small, then big. Generation
# order follows this argument order and is part of the manifest's identity.
stage_model_payload "$RB/stage" "$REPO" "$RB/models.approved" "$RB/desc" "-" \
    "$RB/src/digitv3.engine" "$RB/src/float-small.engine" "$RB/src/float-big.engine" >/dev/null 2>&1
rc_is "slice12: stage_model_payload succeeds for the three-model Release B set" $? 0
RD="$RB/stage/opt/denso"

is "slice12: the manifest declares exactly three generations, in cut order" \
   "$(python3 -c 'import json,sys; d=json.load(open(sys.argv[1],encoding="utf-8")); print(",".join(g["canonical_id"] for g in d["generations"]))' "$RD/lib/manifest.json")" \
   "digitv3,float-small,float-big"
python3 - "$REPO" "$RD/lib/manifest.json" <<'PY'
import json, sys
sys.path.insert(0, sys.argv[1] + "/tools")
import gen_model_manifest as g
g.validate_manifest_object(json.load(open(sys.argv[2], encoding="utf-8")))
PY
rc_is "slice12: the three-generation manifest parses and validates" $? 0

# Every approved pair is really in the payload, engine AND sidecar.
RBMISS=""
for s in digitv3 float-small float-big; do
    [ -f "$RD/models/$s.engine" ]     || RBMISS="$RBMISS $s.engine"
    [ -f "$RD/models/$s.names.json" ] || RBMISS="$RBMISS $s.names.json"
done
is "slice12: all three engine/sidecar pairs are staged" "$RBMISS" ""
is "slice12: the payload holds exactly six model files" \
   "$(find "$RD/models" -type f | wc -l | tr -d ' ')" "6"
( cd "$RD" && sha256sum -c lib/SHA256SUMS >/dev/null 2>&1 ) \
    && ok "slice12: SHA256SUMS -c passes over the three-model payload" \
    || bad "slice12: SHA256SUMS -c passes over the three-model payload"
manifest_matches_models_dir "$RD/lib/manifest.json" "$RD/models" >/dev/null 2>&1
rc_is "slice12: the manifest declares exactly the three staged pairs" $? 0

# built_for is NORMALISED major.minor, for EVERY generation.
#
# This is the Slice-12 hard prerequisite, and it is a correctness rule, not a
# style one. The Slice-7 runtime platform provider (src/app/platform) reports
# trt=10.3 / cuda=12.6 / sm=87 on the appliance, and the compatibility policy
# compares built_for against it EXACTLY. The archived Slice-4 Float descriptors
# declared the full versions 10.3.0.30 / 12.6.68 / 87, which can never match —
# a Float engine declared that way is permanently unloadable while every hash
# and signature still looks correct. digitv3 already used the normalised form.
is "slice12: every staged generation declares the normalised built_for" \
   "$(python3 -c '
import json,sys
d=json.load(open(sys.argv[1],encoding="utf-8"))
print(",".join("%s=%s/%s/%s" % (g["canonical_id"],
      g["runtime"]["tensorrt"]["built_for"]["trt"],
      g["runtime"]["tensorrt"]["built_for"]["cuda"],
      g["runtime"]["tensorrt"]["built_for"]["sm"]) for g in d["generations"]))' "$RD/lib/manifest.json")" \
   "digitv3=10.3/12.6/87,float-small=10.3/12.6/87,float-big=10.3/12.6/87"
# ...and the COMMITTED descriptors, not just the fixture, carry that same form.
is "slice12: the committed descriptors declare the normalised built_for" \
   "$(python3 -c '
import json,sys
out=[]
for n in ("digitv3","float-small","float-big"):
    b=json.load(open(sys.argv[1]+"/packaging/manifest/"+n+".descriptor.json",encoding="utf-8"))["tensorrt"]["built_for"]
    out.append("%s=%s/%s/%s" % (n,b["trt"],b["cuda"],b["sm"]))
print(",".join(out))' "$REPO")" \
   "digitv3=10.3/12.6/87,float-small=10.3/12.6/87,float-big=10.3/12.6/87"

# Float artifacts are now expected — ONNX, PyTorch checkpoints and staging dirs
# are not, and never become expected. The Linux app catalogs *.engine only.
is "slice12: no .onnx anywhere in the Release B payload" \
   "$(find "$RB/stage" -iname '*.onnx' | wc -l | tr -d ' ')" "0"
is "slice12: no .pt anywhere in the Release B payload" \
   "$(find "$RB/stage" -iname '*.pt' | wc -l | tr -d ' ')" "0"
is "slice12: no staging directory under opt/denso" \
   "$(find "$RD" -type d -iname '*stag*' | wc -l | tr -d ' ')" "0"
grep -q 'allowed_modes' "$RD/lib/manifest.json" \
    && bad "slice12: no allowed_modes in the three-generation manifest" \
    || ok "slice12: no allowed_modes in the three-generation manifest"

case "$(uname -s 2>/dev/null)" in
    Linux)
        RBMODES=""
        for f in models/digitv3.engine models/digitv3.names.json \
                 models/float-small.engine models/float-small.names.json \
                 models/float-big.engine models/float-big.names.json \
                 lib/manifest.json lib/models.approved lib/SHA256SUMS; do
            [ "$(stat -c %a "$RD/$f")" = "644" ] || RBMODES="$RBMODES $f=$(stat -c %a "$RD/$f")"
        done
        is "slice12: every Release B payload file is mode 0644" "$RBMODES" "" ;;
    *) echo "skip - slice12: payload file modes (POSIX bits not modelled on $(uname -s 2>/dev/null || echo unknown))" ;;
esac

stage_model_payload "$RB/stage2" "$REPO" "$RB/models.approved" "$RB/desc" "-" \
    "$RB/src/digitv3.engine" "$RB/src/float-small.engine" "$RB/src/float-big.engine" >/dev/null 2>&1
# Assert the SECOND staging succeeded before comparing: otherwise a missing or
# truncated second output reports as a mere "not reproducible" hash mismatch and
# the real failure (staging broke) is never named.
rc_is "slice12: the second Release B staging succeeds" $? 0
is "slice12: the three-generation manifest is byte-reproducible" \
   "$(sha256sum < "$RD/lib/manifest.json")" "$(sha256sum < "$RB/stage2/opt/denso/lib/manifest.json")"
is "slice12: the three-model SHA256SUMS is byte-reproducible" \
   "$(sha256sum < "$RD/lib/SHA256SUMS")" "$(sha256sum < "$RB/stage2/opt/denso/lib/SHA256SUMS")"

# Refusals that still hold now that Float is approved: an unpaired Float engine,
# and a Float engine that is NOT in models.approved. Both must stop the build —
# approving the family must not soften the pair rule or the approval rule.
mkdir -p "$RB/nosidecar" "$RB/stage3" "$RB/stage4"
cp "$RB/src/float-big.engine" "$RB/nosidecar/float-big.engine"
stage_model_payload "$RB/stage3" "$REPO" "$RB/models.approved" "$RB/desc" "-" \
    "$RB/nosidecar/float-big.engine" >"$RB/stage3.out" 2>&1
rc_is "slice12: a Float engine with no sidecar is REFUSED" $? 1
# ...and the exit status alone is not the evidence: assert the REASON, so an
# incidental earlier failure cannot pass as the pair rule doing its job.
grep -q 'float-big has no sidecar' "$RB/stage3.out" \
    && ok "slice12: the no-sidecar refusal names the missing sidecar" \
    || bad "slice12: the no-sidecar refusal names the missing sidecar"
grep -v '^float-big ' "$RB/models.approved" > "$RB/models.approved.nobig"
stage_model_payload "$RB/stage4" "$REPO" "$RB/models.approved.nobig" "$RB/desc" "-" \
    "$RB/src/digitv3.engine" "$RB/src/float-big.engine" >"$RB/stage4.out" 2>&1
rc_is "slice12: an unapproved Float stem is REFUSED even though the family is approved" $? 1
grep -q 'float-big is not in models.approved' "$RB/stage4.out" \
    && ok "slice12: the unapproved-stem refusal names the approval rule" \
    || bad "slice12: the unapproved-stem refusal names the approval rule"
# Neither refusal may leave the rejected pair staged.
is "slice12: a refused Float engine is not left in the payload" \
   "$(find "$RB/stage3" "$RB/stage4" -name 'float-big.*' 2>/dev/null | wc -l | tr -d ' ')" "0"

# ── seed-manifest decision (every branch) + the atomic write mechanics.
SM="$T/seedm"; mkdir -p "$SM/pkg" "$SM/data"
printf 'ENGINEBYTES' > "$SM/pkg/digitv3.engine"; printf '["0"]' > "$SM/pkg/digitv3.names.json"
PEH="$(sha256sum "$SM/pkg/digitv3.engine" | cut -d' ' -f1)"; PSH="$(sha256sum "$SM/pkg/digitv3.names.json" | cut -d' ' -f1)"
python3 - "$SM/pkgmanifest.json" "$PEH" "$PSH" <<'PY'
import json, sys
p, e, s = sys.argv[1:4]
json.dump({"schema": 2, "generations": [{"name": "digitv3", "canonical_id": "digitv3",
    "runtime": {"tensorrt": {"engine": "digitv3.engine", "engine_sha256": e,
    "sidecar": "digitv3.names.json", "sidecar_sha256": s}}}]}, open(p, "w"))
PY
sm_reset() { rm -rf "$SM/data"; mkdir -p "$SM/data";
    cp "$SM/pkg/digitv3.engine" "$SM/data/digitv3.engine"; cp "$SM/pkg/digitv3.names.json" "$SM/data/digitv3.names.json"; }
DEC() { seed_manifest_decide "$SM/pkgmanifest.json" "$SM/pkg" "$SM/data" "$SM/data/manifest.json"; }

# (13) matching artifacts, no manifest -> seed
sm_reset
is "slice5: seed-manifest on matching artifacts with no manifest -> seed" "$(DEC)" "seed"
# (12) fresh flow: seed the pair then seed the manifest -> all three present
install_manifest_atomic "$SM/pkgmanifest.json" "$SM/data/manifest.json" >/dev/null 2>&1
[ -f "$SM/data/digitv3.engine" ] && [ -f "$SM/data/digitv3.names.json" ] && [ -f "$SM/data/manifest.json" ] \
    && ok "slice5: fresh flow yields engine + sidecar + manifest together" \
    || bad "slice5: fresh flow yields engine + sidecar + manifest together"
# (14) second run is a no-op: decision current, and a write would not change bytes/mtime
MT1="$(stat -c %Y "$SM/data/manifest.json")"; H1="$(sha256sum "$SM/data/manifest.json" | cut -d' ' -f1)"
is "slice5: second seed-manifest run decision is 'current'" "$(DEC)" "current"
sleep 1
install_manifest_atomic "$SM/pkgmanifest.json" "$SM/data/manifest.json" >/dev/null 2>&1; SEC_RC=$?
rc_is "slice5: a second atomic write into an existing manifest reports 'already exists' (rc=4)" "$SEC_RC" 4
is "slice5: second run preserves manifest bytes"  "$(sha256sum "$SM/data/manifest.json" | cut -d' ' -f1)" "$H1"
is "slice5: second run preserves manifest mtime"  "$(stat -c %Y "$SM/data/manifest.json")" "$MT1"
NTMP="$(find "$SM/data" -name '.manifest.json.*' | wc -l | tr -d ' ')"
is "slice5: no stale .manifest.json.* temp remains" "$NTMP" "0"

# (15) reformatted-but-equivalent existing manifest -> accepted without change
sm_reset
python3 - "$SM/pkgmanifest.json" "$SM/data/manifest.json" <<'PY'
import json, sys
d = json.load(open(sys.argv[1])); json.dump(d, open(sys.argv[2], "w"), indent=4, sort_keys=True)
PY
RH="$(sha256sum "$SM/data/manifest.json" | cut -d' ' -f1)"
is "slice5: a reformatted-equivalent manifest reads as 'current'" "$(DEC)" "current"
is "slice5: the reformatted manifest is left byte-for-byte unchanged" "$(sha256sum "$SM/data/manifest.json" | cut -d' ' -f1)" "$RH"

# (16) a canonically-different manifest -> refuse, unchanged
sm_reset
python3 - "$SM/pkgmanifest.json" "$SM/data/manifest.json" <<'PY'
import json, sys
d = json.load(open(sys.argv[1])); d["generations"][0]["canonical_id"] = "tampered"
json.dump(d, open(sys.argv[2], "w"))
PY
DH="$(sha256sum "$SM/data/manifest.json" | cut -d' ' -f1)"
is "slice5: a differing manifest is refused" "$(DEC)" "target-differs"
is "slice5: the differing manifest is left unchanged" "$(sha256sum "$SM/data/manifest.json" | cut -d' ' -f1)" "$DH"

# (17,18) engine hash mismatch -> refuse, and NO manifest is created
sm_reset; printf 'DIFFERENT-ENGINE' > "$SM/data/digitv3.engine"
is "slice5: an engine hash mismatch refuses" "$(DEC)" "data-artifact-differs:digitv3"
[ -f "$SM/data/manifest.json" ] && bad "slice5: engine mismatch creates no manifest" || ok "slice5: engine mismatch creates no manifest"
# (19) sidecar hash mismatch -> refuse
sm_reset; printf '["WRONG"]' > "$SM/data/digitv3.names.json"
is "slice5: a sidecar hash mismatch refuses" "$(DEC)" "data-artifact-differs:digitv3"

# a symlink target is refused (never written through). Symlink creation is not
# available on MSYS2, so this is guarded — it runs on the Linux appliance.
sm_reset
if ln -s /etc/hostname "$SM/data/manifest.json" 2>/dev/null; then
    is "slice5: a symlink manifest target is refused" "$(DEC)" "target-symlink"
    rm -f "$SM/data/manifest.json"
else
    echo "skip - slice5: symlink-target refusal (symlinks unavailable on $(uname -s 2>/dev/null || echo unknown))"
fi

# (20) a failed/interrupted write leaves no truncated manifest and no stale temp
sm_reset
install_manifest_atomic "$SM/nonexistent-src.json" "$SM/data/manifest.json" >/dev/null 2>&1
rc_is "slice5: atomic write with a missing source fails" $? 1
[ -f "$SM/data/manifest.json" ] && bad "slice5: a failed write leaves no manifest" || ok "slice5: a failed write leaves no manifest"
is "slice5: a failed write leaves no stale temp" "$(find "$SM/data" -name '.manifest.json.*' | wc -l | tr -d ' ')" "0"

# (21) concurrent invocation cannot publish conflicting/truncated output: the
# second create-if-absent must NOT clobber the first, and the winner's bytes stay.
sm_reset
install_manifest_atomic "$SM/pkgmanifest.json" "$SM/data/manifest.json" >/dev/null 2>&1
W1="$(sha256sum "$SM/data/manifest.json" | cut -d' ' -f1)"
# a "loser" with different content must not overwrite
printf '{"schema":2,"generations":[]}' > "$SM/other.json"
install_manifest_atomic "$SM/other.json" "$SM/data/manifest.json" >/dev/null 2>&1
rc_is "slice5: a concurrent create-if-absent does not overwrite (rc=4)" $? 4
is "slice5: the first writer's bytes survive a racing second write" "$(sha256sum "$SM/data/manifest.json" | cut -d' ' -f1)" "$W1"

# (22) verify DISTINGUISHES missing / matching / differing (the manifests_equivalent
#      verdicts verify branches on).
sm_reset
manifests_equivalent "$SM/pkgmanifest.json" "$SM/data/manifest.json" >/dev/null 2>&1
rc_is "slice5: verify sees a MISSING manifest as absent (equiv rc=2)" $? 2
cp "$SM/pkgmanifest.json" "$SM/data/manifest.json"
manifests_equivalent "$SM/pkgmanifest.json" "$SM/data/manifest.json" >/dev/null 2>&1
rc_is "slice5: verify sees a MATCHING manifest (equiv rc=0)" $? 0
python3 - "$SM/pkgmanifest.json" "$SM/data/manifest.json" <<'PY'
import json, sys
d = json.load(open(sys.argv[1])); d["generations"][0]["canonical_id"] = "z"
json.dump(d, open(sys.argv[2], "w"))
PY
manifests_equivalent "$SM/pkgmanifest.json" "$SM/data/manifest.json" >/dev/null 2>&1
rc_is "slice5: verify sees a DIFFERING manifest (equiv rc=1)" $? 1

# (23) verify changes NOTHING under models/: snapshot the subtree (hash+mtime+
#      type+name) around the read verify performs.
snap_models() { ( cd "$1" && LC_ALL=C find . -mindepth 1 | LC_ALL=C sort | while IFS= read -r p; do
        if [ -d "$p" ]; then printf 'd|%s|%s\n' "$(stat -c %Y "$p")" "$p"
        elif [ -f "$p" ]; then printf 'f|%s|%s|%s\n' "$(stat -c %Y "$p")" "$(sha256sum "$p" | cut -d' ' -f1)" "$p"
        else printf 'o|%s\n' "$p"; fi; done ) }
sm_reset; cp "$SM/pkgmanifest.json" "$SM/data/manifest.json"
BEFORE="$(snap_models "$SM/data")"
manifests_equivalent "$SM/pkgmanifest.json" "$SM/data/manifest.json" >/dev/null 2>&1   # the verify read
manifest_matches_models_dir "$SM/pkgmanifest.json" "$SM/data" >/dev/null 2>&1           # ...and this one
AFTER="$(snap_models "$SM/data")"
is "slice5: verify's manifest read changes no hash/mtime/type under models/" "$AFTER" "$BEFORE"

# (24) no `verify --repair` ENTRY POINT exists. The word appears only in prose
#      comments explaining that it does NOT exist, so strip comments first and
#      look for any real `repair` token (an option or a dispatcher case).
sed 's/#.*//' "$REPO/packaging/denso-setup" | grep -Eq 'repair' \
    && bad "slice5: no verify --repair entry point exists" \
    || ok "slice5: no verify --repair entry point exists"

# (25) NO maintainer script invokes seed-manifest, and no OTHER cmd_* function in
#      denso-setup CALLS cmd_seed_manifest. A user-facing NOTE that mentions the
#      external 'denso-setup seed-manifest' command in help text is allowed; a
#      call to the internal cmd_seed_manifest function is not.
INVOKERS=0
for f in "$REPO/packaging/debian/postinst" "$REPO/packaging/debian/prerm" "$REPO/packaging/debian/postrm"; do
    grep -Eq 'seed[-_]manifest' "$f" && INVOKERS=$((INVOKERS+1))
done
for fn in cmd_configure cmd_verify cmd_preflight cmd_unconfigure cmd_replace_model; do
    awk -v f="$fn" '$0 ~ "^"f"\\(\\)" {inb=1} inb && /^}/ {inb=0; next} inb {print}' \
        "$REPO/packaging/denso-setup" | grep -Eq 'cmd_seed_manifest' \
        && INVOKERS=$((INVOKERS+1))
done
is "slice5: no maintainer script / configure / verify invokes seed-manifest" "$INVOKERS" "0"

fi  # python3 available

rm -rf "$T"
echo; echo "passed: $pass   failed: $fail"
[ "$fail" -eq 0 ]
