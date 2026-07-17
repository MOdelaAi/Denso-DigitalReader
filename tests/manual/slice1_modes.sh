#!/usr/bin/env bash
# Slice 1 behavioral gate. Run from the repo root after a build.
set -u
EXE=build/src/app/denso.exe
[ -f "$EXE" ] || EXE=build/src/app/denso
fail=0
chk() { # chk <description> <expected-rc> <actual-rc>
  if [ "$2" = "$3" ]; then echo "ok   - $1"; else echo "FAIL - $1 (want rc=$2, got rc=$3)"; fail=1; fi
}
# BEFORE the CLI dispatch exists, every one of these flags falls through and
# opens the GUI, which blocks forever. Bound each run so the red test FAILS fast
# instead of hanging the worker's terminal (timeout returns 124).
run() { timeout 20 "$EXE" "$@" >/dev/null 2>&1; }

TMP=$(mktemp -d)
export DENSO_DATA_DIR="$TMP"
# Keep backend/tmp caches inside the sandbox so a stray write can't land
# somewhere the assertions below don't look.
export TMPDIR="$TMP/tmp"; mkdir -p "$TMPDIR"
export CUDA_CACHE_PATH="$TMP/cuda-cache"

run --version;            chk "--version exits 0" 0 $?
run --wat;                chk "unknown flag exits 2" 2 $?
run --check-migrations;   chk "--check-migrations without path exits 2" 2 $?
run --check-running;      chk "--check-running with nothing running exits 1" 1 $?
[ -f "$TMP/denso.lock" ] && { echo "FAIL - --check-running left a lock corpse"; fail=1; } || echo "ok   - --check-running left no lock"

# --version must not create ANY state in the data dir.
if [ -z "$(ls -A "$TMP")" ]; then echo "ok   - --version left the data dir empty"; else echo "FAIL - --version created: $(ls -A "$TMP")"; fail=1; fi

# --check-migrations builds the chain in a throwaway db and touches nothing else.
run --check-migrations "$TMP/copy.db"; chk "--check-migrations exits 0" 0 $?
[ -f "$TMP/copy.db" ] && echo "ok   - migration ran against the given path" || { echo "FAIL - copy.db absent"; fail=1; }
for artifact in denso.db denso.log denso.lock models; do
  [ -e "$TMP/$artifact" ] && { echo "FAIL - --check-migrations created $artifact"; fail=1; } \
                          || echo "ok   - --check-migrations created no $artifact"
done

rm -rf "$TMP"
exit $fail
