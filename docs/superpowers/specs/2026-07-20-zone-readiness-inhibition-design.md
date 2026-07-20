# Slice (b) — Readiness/Integrity, Per-Zone Inhibition, and Local Alarms

Status: **design approved, not implemented**
Date: 2026-07-20
Supersedes nothing. Implements slice (b) of
`2026-07-20-model-lifecycle-24-7-design.md` (§3.4, the local half of §3.5, §3.5.6).
Slice (a) shipped as `175d1f4` (manifest + sha256 + `denso --migrate-model`, schema v13).

---

## 1. Scope

Slice (b) makes the application **correctly identify, isolate, and display zone
faults**. It does not change what per-zone semantics we *claim* of the backend.

**In scope:** readiness/integrity verdict; composable per-camera inhibit causes
projected onto owned zones; a bounded soft hold for incomplete digit readings; local
alarm surfaces (log, `status.json`, UI); replacing the blanket `app.exit(1)`.

**Explicitly out of scope** (slice (c)): epochs, intent tokens, ACK-confirmed
`last_reported`, per-zone retry replacing `BrazingRetryPolicy`, backend
qualification, the U1/U2 contract test.

**Explicitly out of scope** (slice (b2)): the right-anchor digit-slot assignment and
per-zone pitch/anchor calibration. See §6.

**Explicitly out of scope** (own migration slice): retiring the `sync_models()`
directory scan. See §2.3.

### 1.1 What this slice does NOT claim

The backend is **numeric-only**: one integer per zone, no alarm, invalid, health, or
sentinel value. It cannot represent "this zone is faulty". Whether omitting a zone
from a POST means RETAIN or CLEAR (**U1**) is **unverified** and is qualified in
slice (c).

Therefore: **a faulted zone is invisible to the backend under every option in this
design.** Local alarm is the only fault channel. Nothing here may be described as
"per-zone fail-closed". Immediate eviction is a *local freshness* improvement, not a
demonstrated end-to-end safety improvement — if U1 turns out to be RETAIN, immediate
eviction changes nothing backend-visible.

---

## 2. Readiness & integrity verdict

### 2.1 One shared routine

A pure, unit-testable routine in core, backing **both** boot and `--check`. These must
not diverge: a `--check` that passes an installation boot would refuse is worse than
no check at all.

**The verdict routine is READ-ONLY and takes the catalog as an input.** This is what
makes the shared-routine claim honest: boot mutates (it runs `sync_models()`), whereas
`--check`'s existing contract is strictly non-mutating. So the scan stays a *boot-only*
step that happens **before** the verdict, and the verdict itself observes state rather
than producing it. `--check` computes the same verdict over the current catalog plus a
read-only directory listing — enough to report `engines_unmanifested` without writing
anything.

**`--check` exit contract** (previously undefined for a degraded result, which would
have made "non-blocking degraded" operationally meaningless):

| Verdict | Exit | Meaning |
|---|---|---|
| `Ready` | `0` | All zones healthy |
| `Degraded` | `10` | Boots and runs; some zones inhibited. Diagnostics on stdout. Distinct from `1` so a generic failure is never mistaken for a degraded-but-serviceable appliance. |
| `Blocked` | `78` (`EX_CONFIG`) | Will not boot; matches the boot exit code exactly |

```
enum class Readiness { Ready, Degraded, Blocked };

struct IntegrityVerdict {
    Readiness                  status;
    std::vector<GlobalBlocker> blockers;  // provenance: whole machine
    std::vector<ZoneIssue>     issues;    // provenance: per camera/zone
};
```

Classification is by **error provenance**, not by severity guesswork.

### 2.2 Global blockers → `status.json`, log, `EX_CONFIG` (78)

Exit code 78 so systemd does **not** restart-loop on a condition no restart can fix:

- DB unopenable; schema **newer** than this build knows; migration chain failure
- **DB query FAILED** — distinguished from "query returned no rows". Conflating them
  turns a broken DB into a silently empty fleet.
