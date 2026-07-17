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

# The data dir must be its OWN directory, with the tmp/cuda sandboxes as
# SIBLINGS -- not children. An earlier version put TMPDIR inside the data dir and
# mkdir'd it, so the "data dir is empty" assertions could never pass for any
# implementation. Keep the caches redirected (so a stray backend write can't land
# somewhere unasserted) but out of the directory under test.
SANDBOX=$(mktemp -d)
TMP="$SANDBOX/data"; mkdir -p "$TMP"
export DENSO_DATA_DIR="$TMP"
export TMPDIR="$SANDBOX/tmp"; mkdir -p "$TMPDIR"
export CUDA_CACHE_PATH="$SANDBOX/cuda-cache"

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

# --check on an empty data dir: no DB and no engines is a VALID fresh install (a
# fresh DB references no cameras, so it requires no engines).
CHK=$(mktemp -d); export DENSO_DATA_DIR="$CHK"
run --check; chk "--check on a fresh data dir exits 0" 0 $?

# Persistent-mutation INVENTORY, not a list of four guesses: after --check the
# data dir must be byte-for-byte empty. Anything at all (denso.db, denso.log,
# denso.lock, models/, trt_cache, a -wal/-shm, a leftover probe file) is a
# contract violation, including artifacts we didn't think to name.
LEFT=$(ls -A "$CHK")
if [ -z "$LEFT" ]; then echo "ok   - --check left the data dir empty"
else echo "FAIL - --check created: $LEFT"; fail=1; fi

# A named package engine that isn't there must FAIL, even with no database --
# this is the fresh-install case where configured-only checking would pass a
# corrupt/absent packaged engine.
run --check --engine absent.engine; chk "--check --engine <missing> exits 1" 1 $?

# A present-but-corrupt database must FAIL, not read as "fresh".
BAD=$(mktemp -d); export DENSO_DATA_DIR="$BAD"
printf 'this is not a sqlite file' > "$BAD/denso.db"
run --check; chk "--check on a corrupt database exits 1" 1 $?
rm -rf "$BAD"

export DENSO_DATA_DIR="$TMP"
rm -rf "$CHK"

# NOTE: the unwritable-data-dir case is NOT tested here. `chmod 500` does not
# reliably make a directory unwritable to a Windows process under MSYS2, so the
# test would fail despite correct behavior. It is covered on the Jetson below.

rm -rf "$SANDBOX"
exit $fail
