#!/usr/bin/env bash
# Build the Denso .deb. MUST run on an aarch64 JetPack 6.2 Jetson — there is no
# cross-toolchain, and the engines are sm_87/TRT-10.3 pinned.
#
#   tools/build_package.sh --model models/digitv3.engine [--allow-dirty]
set -euo pipefail

HERE="$(cd "$(dirname "$0")/.." && pwd)"
. "$HERE/packaging/lib/policy.sh"
# The standalone preflight-denso.sh emitted below is the same generator the
# test harness calls, so the two never drift apart — see gen_preflight.sh.
. "$HERE/packaging/lib/gen_preflight.sh"
# Same split, same reason: the transport bundle is assembled by a sourceable
# emitter so tests/packaging/run.sh can prove its shape on the dev box, which
# can never run this script (aarch64/JetPack contract below).
. "$HERE/packaging/lib/gen_bundle.sh"

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

# ── JetPack 6.2 / L4T R36.5.0 platform contract — HARD-enforced, not just
# aarch64. An aarch64 box running a different L4T/TensorRT/CUDA stack can
# still produce a package whose control file and MANIFEST swear R36.5/JP6.2
# while the shipped engines are pinned to TRT 10.3 / sm_87 — this is a
# build-time contract, so a warning is not enough; refuse the build outright.
# Same L4T-parsing expression `denso-setup verify` uses (packaging/denso-setup)
# so the two checks can never quietly diverge on what "36.5.0" means.
echo ">> verifying build-host platform contract (JetPack 6.2 / L4T R36.5.0)"
CONTRACT_FAIL=0
DPKG_ARCH="$(dpkg --print-architecture)"
if [ "$DPKG_ARCH" = "arm64" ]; then
    echo "   ok   dpkg architecture: $DPKG_ARCH"
else
    echo "   FAIL dpkg architecture is '$DPKG_ARCH', expected arm64" >&2
    CONTRACT_FAIL=1
fi
L4T="$(sed -n 's/^# R\([0-9]*\).*REVISION: \([0-9.]*\).*/\1.\2/p' /etc/nv_tegra_release 2>/dev/null | head -1)"
if [ "$L4T" = "36.5.0" ]; then
    echo "   ok   L4T: $L4T"
else
    echo "   FAIL L4T is '${L4T:-unknown}', expected exactly 36.5.0 -- the shipped engines are TRT 10.3/sm_87 pinned to this image" >&2
    CONTRACT_FAIL=1
fi
# Presence is NOT the contract: the package name `libnvinfer10` pins only the
# SONAME major, so it can hold ANY 10.x -- while the shipped engines are built
# for TRT 10.3 exactly and a 10.x mismatch fails at deserialize, on the
# appliance, not here. Assert the declared baseline (TRT 10.3 / CUDA 12.6) by
# version, and PRINT it, so the manifest's claim is one the build verified.
check_version() {  # <pkg> <expected-prefix> <human>
    _v="$(dpkg-query -W -f='${Version}' "$1" 2>/dev/null || echo "")"
    if [ -z "$_v" ]; then
        echo "   FAIL $1 is not installed (need $3)" >&2; CONTRACT_FAIL=1; return
    fi
    case "$_v" in
        "$2"*) echo "   ok   $1: $_v" ;;
        *) echo "   FAIL $1 is $_v, expected $3 (engines are pinned to it)" >&2; CONTRACT_FAIL=1 ;;
    esac
}
check_version libnvinfer10     "10.3."   "TensorRT 10.3"
check_version cuda-cudart-12-6 "12.6."   "CUDA 12.6"
[ "$CONTRACT_FAIL" = "0" ] || { echo "build-host platform contract not satisfied -- refusing to build a package that would misrepresent its target platform" >&2; exit 1; }

