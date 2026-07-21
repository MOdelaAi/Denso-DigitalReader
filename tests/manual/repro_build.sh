#!/usr/bin/env bash
# Reproducibility gate for the packaging pipeline. JETSON-ONLY: it runs the real
# tools/build_package.sh, which hard-refuses to run off an aarch64 JetPack 6.2
# box. Run from the repo root:
#
#   tests/manual/repro_build.sh models/digitv3.engine
#
# WHY this exists. A clean build's artifacts are named `r<count>.g<sha>` with no
# content hash, so the NAME is a truthful identifier only if one commit yields
# one set of bytes. That was false: the MANIFEST embedded a wall-clock `built:`
# time and gzip stamped its own header, so rebuilding a commit produced
# different bytes under an IDENTICAL filename and silently replaced the earlier
# artifact — including one an operator may already have shipped and recorded.
# The unit-level half is in tests/packaging/run.sh; only a real double build
# proves the .deb itself.
set -u

ENGINE="${1-}"
[ -n "$ENGINE" ] || { echo "usage: $0 <path/to.engine>" >&2; exit 2; }
[ -f "$ENGINE" ] || { echo "no such engine: $ENGINE" >&2; exit 2; }
[ "$(uname -m)" = "aarch64" ] || { echo "must run on the Jetson (this drives the real build)" >&2; exit 2; }

pass=0; fail=0
ok()  { echo "ok   - $1"; pass=$((pass+1)); }
bad() { echo "FAIL - $1"; fail=$((fail+1)); }
is()  { if [ "$2" = "$3" ]; then ok "$1"; else bad "$1 (want '$3', got '$2')"; fi; }

# Refuse to run against a dirty tree. This script deliberately DIRTIES the tree
# for its second half; starting from someone's uncommitted work would mean the
# restore below silently discards it.
[ -z "$(git status --porcelain)" ] || {
    echo "refusing: the tree is dirty. This gate makes its own controlled edits and" >&2
    echo "restores them; starting dirty risks destroying uncommitted work." >&2
    exit 2; }

# The controlled edit lands in a file that is PACKAGED (/opt/denso/lib/policy.sh)
# but is not C++ — so a rebuild is seconds, not minutes, while still changing
# the .deb's payload for real.
CANARY=packaging/lib/policy.sh
restore() { git checkout -- "$CANARY" 2>/dev/null || true; }
trap restore EXIT INT TERM

sha_of() { sha256sum "$1" | cut -d' ' -f1; }
build()  { ./tools/build_package.sh --model "$ENGINE" "$@" >/tmp/repro_build.log 2>&1 \
             || { echo "BUILD FAILED — tail of /tmp/repro_build.log:" >&2; tail -20 /tmp/repro_build.log >&2; return 1; }; }

