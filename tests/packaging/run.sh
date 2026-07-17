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

# If an admin changed the user AFTER us, restore must refuse rather than clobber.
gdm_set_autologin "$T/gdm.conf" modela
sed -i 's/^AutomaticLogin=modela$/AutomaticLogin=someoneelse/' "$T/gdm.conf"
gdm_restore_autologin "$T/gdm.conf" modela "false" ""
rc_is "gdm: refuses to clobber an admin's later USER change" $? 1
grep -q '^AutomaticLogin=someoneelse$' "$T/gdm.conf" && ok "gdm: admin's value survives" || bad "gdm: admin's value survives"

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

rm -rf "$T"
echo; echo "passed: $pass   failed: $fail"
[ "$fail" -eq 0 ]