# ── version: 0.1.0+g<sha>, dirty-marked. It becomes a dpkg field AND a path.
APP_VERSION="$(sed -n 's/.*APP_VERSION="\([^"]*\)".*/\1/p' src/app/CMakeLists.txt | head -1)"
SHA="$(git rev-parse --short HEAD)"
DIRTY=""
# --porcelain, NOT `git diff --quiet`: diff ignores UNTRACKED files, so a tree
# with an untracked source file would package as "clean" and the manifest would
# lie about what was built.
if [ -n "$(git status --porcelain)" ]; then
    [ "$ALLOW_DIRTY" = "1" ] || { echo "refusing to package a dirty tree (use --allow-dirty to override)" >&2; exit 1; }
    DIRTY="+dirty"
fi
# A git SHA is NOT monotonic, and dpkg compares non-digits by ASCII: the installed
# 0.1.0+gda30437 sorts ABOVE a newer 0.1.0+g7a2d661 ("gd" > "g7"), so apt calls the
# new build a DOWNGRADE and refuses it under -y. Every install was a coin flip.
# Prefix the commit COUNT, which increases with history and compares numerically
# (r105 > r99 regardless of the sha that follows). The sha stays for provenance.
COUNT="$(git rev-list --count HEAD)"
VERSION="${APP_VERSION}+r${COUNT}.g${SHA}${DIRTY}"
version_ok "$VERSION" || { echo "computed version is not a safe path component: $VERSION" >&2; exit 1; }
echo ">> version: $VERSION"

# ── REPRODUCIBILITY. A clean build's version is r<count>.g<sha> — a claim that
# the artifact is identified by its commit. That claim was FALSE: the MANIFEST
# embedded `date -Is` and dpkg-deb stamped the current time into the archive,
# so rebuilding one commit produced different bytes under the SAME filename,
# silently overwriting the earlier artifact. Anything named after a commit must
# be a function of that commit alone.
#
# The commit timestamp IS the source, and for a clean build it is the only
# permitted value. Honouring an arbitrary externally-set SOURCE_DATE_EPOCH — the
# usual reproducible-builds convention — would reintroduce the exact defect this
# closes by another door: two clean builds of one commit with different epochs
# produce different bytes under the SAME r<count>.g<sha> filename. The name
# carries no content hash, so it can only be honest if the epoch is a function
# of the commit. A matching override is accepted (it is a no-op, and lets a
# caller pin the value explicitly); a differing one is refused, loudly, rather
# than silently ignored.
#
# A DIRTY build may override freely: its bundle name carries the .deb's content
# hash, so a different epoch yields a different name and nothing is masked.
COMMIT_EPOCH="$(git log -1 --format=%ct HEAD)"
if [ -n "${SOURCE_DATE_EPOCH:-}" ] && [ "$SOURCE_DATE_EPOCH" != "$COMMIT_EPOCH" ]; then
    if [ -z "$DIRTY" ]; then
        echo "refusing: SOURCE_DATE_EPOCH=$SOURCE_DATE_EPOCH differs from the commit timestamp $COMMIT_EPOCH." >&2
        echo "  A clean build is named r<count>.g<sha> with no content hash, so its bytes must be a" >&2
        echo "  function of the commit alone. Unset it, or set it to $COMMIT_EPOCH." >&2
        exit 1
    fi
    echo ">> NOTE: SOURCE_DATE_EPOCH overridden on a dirty build (name carries the content hash)"
else
    SOURCE_DATE_EPOCH="$COMMIT_EPOCH"
fi
case "$SOURCE_DATE_EPOCH" in
    ''|*[!0-9]*) echo "SOURCE_DATE_EPOCH is not a positive integer: '$SOURCE_DATE_EPOCH'" >&2; exit 1 ;;
esac
# Exported because dpkg-deb reads it from the ENVIRONMENT: it stamps the ar
# container and clamps every tar entry's mtime to it, which is what makes the
# .deb itself byte-stable.
export SOURCE_DATE_EPOCH
echo ">> source date: $SOURCE_DATE_EPOCH ($(date -u -d "@$SOURCE_DATE_EPOCH" -Is))"
# A DIRTY tree is not described by its commit, so its bytes are not a function
# of SOURCE_DATE_EPOCH either — the uncommitted content is what varies. Dirty
# builds stay disambiguated by the .deb-hash suffix on the bundle name (below);
# reproducibility is a CLEAN-build guarantee only, and the docs say so.

# ── models: explicit, and hash-checked against the tracked approval manifest.
# The PAIR is approved, never the engine alone: TrtEngine's ctor reads
# <stem>.names.json for class names, so a wrong/modified sidecar changes
# application semantics exactly as much as a wrong engine would, and would
# otherwise ship unchecked as long as the engine's own hash matched.
SEEN_STEMS=""
for m in "${MODELS[@]}"; do
    [ -f "$m" ] || { echo "no such engine: $m" >&2; exit 1; }
    stem="$(basename "$m" .engine)"
    # Reject a duplicate stem BEFORE staging: passing the same --model twice
    # would silently overwrite during `install -m` staging and the MANIFEST
    # would list the same model twice — neither failure is loud.
    case " $SEEN_STEMS " in
        *" $stem "*) echo "duplicate --model stem '$stem' -- pass each engine once" >&2; exit 1 ;;
    esac
    SEEN_STEMS="$SEEN_STEMS $stem"
    side="$(dirname "$m")/$stem.names.json"
    [ -f "$side" ] || { echo "engine $stem has no sidecar: $side" >&2; exit 1; }
    want_eng="$(awk -v s="$stem" '$1==s {print $2}' packaging/models.approved)"
    want_side="$(awk -v s="$stem" '$1==s {print $3}' packaging/models.approved)"
    [ -n "$want_eng" ] && [ -n "$want_side" ] || { echo "engine '$stem' is not in packaging/models.approved — approve it explicitly" >&2; exit 1; }
    got_eng="$(sha256sum "$m" | cut -d' ' -f1)"
    got_side="$(sha256sum "$side" | cut -d' ' -f1)"
    [ "$want_eng" = "$got_eng" ] || { echo "engine '$stem' HASH MISMATCH:" >&2; echo "  approved: $want_eng" >&2; echo "  actual:   $got_eng" >&2; exit 1; }
    [ "$want_side" = "$got_side" ] || { echo "engine '$stem' SIDECAR HASH MISMATCH:" >&2; echo "  approved: $want_side" >&2; echo "  actual:   $got_side" >&2; exit 1; }
    echo ">> model approved: $stem (engine $got_eng, sidecar $got_side)"
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

