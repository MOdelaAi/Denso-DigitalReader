#!/usr/bin/env bash
# Build the Denso .deb. MUST run on an aarch64 JetPack 6.2 Jetson — there is no
# cross-toolchain, and the engines are sm_87/TRT-10.3 pinned.
#
#   tools/build_package.sh --model models/digitv2.engine [--allow-dirty]
set -euo pipefail

HERE="$(cd "$(dirname "$0")/.." && pwd)"
. "$HERE/packaging/lib/policy.sh"
# The standalone preflight-denso.sh emitted below is the same generator the
# test harness calls, so the two never drift apart — see gen_preflight.sh.
. "$HERE/packaging/lib/gen_preflight.sh"

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
VERSION="${APP_VERSION}+g${SHA}${DIRTY}"
version_ok "$VERSION" || { echo "computed version is not a safe path component: $VERSION" >&2; exit 1; }
echo ">> version: $VERSION"

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

    lib="$(ldconfig -p | awk -v s="$soname" '$1==s {print $NF; exit}')"
    [ -n "$lib" ] || lib="$(ldd "$EXE" | awk -v s="$soname" '$1==s {print $3; exit}')"
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

# ── MANIFEST: what this artifact IS, for after-the-fact diagnosis. Written
# BEFORE md5sums so md5sums covers it too (see below).
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
        echo "model-recipe: $stem $(awk -v s="$stem" '$1==s {$1="";$2="";$3="";print}' packaging/models.approved | sed 's/^  *//')"
    done
    echo "--- ldd report (diagnostic evidence, not a dependency source) ---"
    ldd "$EXE"
} > "$STAGE/opt/denso/MANIFEST"

# md5sums: MUST be the LAST payload write before dpkg-deb --build. Generating
# it any earlier (e.g. before MANIFEST is staged) means `dpkg -V` never
# verifies MANIFEST at all — the file that documents what this artifact IS
# would be the one thing integrity-checking silently skips. `dpkg-deb --build`
# itself does not generate md5sums, and without them `dpkg -V` verifies
# nothing.
( cd "$STAGE" && find . -type f ! -path './DEBIAN/*' -printf '%P\0'     | xargs -0 md5sum > DEBIAN/md5sums )

# ── build the .deb
OUT="denso-digitalreader_${VERSION}_${ARCH}.deb"
dpkg-deb --build --root-owner-group "$STAGE" "$OUT" >/dev/null
sha256sum "$OUT" > "$OUT.sha256"
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
PREFLIGHT_OUT="preflight-denso-${VERSION}.sh"
emit_preflight_script "packaging/lib/policy.sh" "$PREFLIGHT_OUT" "$DEB_SHA256"

echo
echo ">> built $OUT"
dpkg-deb --info "$OUT" | sed -n '1,12p'
# Print ONLY the guarded sequence. Advertising a bare `apt install` first --
# with preflight as an optional extra -- invites the operator to skip the
# protected-stack guard entirely, which is the same as not having one. The
# preflight is bound to THIS .deb by its SHA-256 and refuses any other.
echo ">> install with (both steps, in this order):"
echo "     sudo ./$PREFLIGHT_OUT $OUT"
echo "     sudo apt install --no-install-recommends ./$OUT"
echo ">>   (never 'dpkg -i' — it does not resolve dependencies)"
echo ">> checksums: $OUT.sha256"
