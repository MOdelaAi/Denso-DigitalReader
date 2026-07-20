# Model Lifecycle & Per-Zone Fail-Closed Reporting on the 24/7 Appliance

**Date:** 2026-07-20
**Status:** Draft (design) — Claude + Codex debate synthesized (4 rounds)
**Scope:** Denso-DigitalReader, Jetson Orin Nano appliance (JetPack 6.2, TRT 10.3, sm_87)

---

## 1. Problem & motivation

The reader runs **end-to-end 24/7, unattended**, on a customer Jetson. A model
cutover (`digitv2 → digitv3`) exposed two durable faults plus a safety gap:

1. **Stale catalog / dangling attachments.** `sync_models()` is insert/upsert-only
   over files on disk; it never prunes catalog rows whose `.engine` was deleted.
   Attachments (`camera_model`) kept pointing at the removed `digitv2.engine`;
   `detection_for()` returned that filename regardless of whether the file existed.
2. **Silent model-placement trap.** The app reads models from
   `denso::paths::models_dir()` = `<data_dir>/models`. New engines dropped into the
   *repo* `models/` dir were invisible to the running app.
3. **Whole-process fail-loud is too blunt for a multi-zone line.** A missing/invalid
   engine throws in `TrtEngine` ctor → `WarmupWorker` → `app.exit(1)`, crash-looping
   the *entire* reader when *one* camera's model is bad.

### Locked decisions

- **(A) Model swaps are deliberate maintenance actions** via
  `denso-setup replace-model`; a restart is acceptable. No hot-swap.
- **Per-zone fail-closed.** Each zone reports independently; a bad camera *or* model
  inhibits only its own zones while healthy zones continue.
- **Human-readable versioned model filenames** + SHA/sidecar in a **manifest**.
- **Backend is numeric-only** (one integer per zone; no alarm/sentinel values, no
  fencing).

### The primitives Codex corrected (rounds 1–2)

- **No atomic file + DB swap** (separate durability domains). Models are
  **immutable** — a new model is a new file; an orphan file is harmless, a DB row to
  an absent file is never committed. Atomicity → **safe ordering**.
- **Manifest, not a directory scan, is the artifact-identity authority** (§3.4).

### The reporting guarantee is CONDITIONAL on backend qualification (rounds 3–4)

The backend's per-zone semantics are **UNVERIFIED from inside the app** — we know
only the endpoint + request-body shape. Three unknowns:

- **U1** — does an omitted zone **retain** its prior value, or is it **cleared**?
- **U2** — does a **2xx** mean **every included zone was applied**?
- **U3** — are multiple included zones applied **atomically** (no observable mixed
  intermediate state)?

**U1 is a capability the feature depends on, not an optimization.** Given the three
locked constraints — an inhibited zone is *omitted*, its prior value is *never
re-sent*, and healthy zones keep reporting *through the same endpoint* — if omission
**clears**, the next healthy-zone POST necessarily clears the inhibited zone, and
**no client-side state machine can prevent it.** Supporting "omit the faulted zone +
preserve its backend value + keep reporting healthy zones" then requires a **backend
protocol change** (per-zone endpoint, PATCH, explicit retain marker, versioned
writes, or a fault representation).

Therefore the guarantee is stated conditionally:

> Per-zone remote reporting requires authoritative evidence that **U1 = retain** and
> **U2 = applied-on-2xx** for the *exact* backend identity + contract version. Until
> that is **qualified during commissioning**, the app **MUST NOT** enable or claim
> per-zone fail-closed continuation semantics. Even once qualified, an
> already-`post()`ed request cannot be recalled — so the app-side guarantee is "no
> value for an inhibited zone is *selected, added, or newly transmitted* after
> inhibition linearizes," and the backend shows **no** fault indicator (local alarm
> only). A pre-fault in-flight POST may still land.

---

## 2. Architecture overview

| Unit | Responsibility | Owner |
|---|---|---|
| **Model store + manifest** | Immutable versioned `.engine` + `.names.json`; `manifest.json` = sole artifact-identity authority. | files + shell |
| **`denso --migrate-model`** | Sole importer of a manifest entry into the DB and sole mutator of catalog + attachments. CAS, transactional, class-map by name, rollback receipt. | app |
| **`denso-setup replace-model` / `rollback-model`** | Stop-first orchestration + convergent rollback. | shell |
| **Readiness + integrity** | One computed verdict, provenance-classified: global blockers vs per-zone issues. | app |
| **Per-zone reporter (intent ledger)** | The change-only + inhibition + forced-recovery machine (§3.5). **Built + unit-tested now, remote semantics DORMANT until backend qualification** (§5). | app |
| **Backend qualification record** | Commissioning-time evidence binding {U1,U2,U3} to backend identity + version; selects the reporting mode (§5). | denso-setup |