ARCH="$DPKG_ARCH"   # already verified == arm64 by the platform contract above

# ── DEPENDENCY DERIVATION — the distro's own tool, as the spec asks.
#
# dpkg-shlibdeps hard-errors on /usr/local/cuda/lib64/libcudart.so.12 and on
# NVIDIA's libopencv (neither ships .shlibs/.symbols). The fix is NOT
# --ignore-missing-info (that DROPS the dep silently while still looking
# derived) — it is packaging/debian/shlibs.local, which supplies exactly that
# missing metadata. Verified on the Jetson: with it, shlibdeps exits 0 and emits
# the full version-constrained set including libnvinfer10 and libc6.
# ── shlibs.local is only trustworthy if its mappings are TRUE on this box.
# dpkg-shlibdeps trusts this file blindly and will emit a dependency on a
# package that does not exist (an earlier draft mapped libcudla -> nvidia-l4t-cuda;
# the real owner is libcudla-12-6, so `apt install` would have failed on a
# correct box). Assert ownership before using it.
echo ">> verifying packaging/debian/shlibs.local mappings against this box"
# The set of SONAMEs the exe actually, directly, links against -- the ONLY
# thing shlibs.local mappings are allowed to correspond to. dpkg-shlibdeps
# only ever consults DIRECT NEEDED entries, so a mapping for anything else
# (a transitive lib, or one that stopped being linked) is dead weight that
# rots silently while still looking correctly-owned; two such entries already
# had to be found and removed by hand (libcudla, libopencv_imgcodecs).
NEEDED="$(objdump -p "$EXE" | awk '/NEEDED/{print $2}')"
bad_map=0
while read -r libname sover dep rest; do
    case "${libname-}" in ''|'#'*) continue ;; esac
    dep="${dep%%(*}"; dep="${dep// /}"     # defensive: dep should already be bare
    soname="$libname.so.$sover"

    if ! printf '%s\n' "$NEEDED" | grep -qxF "$soname"; then
        echo "   BAD  $soname is mapped in shlibs.local but is not a direct NEEDED entry of $EXE (dead mapping — remove it)" >&2
        bad_map=1; continue
    fi

    # NO `exit` in these awks. With `set -o pipefail`, an awk that exits early
    # closes the pipe, the producer (ldconfig/ldd) takes SIGPIPE, pipefail
    # propagates 141 and `set -e` kills the build. It is also a RACE — it only
    # fires when the match appears early enough that the producer is still
    # writing, which is why libcudart passed and libopencv_core did not. Consume
    # the whole stream and keep the first match instead.
    lib="$(ldconfig -p | awk -v s="$soname" '$1==s && !f {print $NF; f=1}')"
    [ -n "$lib" ] || lib="$(ldd "$EXE" | awk -v s="$soname" '$1==s && !f {print $3; f=1}')"
    if [ -z "$lib" ] || [ ! -e "$lib" ]; then
        echo "   BAD  $soname — not resolvable on this box" >&2; bad_map=1; continue
    fi
    owner="$(dpkg-query -S "$(readlink -f "$lib")" 2>/dev/null | cut -d: -f1 | head -1)"
    if [ "$owner" != "$dep" ]; then
        echo "   BAD  $soname -> mapped to '$dep' but dpkg says '${owner:-<unowned>}'" >&2; bad_map=1; continue
    fi
    dpkg-query -W -f='${Status}' "$dep" 2>/dev/null | grep -q "install ok installed" || {
        echo "   BAD  mapped package '$dep' is not installed" >&2; bad_map=1; continue; }

    # A bare package-name match is not enough: shlibdeps will happily emit an
    # UNSATISFIABLE `libopencv (>= 999)` if this file lies about the floor, and
    # that only fails at `apt install` time on the appliance -- far from here.
    if [ -n "$rest" ]; then
        floor="$(printf '%s' "$rest" | sed -n 's/^(>= \(.*\))$/\1/p')"
        if [ -z "$floor" ]; then
            echo "   BAD  $soname -> unsupported version-constraint syntax '$rest' (only '(>= X)' is handled)" >&2
            bad_map=1; continue
        fi
        installed_ver="$(dpkg-query -W -f='${Version}' "$dep" 2>/dev/null)"
        if ! dpkg --compare-versions "$installed_ver" ge "$floor"; then
            echo "   BAD  $soname -> $dep declares floor >= $floor but installed is '$installed_ver'" >&2
            bad_map=1; continue
        fi
        echo "   ok   $soname -> $dep (>= $floor, installed $installed_ver)"
    else
        echo "   ok   $soname -> $dep"
    fi
