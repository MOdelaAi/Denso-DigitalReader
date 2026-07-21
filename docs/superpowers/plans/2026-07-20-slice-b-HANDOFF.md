# Slice (b) — HANDOFF

Written 2026-07-20. Read this first if you are picking up slice (b) in a new session
or on a different account.

**Branch:** `feature/zone-readiness-inhibition` (13 commits ahead of `main`, HEAD `688c126`)
**Suite:** `ctest -N` total = **467** on Windows. A run gives 466 pass + 1 "Failed".
**`main` is untouched and green.** Nothing here is merged or deployed.

> **The 1 "Failed" is expected and pre-existing.** `run_migrate rejects a sidecar
> symlinked outside models_dir` is a Catch2 **SKIP** (Windows has no symlink
> privilege) and ctest renders a SKIP as `Failed`. It passes for real on the Jetson.
> Do not "fix" it.

---

## 1. Read these, in this order

1. `docs/superpowers/specs/2026-07-20-zone-readiness-inhibition-design.md` — the spec.
   §3.1.1, §5.2, §5.3.1 and §10 are the parts that carry non-obvious decisions.
2. `docs/superpowers/plans/2026-07-20-zone-readiness-inhibition.md` — the plan, 10 tasks.
3. `.superpowers/sdd/progress.md` — the running ledger, **git-ignored**, so it exists
   only on the original machine. Everything load-bearing from it is reproduced below.

## 2. Build and test — non-obvious, gets people every time

```bash
export PATH=/c/msys64/ucrt64/bin:$PATH   # MSYS2 UCRT64, every shell
cmake --build build                       # build/ already exists (Ninja)
./build/tests/denso_tests "[zone_aggregator]"   # tag-scoped run
ctest --test-dir build                    # full suite
```

**`ctest -R <tag>` does not work.** Catch2 registers tests by *name*, not tag, so
`ctest -R zone_aggregator` prints `No tests were found!!!` — which reads as success
and will mislead you. Always use the binary directly for tag subsets.

Compare deltas against `ctest -N` **totals**, never a remembered pass count.

Jetson: `ssh modela@192.168.1.15`, repo at `~/project/Denso-DigitalReader`,
passwordless. No `ninja` (`make -j4`), no `sqlite3` CLI (`python3 -c 'import sqlite3'`).

## 3. Status

| Task | State |
|---|---|
| 1 assembly sum type + gap guard | done, reviewed, fixed |
| 2 soft hold | done, reviewed, fixed |
| 3 evict + empty-snapshot rule | done, reviewed, fixed |
| 4 hold timeout + cold start | done, reviewed, fixed ×2 |
| 6 integrity verdict | done, **review findings open** |
| 7 status.json | done, **review findings open** |
| 8 zone health cause-set | done, **review findings open** |
| **5 + 9 integration** | **NOT STARTED — do these next, atomically** |
| 10 Jetson gate | not started |