- `models_dir` unreadable; `manifest.json` present but corrupt
- shared backend failure (CUDA device unavailable, TRT runtime init failure) — *every*
  model is untrustworthy, so the provenance is global

### 2.3 Per-zone issues → boot **degraded**, inhibit only affected cameras

`engine_missing`, `sidecar_missing_or_invalid`, `sha_mismatch`,
`artifact_deserialize_failed`, `class_selection_incompatible`, and:

**`engines_unmanifested`** — engines catalogued by the directory scan but absent from
`manifest.json`. Reported as actionable and degraded; **never blocking**.

> **Compatibility decision.** `sync_models()`'s directory scan is **retained** in this
> slice. Verified 2026-07-20: the production Jetson holds `digitv3.engine` +
> `digitv3.names.json` and **no `manifest.json`** — it works today *only* because of
> the scan. Making manifest authority mandatory now would globally block the machine
> currently running production. Retirement moves to its own slice, paired with a
> migration tool that generates and validates a manifest from installed engines.

`replacement-failed` from the parent spec is **omitted**: its only producer would be
`rollback-model`, which slice (a) did not build. Deferred to the rollback slice rather
than shipping an unreachable enum value.

---

## 3. Inhibition — causes, gate, and the aggregator

### 3.1 Causes are per-camera, conservatively projected

```
enum class ZoneCause : uint32_t {
    AreasNeedReview, ModelUnavailable, ModelInvalid,
    CaptureOffline,  InferenceWorkerFailed
};
```

Held as a bitmask **per camera**. `is_inhibited := cause_set != 0`; a camera releases
only when **all** causes clear.

> **Causes are evaluated per camera and conservatively projected onto all owned
> zones. This design does NOT implement precise per-zone isolation.**
> `class_selection_incompatible` genuinely belongs to a camera-model attachment, so
> camera-wide inhibition can suppress zones that might still be trustworthy. That is
> an accepted conservative bias, stated rather than hidden.

Camera offline / invalid model / inference-worker failure inhibit **every** zone owned
by that camera. `AreasNeedReview` is projected to all owned zones while it remains a
camera-level persisted flag. An incomplete digit reading is a **soft hold of only the
affected zone** (§5) — not a camera-level inhibit.

### 3.1.1 Two inhibit scopes

Camera causes are not the only source of inhibition: a hold that exceeds its timeout
(§5.3) inhibits **one zone** on an otherwise healthy camera. So effective inhibition is
the union of two scopes:

```
zone_inhibited(z) := camera_causes[owner_of(z)] != 0     // §3.1, projected
                  || zone_inhibit[z]                      // currently: hold timeout only
```

`zone_inhibit` is a per-zone flag with exactly one producer in this slice (hold
timeout). Both scopes are evaluated under the reporter mutex (§3.3a), so a camera-level
release cannot un-inhibit a zone that timed out its hold, and vice versa.

**The two scopes gate DIFFERENT things, and conflating them is a permanent-inhibit
trap.** The asymmetry is deliberate:

| Scope | Recovery evidence | Gate blocks |
|---|---|---|
| `camera_causes` | **External** — the camera comes back online, the model revalidates, the worker recovers. Arrives as a cause *clear*, not as an observation. | The whole observation. Nothing is evaluated. |
| `zone_inhibit` (hold timeout) | **Only the observations themselves** — there is no external signal that a meter became readable again. | **Publication only.** Observations still update private debounce state. |

If `zone_inhibit` blocked observations the way camera causes do, the zone could never
accumulate the complete readings needed to clear its own inhibition, and would stay
inhibited until process restart. **Quarantined recovery** avoids that:

> A zone with `zone_inhibit` set continues to be fed to `observe()`, rebuilding its
> `Debounce` normally, but is **excluded from the emitted snapshot**. When a *complete*
> reading passes debounce, `zone_inhibit` clears and the zone publishes in the same
> critical section — forced, because `last_sent_` no longer holds it.