done < packaging/debian/shlibs.local
[ "$bad_map" = "0" ] || { echo "shlibs.local has wrong/dead mappings — fix them; shlibdeps trusts this file blindly" >&2; exit 1; }

echo ">> deriving Depends: with dpkg-shlibdeps"
SHLIBDIR="$(mktemp -d)"
mkdir -p "$SHLIBDIR/debian"
cp packaging/debian/shlibs.local "$SHLIBDIR/debian/shlibs.local"
printf 'Source: denso-digitalreader
Package: denso-digitalreader
Architecture: %s
Depends: ${shlibs:Depends}
Description: x
' "$ARCH" > "$SHLIBDIR/debian/control"
# Stage at the REAL package path, opt/denso/bin/denso, not a bare filename
# under debian/denso-digitalreader/. dpkg-shlibdeps expects the binary to
# already be installed at its package-relative location -- staging it at the
# wrong path is exactly what produces its "binaries to analyze should already
# be installed in their package's directory" warning.
mkdir -p "$SHLIBDIR/debian/denso-digitalreader/opt/denso/bin"
cp "$EXE" "$SHLIBDIR/debian/denso-digitalreader/opt/denso/bin/denso"
# NO --ignore-missing-info: if this errors, a dependency is genuinely
# underivable and shlibs.local needs a line — do not paper over it.
SHLIBS_LINE="$( cd "$SHLIBDIR" && dpkg-shlibdeps -O debian/denso-digitalreader/opt/denso/bin/denso )" || {
    echo "dpkg-shlibdeps FAILED — add the missing SONAME to packaging/debian/shlibs.local" >&2
    rm -rf "$SHLIBDIR"; exit 1; }
rm -rf "$SHLIBDIR"
# Require exactly the expected shape with a non-empty suffix: any non-empty
# output lacking the "shlibs:Depends=" prefix (a warning line, a different
# -O field, malformed output) must never silently become SHLIBS_DEPENDS.
case "$SHLIBS_LINE" in
    shlibs:Depends=?*) : ;;
    *) echo "dpkg-shlibdeps produced unexpected output: '$SHLIBS_LINE'" >&2; exit 1 ;;
esac
SHLIBS_DEPENDS="${SHLIBS_LINE#shlibs:Depends=}"
echo "   derived: $SHLIBS_DEPENDS"

