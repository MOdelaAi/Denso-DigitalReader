# Build-time model-payload staging for the Denso .deb — a SOURCEABLE emitter.
#
# Same split, same reason as packaging/lib/gen_preflight.sh and gen_bundle.sh:
# tools/build_package.sh hard-refuses to run off an aarch64 JetPack box, so any
# logic inlined there is untestable on the dev box. The model/manifest staging is
# the part Slice 5 must prove — mode of every payload file, the manifest declared
# from the packaged bytes, the SHA256SUMS, and the "no Float artifact anywhere"
# guarantee — so it lives here where tests/packaging/run.sh can drive it with
# fixtures and inspect the resulting staging tree, with no .deb build at all.
#
# REQUIRES packaging/lib/policy.sh already sourced (for manifest_matches_models_dir).
# build_package.sh and run.sh both source policy.sh first.
#
# POSIX sh. Every function body is a SUBSHELL ( ... ), not { ... }: POSIX shell
# variables are global, and build_package.sh sources this into the same shell it
# uses $m/$stem/$STAGE in — a brace body would clobber those. See policy.sh.

# --- cpp_symbol_present <def|use> <symbol> <file> ----------------------------
# Textual proof that a C++ symbol is really DEFINED (or USED), not merely
# mentioned in a comment, a string, or a declaration. Comments and string/char
# literals are stripped first, so a `// loadable_model_files` line or a log
# message can never satisfy it; a definition additionally requires the
# `symbol(params) {` shape (optionally a trailing-return-type), which a bare
# `symbol(params);` declaration does not have.
#
# This is a HEURISTIC gate, deliberately conservative, and it is NOT a C++
# parser. It strips comments, string/char literals and `#if 0`/`#if false`
# blocks, then:
#   def : requires the `symbol(params) {` shape — a bare `symbol(params);`
#         declaration (no body) does NOT satisfy it.
#   use : requires a genuine CALL — an occurrence that is neither a definition
#         nor a type-led `;`-terminated declaration. A forward declaration or an
#         uncalled dummy definition placed in the file does NOT satisfy it.
# It still cannot see through raw-string literals or macro-built signatures, and
# textual matching cannot PROVE a symbol is compiled and reached at run time.
# Its ONLY job is to keep the Slice-5 ordering assertion honest until Slice 7
# lands; the AUTHORITATIVE proof that the warm-up allow-list is wired into
# startup is Slice 7's C++ tests, not this gate. See assert_float_seeding_guarded.
#   exit 0 = present, exit 1 = absent (or file missing)
cpp_symbol_present() (
    mode="${1-}"; sym="${2-}"; file="${3-}"
    [ -f "$file" ] || return 1
    python3 - "$mode" "$sym" "$file" <<'PY'
import re, sys
mode, sym, path = sys.argv[1], sys.argv[2], sys.argv[3]
try:
    src = open(path, "r", encoding="utf-8", errors="replace").read()
except OSError:
    sys.exit(1)
# Drop `#if 0` / `#if false` regions, keeping the `#else` branch if present.
src = re.sub(r"#\s*if\s+(?:0|false)\b.*?(?:#\s*else\b(.*?))?#\s*endif",
             lambda m: m.group(1) or " ", src, flags=re.S)
src = re.sub(r"/\*.*?\*/", " ", src, flags=re.S)   # block comments
src = re.sub(r"//[^\n]*", " ", src)                 # line comments
src = re.sub(r'"(?:\\.|[^"\\])*"', '""', src)       # string literals
src = re.sub(r"'(?:\\.|[^'\\])*'", "''", src)       # char literals
s = re.escape(sym)
# A definition: symbol '(' params ')' optional-trailing-return '{' (no ';'/'{' in params).
defn = re.compile(r"\b" + s + r"\s*\([^;{]*\)\s*(?:->[^;{]*)?\{")
if mode == "def":
    sys.exit(0 if defn.search(src) else 1)
# mode == "use": remove definitions and type-led ;-terminated declarations; any
# remaining occurrence is an expression-context CALL. A bare `symbol(a);` call
# statement survives (no type token precedes the symbol); a `Type symbol(a);`
# declaration and a `symbol(...) { }` definition are removed.
without = defn.sub(" ", src)
without = re.sub(r"(?:(?<=[;{}])|^)\s*[A-Za-z_][\w:<>,&*\s]*\b" + s + r"\s*\([^;{]*\)\s*;",
                 " ", without, flags=re.M)
sys.exit(0 if re.search(r"\b" + s + r"\s*\(", without) else 1)
PY
)

