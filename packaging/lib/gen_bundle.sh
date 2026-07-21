# Emits the single-file transport bundle: <name>.tar.gz holding the .deb, its
# preflight guard, a bundle-local SHA256SUMS and an INSTALL.txt.
#
# WHY a bundle at all: the .deb and the guard that vets it are USELESS APART.
# The guard refuses any .deb but the one it was generated for (an embedded
# SHA-256 — see gen_preflight.sh), so an operator who scp's only the .deb has
# no way to install it through the protected path, and one who scp's a stale
# guard beside a fresh .deb gets a hard refusal. Shipping them as one file
# makes "they travel together" a property of the artifact instead of a step in
# a runbook.
#
# WHY here and not inline in tools/build_package.sh: that script hard-refuses
# to run anywhere but an aarch64 JetPack 6.2 Jetson (uname/L4T/TensorRT
# contract), so anything living in it cannot be tested on the dev box. This is
# the same split — and the same reason — as gen_preflight.sh: ONE sourceable
# emitter, called by the real build AND by tests/packaging/run.sh.
#
# POSIX sh, sourceable by build_package.sh (bash) and run.sh (bash) alike.
#
#   emit_bundle <deb-path> <preflight-path> <bundle-name> <output.tar.gz> <source-date-epoch>
#
# <bundle-name> is BOTH the archive's single top-level directory and the stem
# an operator will `cd` into. It must already be a safe path component — the
# caller validates it (build_package.sh runs version_ok on the version it is
# built from); this function re-checks the cheap structural part rather than
# trusting that.
#
# A subshell body ( ... ), not { ... }: POSIX shell variables are global by
# default, so a brace-bodied function's locals would clobber a caller's
# variables of the same name — the defect already fixed across every function
# in policy.sh, and build_package.sh sources all three files into one shell.
emit_bundle() (
    deb="$1"; pre="$2"; name="$3"; out="$4"; epoch="$5"

    [ -f "$deb" ] || { echo "emit_bundle: no such .deb: $deb" >&2; return 1; }
    [ -f "$pre" ] || { echo "emit_bundle: no such preflight: $pre" >&2; return 1; }
    # Required, not defaulted: silently falling back to "now" would make the
    # archive quietly non-reproducible again — the exact defect this closes.
    case "${epoch:-}" in
        ''|*[!0-9]*) echo "emit_bundle: SOURCE_DATE_EPOCH must be a positive integer, got '${epoch:-}'" >&2; return 1 ;;
    esac
    # The name becomes a directory inside an archive an operator unpacks with
    # elevated intent. A slash or a traversal here is an extraction escape.
    case "${name:-}" in
        "") echo "emit_bundle: empty bundle name" >&2; return 1 ;;
        -*|*/*|*..*) echo "emit_bundle: unsafe bundle name: $name" >&2; return 1 ;;
    esac

    deb_base="$(basename "$deb")"
    pre_base="$(basename "$pre")"

    # Stage under mktemp, NOT a predictable dist/<name>/: a fixed path can
    # collide with an operator-created directory or a concurrent build, and it
    # would mean an `rm -rf` on a constructed path. A temp dir is unguessable
    # and its removal names only the directory we just made.
    stage="$(mktemp -d)" || return 1
    root="$stage/$name"
    mkdir -p "$root" || { rm -rf "$stage"; return 1; }

    cp "$deb" "$root/$deb_base" || { rm -rf "$stage"; return 1; }
    cp "$pre" "$root/$pre_base" || { rm -rf "$stage"; return 1; }
    chmod 0644 "$root/$deb_base" || { rm -rf "$stage"; return 1; }
    # The guard is the FIRST thing the operator runs; it must arrive runnable.
    # tar preserves the mode, so this is the one place it can be set.
    chmod 0755 "$root/$pre_base" || { rm -rf "$stage"; return 1; }

    # SHA256SUMS is REGENERATED with bare filenames, never a copy of the
    # build's own <deb>.sha256 — that one records the path `dist/denso-...deb`,
    # so `sha256sum -c` from inside the unpacked bundle would hunt for a
    # `dist/` subdirectory that does not exist and report a failure for a
    # perfectly intact artifact. A checksum that cries wolf is worse than none:
    # it teaches the operator to skip the check.
    #
    # It covers the guard too, not just the .deb. The guard is an executable
    # this procedure tells someone to run as root; leaving it unlisted would
    # check the payload and not the thing with the privileges.
    ( cd "$root" && sha256sum "$deb_base" "$pre_base" > SHA256SUMS ) || { rm -rf "$stage"; return 1; }
    # Explicit 0644: a `>` redirect takes the BUILDER's umask, so on a box with
    # a group-writable umask (002 — the Jetson's default) this shipped 0664 and
    # extracted group-writable. The integrity list for a root-run installer is
    # the last file that should inherit an ambient umask.
    chmod 0644 "$root/SHA256SUMS" || { rm -rf "$stage"; return 1; }

    # GENERATED, not a static file: it names the exact artifacts in THIS
    # bundle. A hand-written INSTALL.txt with <version> placeholders is a file
    # operators copy-paste from and then edit by hand, which is precisely how
    # the wrong .deb gets paired with the wrong command.
    {
        echo "Denso DigitalReader — install on a Jetson (JetPack 6.2 / L4T R36.5.0)"
        echo
        echo "Run these from inside this directory, in this order:"
        echo
        echo "  sha256sum -c SHA256SUMS"
        echo "  sudo ./$pre_base ./$deb_base"
        echo "  sudo apt install --no-install-recommends ./$deb_base"
        echo "  sudo denso-setup configure --user <username>"
        echo "  sudo denso-setup verify                      # expect: verify: PASS"
        echo
        echo "NEVER 'dpkg -i' — it does not resolve dependencies."
        echo
        echo "The preflight guard is bound to THIS .deb by SHA-256 and refuses any"
        echo "other; it simulates the apt transaction and aborts if installing would"
        echo "touch a protected JetPack package (TensorRT, CUDA, nvidia-l4t-*, the"
        echo "NVIDIA OpenCV). It must run BEFORE apt install, because it is the only"
        echo "veto point that exists on a box where denso-setup is not installed yet."
        echo
        echo "'apt remove' keeps /opt/denso/data (database, engines); 'apt purge'"
        echo "removes it."
    } > "$root/INSTALL.txt" || { rm -rf "$stage"; return 1; }
    chmod 0644 "$root/INSTALL.txt" || { rm -rf "$stage"; return 1; }

    # BYTE-REPRODUCIBLE, given the same inputs and epoch. Four separate sources
    # of variance, all of which have to be closed or the guarantee is worthless:
    #   --sort=name    directory read order is filesystem-dependent
    #   --owner/group  otherwise the build user's name+uid land in the archive
    #   --mtime        otherwise every entry carries the staging time (the
    #                  files were literally just created by mktemp+cp)
    #   gzip -n        gzip writes a TIMESTAMP and the source filename into its
    #                  own header, so `tar -czf` is non-reproducible even when
    #                  every tar entry is pinned. This is the one that is easy
    #                  to miss: the tar stream is identical and the .gz differs.
    # Piping to an explicit `gzip -n` is why -cf is used here, not -czf.
    #
    # Write to a temp name in the SAME directory as $out and rename, so an
    # interrupted tar can never leave a truncated .tar.gz sitting at the path
    # an operator is about to scp.
    tmp="$(mktemp "$(dirname "$out")/.bundle.XXXXXX")" || { rm -rf "$stage"; return 1; }
    # No pipefail assumption: this is sourced into callers with and without it.
    # tar's status is captured explicitly so a tar failure can never be masked
    # by a successful gzip writing a valid archive of a truncated stream.
    tar_rc=0
    { tar --sort=name --owner=0 --group=0 --numeric-owner \
          --mtime="@$epoch" -cf - -C "$stage" "$name" || tar_rc=$?; } \
        | gzip -n > "$tmp"
    gzip_rc=$?
    if [ "$tar_rc" != "0" ] || [ "$gzip_rc" != "0" ]; then
        echo "emit_bundle: archive creation failed (tar=$tar_rc gzip=$gzip_rc)" >&2
        rm -f "$tmp"; rm -rf "$stage"; return 1
    fi
    chmod 0644 "$tmp" || { rm -f "$tmp"; rm -rf "$stage"; return 1; }
    mv -f "$tmp" "$out" || { rm -f "$tmp"; rm -rf "$stage"; return 1; }
    rm -rf "$stage"
    return 0
)