So the rule is: **camera causes drop the observation; a zone hold-timeout drops only
the publication.**

### 3.2 Cause transitions are serialized on the GUI thread

All four cause sources funnel through **one owner thread**:

| Cause | Source | Thread |
|---|---|---|
| `AreasNeedReview` | DB bool at load; wizard save | GUI |
| `ModelUnavailable` / `ModelInvalid` | §2 verdict at boot | GUI |
| `CaptureOffline` | `CameraStream::status_changed(Offline)` | GUI (already a queued signal) |
| `InferenceWorkerFailed` | `infer_fail_streak_` past threshold | inference worker → **`common::post_to_gui`** |

Only the last originates off-thread, so one `post_to_gui` makes every transition
GUI-ordered. Consequently **`ZoneHealth` needs no mutex**, no revision counter, and
there is no ZoneHealth↔ZoneReporter lock ordering to reason about. `ZoneReporter`
keeps its existing mutex because it genuinely spans threads.

**Wiring requirement (load-bearing, not incidental).** `status_changed` is *emitted on
`CameraStream`'s private capture thread* — it is not intrinsically GUI-delivered. It
reaches the GUI thread today only because `CameraGrid` connects it to a GUI-affine
receiver under `Qt::AutoConnection`, which Qt then queues. The health connection **must
likewise use a GUI-affine receiver/context object with an auto/queued connection**. A
contextless functor or an explicit `Qt::DirectConnection` would execute on the capture
thread and silently invalidate the single-owner claim — reintroducing the ordering
hazard this section exists to remove.

Note that a cause transition can still race an observation in wall-clock terms; that is
harmless precisely because both the predicate and `observe()` are taken under the
reporter mutex (§3.3a), so mutex acquisition order *is* the linearization order.

This removes the ordering hazard where an older add/clear overtakes a newer one.

### 3.3 The reporter gate

Four corrections, all under `ZoneReporter`'s **existing** mutex:

**(a) Inhibition is a predicate, not a one-shot evict.** `on_zones(camera_id, …)`
consults the inhibited set under the *same* mutex as `observe`. Without this, a worker
already blocked on the mutex re-inserts the zone the instant `evict()` releases it —
the zone resurrects while still inhibited and re-reports after 5 frames. `camera_id`
is currently accepted and ignored (`/*camera_id*/`), so the gate is nearly free.

**(b) Ownership is recorded, not derived.** The reporter records camera→zones from
observations it *accepts*. A config change evicts previously-recorded zones **before**
installing the new set. Otherwise renumbering an ROI from zone 7 to 9 evicts 9 and
leaves stale zone 7 publishable. Machine-wide zone uniqueness is **validated** as an
integrity invariant, not assumed.

**(c) Eviction emits.** `evict` reports whether `last_sent_` changed and emits the
shrunk snapshot, mirroring the existing expiry path. Erasing the maps alone POSTs
nothing, so the zone would linger on the backend until some *other* zone changed.

**(d) Snapshot publication is ordered.** Callbacks fire outside the mutex and marshal
from different threads, so an eviction and a recovery snapshot can arrive reversed —
whole-snapshot latest-wins would then treat the eviction as newest and the recovered
zone would vanish indefinitely. A monotonic sequence number is assigned under the
mutex; the GUI side drops any snapshot older than the last applied.

**Never emit an empty snapshot.** The aggregator's "a genuinely empty snapshot never
arises" invariant breaks once eviction exists, and `build_brazing_payload({})` emits
literal `"{}"`. If all cameras drop, that POST under U1=CLEAR could clear every zone.

A suppressed empty snapshot **must not consume or advance the sequence number** of
§3.3(d). If suppression burned a sequence, the GUI side's drop-stale rule would discard
the next genuine snapshot as "older" and the recovery would be lost. Sequence numbers
are assigned only to snapshots actually dispatched.