Boundaries: **only app code touches the DB**; the shell owns privilege, systemd
lifecycle, staging, ownership, locks. The Jetson **retires the boot directory scan**.

---

## 3. Component design

### 3.1 Model store — immutable, versioned, manifested

- `replace-model` **never overwrites**; a change is a new versioned pair
  (`digit-v3.1.engine` + `digit-v3.1.names.json`).
- `<data_dir>/models/manifest.json`, one entry/generation:
  `{name, engine, engine_sha256, sidecar, sidecar_sha256, class_names, built_for{trt,cuda,sm}, installed_utc, state:"installed"}`.
- **State is `installed` only** — "active" is attachment-specific, **derived from
  `camera_model`**. Identity is the **SHA**, never the filename (same name + different
  bytes ⇒ hard error).
- **Path hygiene:** basenames only; no traversal/symlink escape; matching stems;
  duplicate names/JSON keys rejected.
- **GC** of unreferenced `installed` generations is a **separate maintenance step**,
  never on boot.

### 3.2 `denso --migrate-model` — sole DB mutator & manifest importer

Headless subcommand (dispatched before `QApplication`). **All-or-nothing in one txn:**
(1) **re-validate identity immediately before the txn** (manifest membership,
canonical paths exist, SHAs match); (2) **CAS** — fail unless each named camera
*currently* attaches the expected `--old` model (no broad
`UPDATE … WHERE model_id=old`); (3) upsert the new model from the **manifest**;
(4) **class-threshold map by class *name*** (exact ordered equality or explicit
`--class-map`; refuse duplicate/removed/ambiguous/out-of-range); (5) re-point
attachments; (6) **write a rollback receipt** (attachment ids, old/new identities,
prior selections+thresholds, forward+inverse map); (7) machine-readable JSON +
distinct exit code on failure.

### 3.3 `denso-setup replace-model` / `rollback-model` — stop-first orchestration

Concurrency **prohibited**: `systemctl stop` → assert `--check-running` = not running
→ **dedicated maintenance lock** (separate from the app single-instance lock; shared
by configure/replace/rollback/uninstall) → stage in `models/.staging/<unique>/`
(verify SHAs) → **validate: deserialize AND one deterministic smoke inference** with
expected bindings/shapes, non-overlapping engine lifetimes → `fsync` + move under the
new name + `fsync` dir + append manifest (atomic) → **commit via `--migrate-model`**
(re-checks identity) → **`--check` gates restart on its structured outcome** (§3.4)
→ `systemctl start`.

**Rollback is convergent:** fully validate the retained generation before its CAS;
run CAS against the **receipt's exact attachment set** via the **inverse class map**;
**auto best-effort rollback** on a forward-commit-then-verify-fail *under the lock*;
a non-applicable rollback CAS enters a defined **"manual reconciliation required"**
state — never a broad update.

### 3.4 Catalog authority & readiness — computed, provenance-classified

**Retire the Jetson boot directory scan.** `sync_models()`'s upsert-by-filename
(`model_sync.cpp:19`) lets unmanifested engines become selectable and sidecar edits
rewrite class identity — bypassing SHA/immutability. Authority split: **manifest** =
identity; **DB** = attachments/classes/thresholds; **`--migrate-model`** = the only
import path.

Readiness = one verdict, **computed fresh** (no persisted `missing` bit), by **error
provenance**:

**Global blockers** → `status.json (blocked)`, log, **`EX_CONFIG` (78)**, systemd
**not** restart-looping: DB unopenable / schema newer / migration failure / DB query
**failure** (distinguish "no rows" from "query failed"); `models_dir` unreadable or
manifest corrupt; **shared backend failure** (CUDA device unavailable, TRT runtime
init failure, GPU inaccessible — *every* model untrustworthy ⇒ global).

**Per-zone issues** → app **boots and runs healthy zones**; only affected zones
inhibit (§3.5). `kind ∈ { manifest_entry_missing, engine_missing,
sidecar_missing_or_invalid, sha_mismatch, artifact_deserialize_failed,
artifact_smoke_infer_failed, class_selection_incompatible, capture_open_failed,
capture_offline_terminal, inference_worker_failed }`.