# --- assert_float_seeding_guarded <models.approved> <repo-root> --------------
# The ordering assertion (spec 8.7.2). Release A ships no Float artifact, and no
# Float stem may be approved for SEEDING before the Slice-7 warm-up firewall
# exists — otherwise a Float engine placed in the models directory would be
# deserialized and run on a Digital Number Reader appliance at every boot
# (EngineRegistry::warm_up scans the whole directory, attachment or not).
#
# So: if any `float-*` stem is approved in models.approved, REFUSE the build
# unless BOTH the compiled allow-list symbol exists — a real definition of
# `loadable_model_files` in src/core/models/compatibility.cpp — AND it is used
# from src/app/ui/startup.cpp. A comment or a declaration does not satisfy it.
# In Release A those files do not exist and no Float stem is approved, so this
# passes trivially; it goes live before a Float line could ever be added.
#   exit 0 = ok (no Float approved, or Float approved AND guarded)
#   exit 1 = a Float stem is approved but the Slice-7 symbol is absent
assert_float_seeding_guarded() (
    approved="${1-}"; root="${2-}"
    [ -f "$approved" ] || { echo "gen_payload: no such models.approved: $approved" >&2; return 1; }
    floats="$(awk 'NF && $1 !~ /^#/ && $1 ~ /^float-/ { print $1 }' "$approved")"
    [ -n "$floats" ] || return 0
    comp="$root/src/core/models/compatibility.cpp"
    start="$root/src/app/ui/startup.cpp"
    if cpp_symbol_present def loadable_model_files "$comp" \
       && cpp_symbol_present use loadable_model_files "$start"; then
        return 0
    fi
    echo "gen_payload: REFUSED — a Float model is approved for seeding but the" >&2
    echo "  Slice-7 warm-up allow-list is absent. A Float engine in the models" >&2
    echo "  directory would be deserialized on a Digital Number Reader appliance." >&2
    echo "  Require a real definition of loadable_model_files in" >&2
    echo "    src/core/models/compatibility.cpp" >&2
    echo "  AND its use in src/app/ui/startup.cpp (a comment/declaration is not enough)." >&2
    echo "  approved Float stem(s): $floats" >&2
    return 1
)