### 3.4 Inhibit / release semantics

**Inhibit** = mark camera inhibited **and** evict its recorded zones, atomically under
the mutex. **Release** = clear the flag only. The zone re-enters naturally on the next
accepted observation: it rebuilds a fresh `Debounce{}` (so it must re-earn
`kStableFrames` = 5), and because `last_sent_` no longer holds it, the
`it == last_sent_.end()` branch marks it `changed` — yielding one forced fresh report
even if the value equals the pre-fault value. No epochs, flags, or generations.

*(This natural-re-entry property holds for the hard-inhibit path only. The soft-hold
path needs an explicit flag — see §5.3.)*

---

## 4. The live bug this slice fixes

`assemble_zone_value` (`src/app/camera/zone_assembly.cpp`) sorts detected digits
left-to-right, **concatenates however many it found**, and accepts any count 1..3:

```cpp
if (digits.empty() || digits.size() > 3) return std::nullopt;
return std::stoi(digits);   // "13" -> 13
```

It guards against **extra** digits and not at all against **missing** ones. A meter
showing `123` whose tens digit is missed yields the string `"13"` → the integer `13`,
POSTed as a normal reading and indistinguishable from a genuine thirteen. Rollover
case: `130` with the tens missed → `10`. **This is live in production.**

This also motivates retaining the **entire** previous value rather than substituting
individual stale digits: with a previous value of `129` and a real value of `130`,
reusing the old tens digit would synthesise `120` — a number that was never displayed.

---

## 5. Interim gap mitigation and the bounded hold

### 5.1 Assembly returns a sum type, never a valid-looking integer

```
struct ZoneAssembly {
    enum class Kind { Complete, Incomplete, NoDigits } kind;
    int value;   // meaningful ONLY when kind == Complete
};
```

An incomplete reading **must never construct a reusable integer anywhere in the
pipeline**. A detected `1_3` yields `Incomplete`, never a `13` that some later stage
could mistake for a reading.

### 5.2 The gap guard — deliberately narrow

Pitch is estimated from **median detected box height**, not width: `1` is
substantially narrower than other digits, so width is a poor pitch estimator, whereas
seven-segment glyphs share a consistent height across classes.

```
expected_pitch := median(box.height) * kPitchPerHeight   // default 0.70
Incomplete     := any adjacent centre distance > kGapFactor * expected_pitch
                                                          // default 1.60
```

Both constants are named and tunable in one place, with the defaults above as the
starting point; (b2) refines them against recorded production detections.

Thresholds are **biased to under-detect**. A missed gap leaves today's behaviour; a
false gap freezes a healthy zone, which is worse than the bug being fixed. A gap of
2.0 pitch (one missing digit) clears 1.60 comfortably, so the guard fires on the case
it targets while tolerating pitch-estimate noise.

> **DOCUMENTED LIMITATION — this does not detect a missing LEADING digit.** `123`
> losing its hundreds digit reads as `23`, which is geometrically indistinguishable
> from a genuine `23` when leading zeros and fixed digit counts are not guaranteed. No
> geometric rule can close this; it needs either a guaranteed fixed digit count or a
> detector that classifies a blank/unlit position as its own class. Neither is in
> scope. **The residual is accepted and stated, not hidden behind the guard.**

Note also that this guard cannot ship *without* §5.3: an incomplete reading that
simply emits no `ZoneReading` stops the zone being seen, the 10 s expiry erases it,
and a shrunk snapshot drops it — the opposite of "retain the last valid value".

### 5.3 Zone states and the bounded hold

`Healthy | HoldingLastValid | Inhibited`

**Two timestamps, tracked separately:**

- `last_seen_ms` — refreshed by **any** frame including incomplete ones. Incomplete
  frames prove the *camera* is alive.
- `last_complete_ms` — refreshed **only** by a complete reading. The hold timeout is
  measured against this.