sed -e "s/@VERSION@/$VERSION/" -e "s/@ARCH@/$ARCH/"     -e "s|@SHLIBS_DEPENDS@|$SHLIBS_DEPENDS|"     packaging/debian/control.in > "$STAGE/DEBIAN/control"
# A `>` redirect inherits the caller's umask, so this file is 0600 in a
# umask-077 shell and 0664 under the Jetson's default 002. That does NOT reach
# the .deb: `dpkg-deb --build` normalizes CONTROL-archive members to 0644
# regardless (verified on-device: staged 0600 -> 0644 inside the .deb). This
# chmod is therefore defense-in-depth, not a bug fix — it makes the staging tree
# say what ships instead of leaving the result to an external tool's
# normalization, which nothing here would notice changing.
# NOTE the asymmetry, because it is the part that actually bites: dpkg-deb does
# NOT normalize the DATA archive (staged 0600 -> 0600 in the .deb). Any future
# `>`-created PAYLOAD file needs its own chmod, as MANIFEST has below.
chmod 0644 "$STAGE/DEBIAN/control"

# ── MANIFEST: what this artifact IS, for after-the-fact diagnosis. Written
# BEFORE md5sums so md5sums covers it too (see below).
{
    echo "package: denso-digitalreader"
    echo "version: $VERSION"
    echo "arch: $ARCH"
    echo "source-sha: $SHA${DIRTY:+ (DIRTY TREE)}"
    # NOT `date -Is`. A wall-clock build time is the single field that would
    # make every rebuild of one commit differ, defeating the whole point of
    # naming the artifact after that commit. Named `source-date`, not `built`,
    # so it does not claim to be something it is not: it is the COMMIT's
    # timestamp, and for a dirty tree it describes the commit the tree is
    # based on, not when anyone pressed build.
    echo "source-date: $(date -u -d "@$SOURCE_DATE_EPOCH" -Is) (commit time; this file is reproducible, so it is NOT the wall-clock build time)"
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
        echo "model-recipe: $stem $(awk -v s="$stem" '$1==s {$1="";$2="";$3="";print}' packaging/models.approved | sed 's/^  *//')"
    done
    echo "--- ldd report (diagnostic evidence, not a dependency source) ---"
    # The load ADDRESSES are stripped. ldd prints where each library happened to
    # be mapped in that one run, and ASLR randomizes it every time — so the raw
    # output made the MANIFEST, and therefore the .deb, different on every build
    # of identical sources. That defeats naming an artifact after its commit,
    # and it is invisible: the file looks stable because only the hex changes.
    # The mapping (soname -> resolved path) is the diagnostic value; the
    # addresses carry none.
    ldd "$EXE" | sed 's/ (0x[0-9a-f]*)$//'
} > "$STAGE/opt/denso/MANIFEST"
# Pinned, not umask-inherited: this shipped 0664 from the Jetson's default 002.
chmod 0644 "$STAGE/opt/denso/MANIFEST"

# md5sums: MUST be the LAST payload write before dpkg-deb --build. Generating
# it any earlier (e.g. before MANIFEST is staged) means `dpkg -V` never
# verifies MANIFEST at all — the file that documents what this artifact IS
# would be the one thing integrity-checking silently skips. `dpkg-deb --build`
# itself does not generate md5sums, and without them `dpkg -V` verifies
# nothing.
( cd "$STAGE" && find . -type f ! -path './DEBIAN/*' -printf '%P\0'     | xargs -0 md5sum > DEBIAN/md5sums )
# Same defense-in-depth pin as DEBIAN/control above (also normalized by
# dpkg-deb). The class that genuinely can vary is the payload, and every path
# there arrives via `install -m` or an explicit chmod. The gate for all of it is
# the two-umask rebuild in tests/manual/repro_build.sh, which asserts modes in
# the real control AND data archives rather than trusting either rule.
chmod 0644 "$STAGE/DEBIAN/md5sums"