**Structured outcomes:** `ready | degraded | blocked | replacement-failed`. A
pre-existing unrelated per-zone fault must never strand healthy zones. Same routine
backs boot, `--check`, `denso-setup verify/status` (SSH-visible). **Supersedes the
blanket `app.exit(1)`**; `EX_CONFIG` = global blockers only.

### 3.5 Per-zone reporter — the intent-ledger state machine

*(Built + unit-tested now; remote per-zone semantics DORMANT until §5 qualification.
The machine below is what runs in `Qualified Sparse` mode.)*

Zones map to cameras deterministically (each ROI's `zone` is machine-wide unique). A
camera's zones = `{camera_area.zone WHERE camera_id = X}`.

**Composable causes** — a bitmask `{AreasNeedReview, ModelUnavailable, ModelInvalid,
CaptureOffline, InferenceWorkerFailed}`; `is_inhibited := cause_set ≠ ∅`; releases
only when **all** clear. Model/source failure is **recomputed from integrity +
capture state**, never persisted via `camera.areas_need_review` (`repo.cpp:290`);
UI banners/actions are reason-specific.

**Single synchronized authority (§3.5.1).** `last_reported_value`, `cause_set`,
debounce, recovery generation, pending intents, epochs, and **ACK application** live
in one state machine under **one `ZoneReporter` mutex**; every observation (inference
worker), inhibit/release (GUI thread), and network ACK acquires it. Callbacks fire
**outside** the mutex (`zone_reporter.cpp:17`). Split `last_reported_value`
(2xx-confirmed) from `latest_stable_value` (observed) — today the aggregator advances
`last_sent_` before HTTP success (`zone_aggregator.cpp:63`); the new model advances
`last_reported_value` **only on ACK**.

**Fence intents, not snapshots (§3.5.2).** Every value in a POST carries an immutable
token `{zone_no, value, epoch:uint64, intent_id:uint64, kind: Normal|RecoveryForced}`.
A 2xx applies to the **exact tokens carried**. Keep **one combined single-flight POST
+ one global backoff**, but make the *logical* retry state **per-zone**
(`pending_intents[zone]`, `in_flight_batch: map<zone,token>`,
`last_reported_value[zone]`) — the whole-snapshot retry (`pending_=snapshot`,
`pending_==delivered_`, success-replaces-`delivered_`; `brazing_retry_policy.cpp:15,26,35`)
is **wrong for deltas** and is replaced.

**States:** `INHIBITED | RECOVERING | HEALTHY_IDLE | HEALTHY_PENDING` (in-flight
orthogonal). **Transitions (§3.5.3):** boot → INHIBITED if causes else RECOVERING
(forced resend; `last_reported=unknown`); first cause → bump epoch, drop pending
unsent, **preserve** `last_reported`, leave in-flight alone; last cause clears → new
`recovery_generation` + forced resend → RECOVERING; recovering + stable → a
`RecoveryForced` pending intent; healthy + stable≠last (or unknown) → `Normal`
pending; healthy + stable==last → idle; **batch send** re-validates epoch +
`cause_set.empty()` under the lock; **non-2xx** requeues still-current healthy
intents, never clears forced resend; **2xx** sets `last_reported` for tokens carried,
clears forced resend **only if the token matches the current `recovery_generation`**,
never clears causes.

**Cancellation & in-flight ACK (§3.5.4):** pending-unsent removed on inhibit; queued
GUI send rejected at dispatch if stale/inhibited; **already `post()`ed = irrevocable**
— a pre-fault in-flight 2xx after inhibition sets `last_reported` but does not remove
inhibition, clear recovery, or authorize a new send (the admitted residual race).

**Same-round ordering (§3.5.5):** *assert causes → clear/start recovery →
observations/debounce → send selection*; any cause assertion wins; the mutex
acquisition order **is** the linearization order.

**Alarm surfaces are LOCAL only (§3.5.6)** (backend can't show fault): log ERROR
(episode-collapsed), `status.json` per-zone compositional `degraded` state, UI tile
banner (`set_review_paused` → `set_inhibited(reasons)`). Diagnostic preview allowed
only with **no reporting sink**; the `OrientationProcessor` fallback
(`camera_grid.cpp:195`) is **prohibited** unless the camera's zones are already
inhibited. Preserve teardown order — workers join before reporters destroyed
(`camera_grid.cpp:59`).

---

## 4. Boot & reporting-ready sequence

Video display must **not** block on HTTP; the gate below is **"production
reporting,"** not video streams. And production reporting is enabled **only** when
the backend qualification (§5) permits it.

1. Load handler → open DB → migrate → **load & validate `manifest.json` (no directory
   scan)** → **integrity verdict**. Global-blocked ⇒ `status.json` + `EX_CONFIG`.
2. **Install all inhibition causes before any zone observation can publish.**
3. Start capture/inference **reporting-gated**; healthy zones accumulate fresh
   debounce (`last_reported=unknown` ⇒ every healthy zone is a forced initial report;
   inhibited zones omitted).
4. **Consult the qualification record** (§5) to choose the mode. If unqualified ⇒
   per-zone fail-closed reporting is **not** enabled (Unqualified/Legacy per policy).
5. In a qualified mode: submit the forced initial set; optionally require its 2xx
   before declaring **production reporting ready**; then enable steady-state
   reporting.

**A zone goes bad at runtime** (offline/terminal-reconnect/worker death) enters the
**same interlock immediately** — not after the 10 s aggregator expiry.

---

## 5. Backend qualification (CONDITIONAL DESIGN — gates all remote per-zone behavior)

The reporting state machine (§3.5) is implemented and testable now, but its **remote
per-zone semantics stay dormant** until the deployed backend is qualified.

### 5.1 Three reporting modes, selected by the qualification record

| Backend qualification | Permitted reporting |
|---|---|
| **U1/U2 unverified** | `Unqualified/Legacy` — no per-zone fail-closed claim; either keep today's behavior *explicitly labelled legacy/unverified*, or block production reporting (deployment policy). Today's whole-snapshot behavior must **not** be called fail-closed, and must not re-send a faulted zone's cached value. |
| **U1 = clear** | Per-zone continuation **unsupported** — requires a backend protocol change. |
| **U1 = retain, U2 = false** | Do **not** maintain ACK-confirmed per-zone `last_reported`; the intent ledger is unsafe — protocol change required. |
| **U1 = retain, U2 = true** | `Qualified Full-Set` (send the full healthy set on change / membership transition) **or** `Qualified Sparse` (§3.5 intent ledger). |
| **+ U3 = true** | May additionally claim atomic multi-zone visibility. |

**Conservative "full-set" mode is NOT immune to U2/U3** (a silent partial-apply under
a 2xx leaves a dropped zone stale until it next changes). A periodic reconciliation
push is **availability hardening, not delivery proof** — never advance
"confirmed-delivered" state on an ambiguous 2xx.

### 5.2 Qualification is manual commissioning, never an auto startup probe

A mutating probe at app startup is prohibited (it would overwrite real values, race
other writers, run every restart, and lacks an authoritative read-back). Instead:

1. Run the contract test against a **staging/sandbox endpoint or a controlled
   production maintenance window**, reporter stopped/isolated, no other writer.
2. Verify via an **authoritative observation channel** (a GET/admin query/vendor test
   tool; a customer display counts **only** if it faithfully shows stored state and
   the tester controls timing — else the backend owner must supply a GET or a written
   contract).
3. Record **named human sign-off** + before/after evidence; restore production values.
4. Provision a **qualification record** bound to backend identity + contract version:
   `{endpoint_identity, contract_version, qualified_utc, qualified_by,
   omission_retains, all_included_applied_on_2xx,
   multi_zone_atomic: confirmed|not_required|unverified, observation_method,
   evidence_ref}` — written by **`denso-setup` at commissioning**, not the GUI.
5. At startup, **validate the binding**; on endpoint/version mismatch, revert to
   `Unqualified/Legacy`. **No probe, no self-enable.** Requalify after any backend
   upgrade or material config change.

### 5.3 Contract test (in the implementation plan)

Beyond the human's seed-Z1Z2 / POST-Z1 / assert-Z2-retained + multi-zone-2xx:

- Confirm an **authoritative read-back** exists (else qualification is a manual
  sign-off, not automated).
- Seed Z1+Z2 and confirm the seed applied; POST only Z1 → Z1 changed, Z2 retained;
  **reverse** (POST only Z2 → Z1 retained); positional first/middle/last; idempotent
  repeat; empty-object behavior; unknown/duplicate/malformed/one-invalid-among-valid;
  whether all zones are required; **timeout/disconnect** (apply-without-2xx);
  concurrent-writer conflict; bind results to endpoint identity/version.
- **U3 is separate from U2:** a final read-back proves *eventual* application, not
  *atomic* application. Proving atomicity needs a vendor guarantee, revisioned
  read-back, an intermediate-state observation stream, or failure injection. **If the
  product does not require cross-zone atomic visibility, drop U3 from the sparse
  gate** and say so — sparse per-zone accounting needs only U1 + U2.

---

## 6. Failure modes & operational cases

Power loss at each boundary (immutable filenames + fsync ⇒ orphan file worst case;
DB never commits a dangling reference; idempotent resume adopts matching-hash bytes,
rejects differing bytes under the same name); disk-full / read-only / wrong-ownership
(abort before mutation; `status.json`/log best-effort, **stderr + exit code
authoritative**); sidecar/engine mismatch (SHA + `built_for`); in-flight 2xx after
inhibition (irrevocable, admitted); empty-vs-failed DB query (distinguished);
multiple models per camera (named old→new); verification GPU memory (non-overlapping
lifetimes); "deserializes but inference fails" (mandatory smoke); downgrade across
schema (receipt-CAS rollback; forward schema blocks binary downgrade).

---

## 7. Testing strategy

- **Pure/unit (host):** manifest parse/validate + SHA + path hygiene; class-map by
  name; readiness classifier (provenance; empty-vs-failed query); CAS; rollback
  receipt + inverse map.
- **Reporter state machine (deterministic interleavings):** pre-fault ACK after
  inhibit; forced-resend survives a failed POST; forced intent not cleared by a
  `Normal`-equality rule; coalescing `100→101→100` with an in-flight `100`; cause
  reasserts mid recovery-debounce; cold-boot forced initial report; stale-token/epoch
  rejection. Plus a worker-thread stress test.
- **Backend contract test (§5.3):** the full matrix, against staging or a controlled
  window — **the gate for enabling per-zone remote reporting.**
- **Integration:** `--migrate-model` (success, CAS-refusal, class-incompat, identity
  re-check fail); `replace-model` + `rollback-model` incl. forward-commit-then-verify-
  fail auto-rollback; fault injection.
- **On-device (Jetson):** real deserialize + smoke; one zone inhibited + alarmed
  (locally) while others report; forced recovery-resend; `denso-setup status` / SSH.

---

## 8. Scope & shipped guarantee

**Ships now, fully usable:** immutability + manifest authority; retire the boot scan;
`denso --migrate-model` (CAS + identity re-check + class-map + receipt);
`replace-model` + convergent `rollback-model`; provenance-classified readiness
(`EX_CONFIG` global gate + per-zone issues incl. camera/source failures) superseding
blanket `exit(1)`; the **per-zone intent-ledger reporter built + unit-tested**; local
alarm (log + `status.json` + UI); smoke-inference validation.

**Ships DORMANT, enabled only after commissioning qualifies the backend:** all
**remote** per-zone behavior (sparse or full-set), because it depends on U1 (retain)
+ U2 (applied-on-2xx) which cannot be verified from inside the app.

**Conditional guarantee (final):** *if* the deployed backend is qualified U1+U2, the
app never *initiates* a fresh report for a faulted zone and forces a corrected value
on recovery; a pre-fault in-flight POST may still land; the backend retains the last
accepted value with **no** fault indicator (local alarm only). If the backend is
**unqualified or U1=clear**, per-zone fail-closed continuation is **not** claimed and
requires a backend protocol change.

**Later (separate slices):** unreferenced-generation GC; jittered periodic
reconciliation (post-qualification resilience); full fault-injection matrix in CI;
downgrade-across-schema tooling.

---

## 9. Decision log (debate synthesis)

- **r1** Immutable versioned filenames + manifest SHA; no atomic file+DB swap.
- **human** Per-zone fail-closed, app boots partial fleets; supersedes `exit(1)`.
- **r2** Manifest sole authority; retire boot scan. End-to-end interlock. Camera/source
  failures inhibit too, immediately. Provenance classification. Convergent rollback +
  receipt. Composable causes; model alarm not via `areas_need_review`.
- **human** Backend numeric-only; change-only + forced recovery-resend.
- **r3** Fence **intents** (tokens), not snapshots; per-zone intent ledger; single
  locked authority incl. ACK application; recovery obligation needs generation
  identity; cold-boot forced initial report; app-initiation guarantee (in-flight may
  land; no backend fault display).
- **human** Backend semantics UNVERIFIED (U1/U2/U3); enable sparse only after a
  contract test; document as conditional.
- **r4** U1 = a required backend *capability*, not an optimization (omission=clear ⇒
  feature impossible without a protocol change). Conservative mode not immune to
  U2/U3. Three modes (Unqualified/Legacy, Qualified Full-Set, Qualified Sparse) gated
  by a **qualification record** bound to backend identity/version. **No auto startup
  probe** — manual commissioning with authoritative read-back. U3 separate from U2;
  final read-back proves eventual, not atomic, application.