Conflating them is what would let a permanently missed digit freeze an old meter value
forever.

**`NoDigits` is treated exactly as `Incomplete`** for hold and liveness purposes. This
is not cosmetic: if `NoDigits` did not refresh `last_seen_ms`, the 10 s expiry would
erase the zone and emit a shrunk snapshot **long before** the 30 s hold timeout ever
ran — the short timer would silently pre-empt the long one and the bounded hold would
never be reached. A frame that yields no digits still proves the camera is alive.

**On an incomplete (or no-digit) reading:** refresh `last_seen_ms` (defeats the 10 s
expiry); set `count = 0` so frames either side of the gap cannot combine into five
"consecutive" complete observations (`candidate` may be left as-is — it cannot
contribute without `count`); leave `stable`, `has_stable`, and `last_sent_` untouched;
retain the complete `last_valid_value`; set `needs_reannounce`. Do **not** refresh
`last_complete_ms`, do not treat as a fresh observation, do not update
`last_reported_value`, do not POST.

**On recovery before the timeout:** five fresh consecutive complete frames, then emit
**one** value even if equal to the pre-hold value, then resume change-only reporting.

The existing `changed` computation cannot express this on its own. Today `changed` is
only evaluated inside the `!has_stable || stable != candidate` branch, which is **false**
on an equal-value recovery — so the forced report would be silently dropped. The
required contract is explicit:

> When `count >= stable_frames_` **and** `needs_reannounce` is set, `changed` is set
> even if `stable == candidate` and `last_sent_[zone]` already equals it.
> `needs_reannounce` is cleared **exactly when the snapshot containing that zone is
> committed** (alongside `last_sent_ = snapshot`), never when the flag is merely read —
> otherwise a suppressed or superseded snapshot would consume the re-announce and lose
> it.

Clearing `last_sent_[zone]` is **not** an acceptable alternative route to forcing the
report: it would emit a transient shrunk snapshot that drops the zone from the payload
mid-hold, which is precisely what the hold exists to prevent.

**On exceeding the hold timeout:** the zone **escalates to `Inhibited`** (setting
`zone_inhibit[z]` per §3.1.1) and is evicted. The timeout is a single configurable
duration, `kHoldTimeoutMs`, default **30 000 ms** — comfortably longer than a transient
occlusion or a few dropped frames, short enough that a stale value is never presented
as live for minutes. Measured against `last_complete_ms`, never `last_seen_ms`.

A hold must never silently persist indefinitely and must never silently transition back
to `Healthy`: the only exits are a complete reading passing debounce (→ `Healthy`, one
forced report) or the timeout (→ `Inhibited`, evicted).

---

## 6. Deferred to slice (b2)

Right-anchor digit-slot assignment (units anchored to the ROI's right edge, slots
consecutive and ending at units) plus **persisted per-zone calibration** of the units
centre and pitch, estimated from many high-confidence frames with quality checks.

Deferred because ROI-edge anchoring is **release-blocking without calibration**:
existing ROIs were drawn only to *enclose* the digit field, and any right-side slack
shifts every digit one slot left, reads as "missing units", and freezes the zone
forever. Cheap pitch sources were ruled out — box width isn't character pitch, and
ROI-width/N re-imports the slack. (b2) requires an on-device calibration campaign
against the real meters, which cannot be validated from the Windows dev host.

**(b2) REPLACES §5.2, it does not refine it.** The `ZoneAssembly` sum type (§5.1) and
the bounded-hold contract (§5.3) are designed to survive unchanged — (b2) swaps only
the classification *inside* assembly, from the height-ratio gap heuristic to calibrated
slot assignment. Implementers should therefore keep §5.2's logic behind the
`ZoneAssembly` boundary and avoid leaking pitch/gap concepts into the aggregator or the
hold state machine, so (b2) is a contained substitution rather than a rewrite.