Tasks 5 and 9 **must land together**. Task 5 changes the `on_snapshot` callback to
carry a sequence number; Task 9 updates its only caller. Between them the app target
does not compile. Briefs are pre-generated at `.superpowers/sdd/task-{5,9}-brief.md`
(regenerate with the `subagent-driven-development` skill's `scripts/task-brief` if absent).

## 4. OPEN REVIEW FINDINGS — fix before the integration

From an adversarial review of `src/core/health` (Tasks 6/7/8). The brick guard,
read-only contract, query-failed/no-rows distinction, atomicity and ZoneHealth
transition semantics all **passed**. These remain:

1. **Speculative enum values (binding-constraint violation).** `GlobalBlocker::Kind`
   declares `DbUnopenable`, `SchemaNewer`, `MigrationFailed`, `SharedBackendFailure`
   with **no producer**. The project rule is that only kinds with a real producer may
   be declared. Either drop them, or have Task 9 produce them (the boot path *does*
   `Db::open` + `run_migrations` before calling the verdict, so the first three get
   real producers there — that is the better fix). `SharedBackendFailure` has no
   near-term producer; drop it.
   *This is a defect in the plan, not in the implementation.*
2. **Test gaps.** No deliberate DB-query-failure test asserting `DbQueryFailed`. The
   `status.json` residue test greps `*.tmp`, but `QSaveFile` makes no promise about
   that suffix, so it could pass with residue under another name. No failed-open /
   failed-commit / old-target-preserved test. No valid-but-empty-manifest test.
3. **`ZoneHealth::set_cause` inserts a phantom entry** via `operator[]` when clearing
   a cause for a camera never seen, so `all()` can report retired cameras and the map
   grows if ids churn. Minor; fix while you are in there.

**Adjudicated, do NOT "fix":** Codex asked for `held_zones`/`inhibited_zones` to be
emitted as strings. Zone numbers are `int` (1..12) and cannot approach 2^53 — the
decimal-string rule exists for 64-bit **camera/model ids**, which *are* strings.
Leave zone numbers as JSON numbers.

## 5. Decisions that look wrong but are deliberate

Change any of these only with the owner's agreement.

- **§3.1.1 asymmetry (the subtlest thing here).** A **camera-level** cause drops the
  *whole observation*; a **zone-level hold-timeout** suppresses *publication only* and
  observations keep rebuilding debounce. Collapsing them into one gate reintroduces a
  **permanent-inhibit deadlock** — the zone could never accumulate the readings that
  clear its own flag. A spec review caught this before implementation; a Task 4
  implementer re-proved it by temporarily reintroducing the deadlock and watching the
  test fail.
- **Expiry runs BEFORE the hold timeout, and `newly_inhibited_` survives expiry.**
  A zone that stopped arriving is *absent*, not *held* — escalating it would alarm on
  a stale baseline and strand state. Coverage is not lost: `group_into_zones` emits a
  reading for **every** zoned area on **every** inference round, so zone silence
  implies *camera* silence, which the camera-level causes handle. `newly_inhibited_`
  is an **event set**, not state, so an already-raised escalation is still delivered
  even if the zone then expired. Both pinned by tests.
- **Three `KNOWN LIMITATION` tests (`[known_limit]`)** assert the gap guard does *not*
  fire for `123`→`23`, `123`→`12`, `13`→`3`. They are not correctness expectations —
  they pin accepted limitations so nobody assumes those cases are solved. Slice (b2)
  closes them; when it does, these flip to `Incomplete` and should be rewritten.
- **`median_h <= 0` skips the gap check** (fail-open). A false gap freezes a healthy
  zone, which is worse than the bug being mitigated.
- **The gap guard's strict `>` boundary is deliberately untested.** With integer
  `cv::Rect` coordinates and a non-representable 44.8 threshold, exact equality is
  unreachable; a float-equality test would be fragile theatre.
- **`sync_models()`'s directory scan is RETAINED.** The production Jetson has engines
  and **no `manifest.json`** — verified 2026-07-20. Making manifest authority
  mandatory would globally block the machine currently running production. Unmanifested
  engines are reported `Degraded`, never `Blocked`. There is a test guarding this;
  treat it as a safety gate.
- **`ZoneHealth` has no mutex on purpose.** Every cause transition happens on the GUI
  thread. Task 9 must marshal the inference-worker source through
  `common::post_to_gui`, and must connect `CameraStream::status_changed` to a
  **GUI-affine receiver with an auto/queued connection** — that signal is emitted on
  the capture thread, so a contextless functor or `Qt::DirectConnection` silently
  breaks the single-owner property the no-mutex design depends on.

## 6. What slice (b) does NOT do

- It does **not** make the backend fault-aware. The backend is numeric-only; local
  alarm (log, `status.json`, UI) is the only fault channel. **Never describe this as
  per-zone fail-closed.**
- The gap guard catches **only an internal missing digit**. Missing leading, missing
  trailing, and single-remaining-detection losses still POST as valid shorter numbers.
  Slice (b2)'s calibrated anchor/slot work closes them.
- Whether omitting a zone means *retain* or *clear* on the backend is still
  **unverified**. Slice (c) qualifies it with a contract test.

## 7. Suggested next moves

1. Fix the three open findings in §4 (one fix wave, not three).
2. Do Tasks 5 + 9 as one atomic batch; run the **full** suite before committing —
   the app target will not build partway through.
3. Review 5+9 together (they are one logical unit).
4. Task 10 on the Jetson. Its sharpest assertion: `denso --check` must exit **10**
   (degraded, unmanifested engines). **Exit 78 there is a FAILURE** — it means the
   appliance is blocked, the exact outcome the compatibility decision exists to prevent.
5. Then `superpowers:finishing-a-development-branch`.

The repo convention is to have Codex review each completed logical unit — every task
above was reviewed that way, and it caught real defects at every single one.

## 8. Verification — 2026-07-21 (push checkpoint)

Branch pushed to `origin/feature/zone-readiness-inhibition` @ `2cc0b23` as a checkpoint
(NOT merged). Windows suite 485 pass + 1 known symlink SKIP; native Jetson suite 485/485;
Codex APPROVE on the integration.

**GUI boot smoke — Jetson, offscreen QPA (real `denso` binary through main.cpp →
startup.cpp launch() → QApplication → window/event loop), isolated DENSO_DATA_DIR:**
- normal migrated v13 DB (no cameras) → boots and STAYS up (event loop alive @7s). PASS.
- future-schema DB (user_version=14) → exits **78**; `status.json` reason `schema_newer`;
  log records `BLOCKED schema_newer`. PASS. status.json matches the CLI classifier.
- corrupt DB (torn page 2) → exits **78**; `status.json` reason `db_unopenable`
  ("database failed integrity check (quick_check)"). PASS.

**RESIDUAL (human, via AnyDesk):** on-monitor visual confirmation was NOT done — the dev
Jetson sits at the GDM greeter (autologin unconfigured), and offscreen validates boot
LOGIC/exit/status.json, not physical pixels. Also the production-DB-on-the-real-display
boot + restore is a human step (autonomous SSH boot would conflict with the live
single-instance + trigger network reassert).

## 9. OPEN FINDINGS discovered during verification — resolve BEFORE merge

1. **`--check` exit-code contract is contradictory (blocking).** `exit_code_for` defines
   Degraded=10, and §7.4 above says the Jetson `--check` "must exit 10 (degraded,
   unmanifested engines)." BUT the implemented `run_check` returns only 0/1/78 (no 10 —
   evaluate_integrity was scoped out of --check), AND `packaging/denso-setup:319` runs
   `if "$DENSO" --check; then ok; else FAIL`, i.e. treats non-zero (incl. 10) as failure.
   So exit 10 from --check would ABORT denso-setup verify on the unmanifested production
   appliance — the opposite of the compatibility guarantee. Exit 10 is currently dead
   (boot runs on Degraded rather than exiting; --check is its only caller). Decision
   needed: (A/recommended) --check returns 0 for Ready+Degraded (serviceable), 78 only
   for Blocked, 1 for hard errors, and REPORTS degraded issues on stdout — then correct
   §7.4's "must exit 10"; or (B) --check returns 10 and denso-setup is updated to accept
   0 and 10 (a Slice-2 packaging change).
2. **No visible on-screen error on a Blocked boot (UX).** The Blocked boot path logs +
   writes status.json + exits with NO GUI dialog/screen, so the operator at the kiosk
   sees a blank desktop, not an error. The §3-GUI-smoke criterion "the visible startup
   error agrees with the CLI classifier" has no on-screen error to compare. Decision:
   accept (headless/remote-managed; status.json + log are the channel, spec §7) or add a
   brief startup error screen (scope add).