# ── build the .deb
# Outputs go to dist/, NOT the repo root. Writing them beside the sources made
# the build SELF-DEFEATING: the first run succeeded, its own artifacts left the
# tree dirty (they are untracked), and every later run then refused to package
# a dirty tree. dist/ is git-ignored, so the dirty-tree gate keeps meaning
# "the SOURCES are modified" rather than "you have built before".
mkdir -p dist
OUT="dist/denso-digitalreader_${VERSION}_${ARCH}.deb"
dpkg-deb --build --root-owner-group "$STAGE" "$OUT" >/dev/null
sha256sum "$OUT" > "$OUT.sha256"
# A `>` redirect takes the builder's umask (002 on the Jetson -> 0664). No
# emitted integrity artifact should depend on ambient umask; the bundle's
# SHA256SUMS is pinned for the same reason.
chmod 0644 "$OUT.sha256"
DEB_SHA256="$(cut -d' ' -f1 "$OUT.sha256")"

# ── PREFLIGHT GUARD, standalone: `denso-setup preflight` cannot protect a
# FIRST install, because denso-setup does not exist on the box until the .deb
# containing it is already installed. So we ALSO emit a standalone guard next
# to the .deb that works with nothing but this .deb and a shell — generated by
# the one shared emitter in packaging/lib/gen_preflight.sh (see there for why).
#
# The filename is VERSIONED and the .deb's own SHA-256 is embedded in it: every
# build used to overwrite the same preflight-denso.sh, so an operator could
# pair an OLD guard with a NEW .deb with no error at all. Binding the guard to
# the ONE artifact it was generated for (both by name and by checksum) makes
# that pairing mistake fail loudly instead of silently passing.
PREFLIGHT_OUT="dist/preflight-denso-${VERSION}.sh"
emit_preflight_script "packaging/lib/policy.sh" "$PREFLIGHT_OUT" "$DEB_SHA256"

# ── TRANSPORT BUNDLE: the .deb + its guard + checksums + instructions as ONE
# file, because the .deb and the guard are useless apart (the guard refuses any
# other .deb by embedded SHA-256) and the appliances that are NOT this build
# host each need the whole set moved to them.
#
# The loose files above are NOT superseded: the FIRST appliance is this build
# box itself, and its operator installs straight out of dist/ without ever
# unpacking an archive. Two audiences, two artifacts — see README "Deploy".
#
# A dirty build gets the .deb's own hash appended. Every `--allow-dirty` build
# at one commit produces the IDENTICAL version string, so two materially
# different archives would share a filename and a top-level directory name —
# one silently overwriting the other in dist/, or being unpacked over it. The
# .deb's version field is deliberately left alone (it is a dpkg ordering key);
# only the transport name disambiguates.
#
# Clean builds keep the plain name, and that is now TRUE rather than assumed:
# with SOURCE_DATE_EPOCH pinned, one commit yields one set of bytes, so the
# plain name cannot collide with a different artifact. Before that it could —
# a rebuild silently replaced the earlier .deb under an identical filename.
# The suffix is a CONTENT hash, so two dirty builds of an unchanged tree now
# land on the same name with the same bytes (a harmless no-op), while any real
# difference in the working tree produces a different name.
BUNDLE="denso-digitalreader_${VERSION}_${ARCH}"
[ -z "$DIRTY" ] || BUNDLE="${BUNDLE}.$(printf %.12s "$DEB_SHA256")"
BUNDLE_OUT="dist/${BUNDLE}.tar.gz"
emit_bundle "$OUT" "$PREFLIGHT_OUT" "$BUNDLE" "$BUNDLE_OUT" "$SOURCE_DATE_EPOCH"

echo
echo ">> built $OUT"
dpkg-deb --info "$OUT" | sed -n '1,12p'
# Print ONLY the guarded sequence. Advertising a bare `apt install` first --
# with preflight as an optional extra -- invites the operator to skip the
# protected-stack guard entirely, which is the same as not having one. The
# preflight is bound to THIS .deb by its SHA-256 and refuses any other.
echo ">> install ON THIS BOX with (both steps, in this order):"
echo "     sudo ./$PREFLIGHT_OUT $OUT"
echo "     sudo apt install --no-install-recommends ./$OUT"
echo ">>   (never 'dpkg -i' — it does not resolve dependencies)"
echo ">> checksums: $OUT.sha256"
echo ">> to install on ANOTHER appliance, move the one bundle and follow the"
echo "   INSTALL.txt inside it (it carries the .deb, its guard and SHA256SUMS):"
echo "     $BUNDLE_OUT"
echo "     scp $BUNDLE_OUT <user>@<host>:~/  &&  tar xzf ${BUNDLE}.tar.gz  &&  cd $BUNDLE"