**(b2) must also verify a physical assumption this design does not establish:** that
short values are right-justified into fixed positions — i.e. that the `3` in a lone
`3` occupies the same physical position as the `3` in `123`. Displays that centre
short values, or shift glyphs when leading positions blank, would break any fixed
anchor+pitch scheme.

---

## 7. Local alarms

Backend visibility is unchanged and unchangeable; these are the only fault channels.

- **Log** — ERROR, **episode-collapsed** (once per episode with start/reason, not per
  frame), plus a recovery line with duration.
- **`status.json`** — per-zone compositional state, written **atomically** (temp +
  rename) so an abrupt restart cannot leave misleading status. Includes affected
  zones, causes, episode start, and hold state.
- **UI** — `set_review_paused(bool)` → `set_inhibited(reasons)` with reason-specific
  text, and a distinct indicator for `HoldingLastValid` (a hold is not an inhibit).

Preserve teardown order: workers join before reporters are destroyed.

---

## 8. Boot sequence

1. Open DB → migrate → load and validate `manifest.json` (scan retained per §2.3)
2. Compute the integrity verdict. Global-blocked ⇒ `status.json` + `EX_CONFIG` (78)
3. **Install all inhibition causes BEFORE any zone observation can publish**
4. Start capture/inference; healthy zones accumulate fresh debounce

Replaces both blanket `app.exit(1)` calls in `startup.cpp`. A zone going bad at
runtime enters the same interlock **immediately**, not after the 10 s expiry.

---

## 9. Testing

**Unit**
- Verdict provenance classification; **"no rows" vs "query failed"** distinguished
- Global blocker ⇒ `EX_CONFIG`; per-zone issue ⇒ degraded, healthy zones unaffected
- `engines_unmanifested` reports degraded and does **not** block
- Cause composition; release only when **all** causes clear
- **Resurrection race**: observation blocked on the mutex during inhibit is dropped
- Ownership: renumbering an ROI evicts the **old** zone
- Eviction emits a shrunk snapshot; **empty snapshot never emitted**
- Reordered snapshots: stale sequence dropped
- Gap guard: the **two-digit `1_3` case** (the case a median-of-gaps rule cannot
  detect, since with one gap the gap *is* the median)
- Assembly returns `Incomplete` and **constructs no integer**
- Hold defeats the 10 s expiry; re-announce fires on an **equal** value
- **Incomplete recovering before the hold timeout** → resumes, one forced report
- **Incomplete exceeding the hold timeout** → escalates to inhibited and evicts
- **One zone holding does not affect sibling zones on the same healthy camera**
- **Camera failure inhibits every zone owned by that camera**
- Two inhibit scopes (§3.1.1) are independent: a camera-level release does **not**
  un-inhibit a zone that timed out its hold
- **Quarantined recovery**: a zone inhibited by hold timeout still updates debounce and
  **does recover** on five complete readings — the permanent-inhibit trap does not occur
- A camera-cause-inhibited zone's observations are dropped entirely (contrast above)
- `NoDigits` refreshes `last_seen_ms`, so the 10 s expiry never pre-empts the 30 s hold
- `needs_reannounce` survives a suppressed/superseded snapshot and is consumed only on
  commit
- A suppressed empty snapshot does **not** advance the sequence number

**On-device (Jetson)** — camera unplugged mid-run: its zones inhibit immediately,
healthy zones keep reporting; `status.json` and the UI show cause and episode;
reconnect restores reporting with one forced fresh value.

---

## 10. Accepted limitations

1. **Missing leading digit is undetectable** (§5.2) — geometric indistinguishability.
2. **No backend fault visibility** (§1.1) — inherent to the numeric-only contract.
3. **Per-camera projection, not per-zone precision** (§3.1) — conservative bias.
4. **Directory scan retained** (§2.3) — compatibility; retirement needs a migration
   tool.
5. **Omission semantics remain unverified** (§1.1) — U1 is qualified in slice (c).