# --- stage_model_payload <stage-root> <repo-root> <models.approved>
#                         <descriptor-dir> <expected-manifest-sha256|->
#                         <engine-path> [<engine-path> ...] --------------------
# Stage every model pair, the generated schema-2 manifest, models.approved and a
# SHA256SUMS into <stage-root>/opt/denso, with every payload mode pinned 0644
# (never a `>`-inherited umask). Emits NOTHING else. Reproducible: the generator
# reads no clock, so identical inputs give identical bytes.
#
# Ordered so a run that will fail fails BEFORE it stages anything:
#   1. ordering assertion over models.approved;
#   2. every engine has a sidecar, is approved, and has a descriptor;
#   3. stage engine+sidecar pairs (0644);
#   4. generate the manifest from the STAGED bytes via a descriptor whose
#      engine/sidecar paths are injected with Python (never a text substitution),
#      to a scratch dir OUTSIDE opt/denso and models/ (ensure_safe_output_dir);
#   5. assert the manifest declares EXACTLY the staged pairs, hashes and all
#      (manifest_matches_models_dir) and, if given, hashes to the reviewed
#      candidate;
#   6. install manifest.json + models.approved into opt/denso/lib (0644);
#   7. emit opt/denso/lib/SHA256SUMS (0644) over the model payload.
# Returns non-zero on any failure, having staged nothing durable past the point
# of failure that a later step would trust.
stage_model_payload() (
    stage="${1-}"; root="${2-}"; approved="${3-}"; descdir="${4-}"; want_sha="${5-}"
    shift 5 || return 1
    [ "$#" -gt 0 ] || { echo "stage_model_payload: at least one engine path required" >&2; return 1; }
    [ -d "$stage" ] && [ -d "$root" ] && [ -f "$approved" ] && [ -d "$descdir" ] \
        || { echo "stage_model_payload: bad arguments" >&2; return 1; }

    models_out="$stage/opt/denso/models"
    lib_out="$stage/opt/denso/lib"
    install -d "$models_out" "$lib_out" || return 1

    # 1. ordering assertion — before any Float line could take effect.
    assert_float_seeding_guarded "$approved" "$root" || return 1

    # 2. validate every requested model, and collect descriptors.
    gen_args=""
    scratch="$(mktemp -d)" || return 1
    # shellcheck disable=SC2064
    trap "rm -rf \"$scratch\"" EXIT

    for eng in "$@"; do
        [ -f "$eng" ] || { echo "stage_model_payload: no such engine: $eng" >&2; return 1; }
        stem="$(basename "$eng" .engine)"
        side="$(dirname "$eng")/$stem.names.json"
        [ -f "$side" ] || { echo "stage_model_payload: $stem has no sidecar $side" >&2; return 1; }
        # Must be approved (the same rule build_package's hash check enforces —
        # here we only need the row to exist, so a stem with no approval can
        # never be staged into a manifest).
        awk -v s="$stem" 'NF && $1 !~ /^#/ && $1==s {found=1} END{exit !found}' "$approved" \
            || { echo "stage_model_payload: $stem is not in models.approved" >&2; return 1; }
        desc="$descdir/$stem.descriptor.json"
        [ -f "$desc" ] || { echo "stage_model_payload: $stem has no descriptor $desc" >&2; return 1; }

        # 3. stage the pair (0644, pinned).
        install -m 0644 "$eng"  "$models_out/$stem.engine"     || return 1
        install -m 0644 "$side" "$models_out/$stem.names.json" || return 1

        # 4a. materialise a concrete descriptor pointing at the STAGED bytes.
        conc="$scratch/$stem.descriptor.json"
        python3 - "$desc" "$conc" "$models_out/$stem.engine" "$models_out/$stem.names.json" <<'PY' || return 1
import json, sys
src, dst, eng, side = sys.argv[1:5]
d = json.load(open(src, encoding="utf-8"))
trt = d.get("tensorrt")
if not isinstance(trt, dict):
    sys.stderr.write("descriptor %s has no tensorrt block\n" % src); sys.exit(1)
trt["engine_path"] = eng
trt["sidecar_path"] = side
json.dump(d, open(dst, "w", encoding="utf-8"), indent=2)
PY
        gen_args="$gen_args --generation $conc"
    done

    # 4b. generate the manifest to the scratch dir (never under opt/denso/models).
    gen_manifest="$scratch/manifest.json"
    # shellcheck disable=SC2086
    PYTHONDONTWRITEBYTECODE=1 python3 "$root/tools/gen_model_manifest.py" \
        $gen_args --output "$gen_manifest" --overwrite \
        > "$scratch/gen.log" 2>&1 \
        || { echo "stage_model_payload: manifest generation failed:" >&2; cat "$scratch/gen.log" >&2; return 1; }

    # 5. the manifest must declare EXACTLY the staged pairs (set + hashes).
    manifest_matches_models_dir "$gen_manifest" "$models_out" \
        || { echo "stage_model_payload: generated manifest does not match the staged models" >&2; return 1; }
    # ...and, when asked, be the reviewed candidate byte-for-byte.
    if [ -n "$want_sha" ] && [ "$want_sha" != "-" ]; then
        got_sha="$(sha256sum "$gen_manifest" | cut -d' ' -f1)"
        [ "$got_sha" = "$want_sha" ] || {
            echo "stage_model_payload: generated manifest sha256 $got_sha != expected $want_sha" >&2
            echo "  (the descriptor or the model bytes changed; re-review the candidate)" >&2
            return 1; }
    fi

    # 6. install manifest.json + models.approved (0644, pinned).
    install -m 0644 "$gen_manifest" "$lib_out/manifest.json" || return 1
    install -m 0644 "$approved"     "$lib_out/models.approved" || return 1

    # 7. SHA256SUMS over the model payload, relative to /opt/denso so
    #    `sha256sum -c` works from there. Sorted + LC_ALL=C for reproducibility;
    #    excludes itself (DEBIAN/md5sums covers it). Written via install -m after
    #    a staged temp so its own mode is pinned, not umask-inherited.
    sums="$scratch/SHA256SUMS"
    ( cd "$stage/opt/denso" && LC_ALL=C find lib models -type f \
        ! -name SHA256SUMS -printf '%p\n' | LC_ALL=C sort | while IFS= read -r rel; do
            printf '%s  %s\n' "$(sha256sum "$rel" | cut -d' ' -f1)" "$rel"
        done ) > "$sums" || return 1
    install -m 0644 "$sums" "$lib_out/SHA256SUMS" || return 1

    return 0
)
