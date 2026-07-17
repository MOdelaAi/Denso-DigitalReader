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
bad_map=0
while read -r libname sover dep _rest; do
    case "${libname-}" in ''|'#'*) continue ;; esac
    dep="${dep%%(*}"; dep="${dep// /}"     # strip any version constraint
    lib="$(ldconfig -p | awk -v s="$libname.so.$sover" '$1==s {print $NF; exit}')"
    [ -n "$lib" ] || lib="$(ldd "$EXE" | awk -v s="$libname.so.$sover" '$1==s {print $3; exit}')"
    if [ -z "$lib" ] || [ ! -e "$lib" ]; then
        echo "   BAD  $libname.so.$sover — not resolvable on this box" >&2; bad_map=1; continue
    fi
    owner="$(dpkg-query -S "$(readlink -f "$lib")" 2>/dev/null | cut -d: -f1 | head -1)"
    if [ "$owner" != "$dep" ]; then
        echo "   BAD  $libname.so.$sover -> mapped to '$dep' but dpkg says '${owner:-<unowned>}'" >&2; bad_map=1; continue
    fi
    dpkg-query -W -f='${Status}' "$dep" 2>/dev/null | grep -q "install ok installed" || {
        echo "   BAD  mapped package '$dep' is not installed" >&2; bad_map=1; continue; }
    echo "   ok   $libname.so.$sover -> $dep"
done < packaging/debian/shlibs.local
[ "$bad_map" = "0" ] || { echo "shlibs.local has wrong mappings — fix them; shlibdeps trusts this file blindly" >&2; exit 1; }

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
mkdir -p "$SHLIBDIR/debian/denso-digitalreader"
cp "$EXE" "$SHLIBDIR/debian/denso-digitalreader/denso"
# NO --ignore-missing-info: if this errors, a dependency is genuinely
# underivable and shlibs.local needs a line — do not paper over it.
SHLIBS_LINE="$( cd "$SHLIBDIR" && dpkg-shlibdeps -O debian/denso-digitalreader/denso )" || {
    echo "dpkg-shlibdeps FAILED — add the missing SONAME to packaging/debian/shlibs.local" >&2
    rm -rf "$SHLIBDIR"; exit 1; }
rm -rf "$SHLIBDIR"
SHLIBS_DEPENDS="${SHLIBS_LINE#shlibs:Depends=}"
[ -n "$SHLIBS_DEPENDS" ] || { echo "shlibdeps produced an EMPTY Depends — refusing" >&2; exit 1; }
echo "   derived: $SHLIBS_DEPENDS"

sed -e "s/@VERSION@/$VERSION/" -e "s/@ARCH@/$ARCH/"     -e "s|@SHLIBS_DEPENDS@|$SHLIBS_DEPENDS|"     packaging/debian/control.in > "$STAGE/DEBIAN/control"

# md5sums: `dpkg-deb --build` does NOT generate these, and without them
# `dpkg -V` verifies nothing — the payload-integrity claim would be empty.
( cd "$STAGE" && find . -type f ! -path './DEBIAN/*' -printf '%P\0'     | xargs -0 md5sum > DEBIAN/md5sums )

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

# ── PREFLIGHT GUARD, standalone: `denso-setup preflight` cannot protect a
# FIRST install, because denso-setup does not exist on the box until the .deb
# containing it is already installed. So we ALSO emit a standalone guard next
# to the .deb that works with nothing but this .deb and a shell — generated by
# the one shared emitter in packaging/lib/gen_preflight.sh (see there for why).
PREFLIGHT_OUT="preflight-denso.sh"
emit_preflight_script "packaging/lib/policy.sh" "$PREFLIGHT_OUT"

echo
echo ">> built $OUT"
dpkg-deb --info "$OUT" | sed -n '1,12p'
echo ">> install with:  sudo apt install ./$OUT     (NEVER dpkg -i — it does not resolve deps)"
echo ">> or verify first with the standalone guard:  sudo ./$PREFLIGHT_OUT $OUT"