echo "== clean build, twice (dist/ is cleared first; it is git-ignored output)"
rm -f dist/*
build || exit 1
DEB1="$(ls dist/*.deb)"; TAR1="$(ls dist/*.tar.gz)"
DEB1_SHA="$(sha_of "$DEB1")"; TAR1_SHA="$(sha_of "$TAR1")"
DEB1_NAME="$(basename "$DEB1")"; TAR1_NAME="$(basename "$TAR1")"

# A second build must land on the SAME names — that is the premise being
# tested. Do not clear dist/: overwriting in place is exactly the scenario that
# used to lose an artifact, so let it happen and then compare.
sleep 2   # cross a wall-clock second: a `date`-derived stamp would now differ
build || exit 1
DEB2="$(ls dist/*.deb)"; TAR2="$(ls dist/*.tar.gz)"

is "clean: the .deb name is stable across rebuilds"    "$(basename "$DEB2")" "$DEB1_NAME"
is "clean: the bundle name is stable across rebuilds"  "$(basename "$TAR2")" "$TAR1_NAME"
is "clean: the .deb is BYTE-IDENTICAL across rebuilds" "$(sha_of "$DEB2")"   "$DEB1_SHA"
is "clean: the bundle is BYTE-IDENTICAL across rebuilds" "$(sha_of "$TAR2")" "$TAR1_SHA"

# The stamp must come from the COMMIT, not the clock. If it did not, the two
# builds above would already have differed — but assert the field directly so a
# failure names the cause instead of leaving it to be diagnosed.
MANIFEST_DATE="$(dpkg-deb --fsys-tarfile "$DEB2" | tar xO ./opt/denso/MANIFEST 2>/dev/null | sed -n 's/^source-date: //p')"
COMMIT_DATE="$(date -u -d "@$(git log -1 --format=%ct)" -Is)"
case "$MANIFEST_DATE" in
    "$COMMIT_DATE"*) ok "clean: MANIFEST source-date is the COMMIT time, not the build time" ;;
    *) bad "clean: MANIFEST source-date is the COMMIT time (want '$COMMIT_DATE...', got '$MANIFEST_DATE')" ;;
esac

# A caller-set SOURCE_DATE_EPOCH would break the clean guarantee by another
# door: same r<count>.g<sha> filename, different bytes. It must be refused for
# a clean build, not silently honoured (the usual reproducible-builds
# convention is wrong here — the name carries no content hash).
if SOURCE_DATE_EPOCH=1600000000 ./tools/build_package.sh --model "$ENGINE" >/tmp/repro_override.log 2>&1; then
    bad "clean: a MISMATCHED SOURCE_DATE_EPOCH is refused"
else
    grep -q "differs from the commit timestamp" /tmp/repro_override.log \
        && ok "clean: a MISMATCHED SOURCE_DATE_EPOCH is refused, and says why" \
        || bad "clean: refusal names the cause (see /tmp/repro_override.log)"
fi
# ...but a MATCHING override is a no-op, so pinning the value explicitly works.
if SOURCE_DATE_EPOCH="$(git log -1 --format=%ct)" ./tools/build_package.sh --model "$ENGINE" >/dev/null 2>&1; then
    is "clean: a MATCHING SOURCE_DATE_EPOCH still yields identical bytes" "$(sha_of "$(ls dist/*.deb)")" "$DEB1_SHA"
else
    bad "clean: a MATCHING SOURCE_DATE_EPOCH is accepted"
fi

echo "== dirty builds stay disambiguated"
# Same tree, built twice dirty: identical inputs, so identical bytes AND an
# identical name. Overwriting is a harmless no-op — that is the claim.
printf '\n# repro-gate canary A\n' >> "$CANARY"
build --allow-dirty || exit 1
DA1="$(ls dist/*+dirty*.tar.gz)"; DA1_SHA="$(sha_of "$DA1")"
sleep 2
build --allow-dirty || exit 1
DA2="$(ls dist/*+dirty*.tar.gz)"
is "dirty: an UNCHANGED dirty tree rebuilds to the same name"  "$(basename "$DA2")" "$(basename "$DA1")"
is "dirty: an UNCHANGED dirty tree rebuilds byte-identically"  "$(sha_of "$DA2")"   "$DA1_SHA"

# Now a MATERIALLY different dirty tree at the SAME commit: the version string
# is identical, so only the hash suffix can keep these apart.
restore
printf '\n# repro-gate canary B -- different content\n' >> "$CANARY"
build --allow-dirty || exit 1
DB1="$(ls dist/*+dirty*.tar.gz | grep -v "$(basename "$DA1")" || true)"
if [ -n "$DB1" ] && [ "$(basename "$DB1")" != "$(basename "$DA1")" ]; then
    ok "dirty: a DIFFERENT dirty tree gets a DIFFERENT bundle name"
else
    bad "dirty: a DIFFERENT dirty tree gets a DIFFERENT bundle name (A=$(basename "$DA1") B=${DB1:-<none>})"
fi
[ -f "$DA1" ] && ok "dirty: the earlier bundle SURVIVED (no silent overwrite)" \
              || bad "dirty: the earlier bundle SURVIVED (no silent overwrite)"

restore
echo
echo "passed: $pass   failed: $fail"
[ "$fail" -eq 0 ]
