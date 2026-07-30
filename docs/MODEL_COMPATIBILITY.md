# Model / Operating-Mode Compatibility

Maintainer and operator reference for how this appliance decides **which model
may be loaded, selected and attached in which operating mode**.

It describes the *committed* implementation. Where a rule exists only in code,
the source file is named; where a rule exists only in a packaging script, that
script is named. Nothing here is aspirational — a claim that is not backed by a
file in this repository does not belong in this document.

Related: `docs/ARCHITECTURE.md` (system-level data flow), `AGENTS.md` (operator
runbook and device rules), `CLAUDE.md` (source map and hard rules).

---

## 1. Purpose and scope

Three concerns are kept deliberately separate, and conflating any two of them is
the failure mode this design exists to prevent:

| Concern | Question it answers | Where it lives |
|---|---|---|
| **Declaration** | *What is this artifact?* | schema-2 `manifest.json` in the models directory |
| **Corroboration** | *Is the artifact on disk actually that?* | `models::resolve_model_metadata` — hashes, ordered class names, TensorRT `built_for` |
| **Authorization** | *May that thing run in this mode?* | one compiled policy, `src/core/models/compatibility.cpp` |

- **Canonical model identity is declared in the schema-2 `manifest.json`.**
  Identity is never inferred from a filename, a display name or a class
  signature. A catalog row is joined to a manifest generation by the active
  backend's declared artifact filename, and the identity returned is the
  *generation's*.
- **Runtime artifacts corroborate the declaration.** The declaration is only
  believed as far as the artifacts agree with it: SHA-256 over the active
  backend's files, ordered class-name and class-count agreement, and — on
  TensorRT only — the platform triple.
- **Authorization comes from one compiled central policy.**
  `denso::models::model_compatibility()` in `src/core/models/compatibility.cpp`
  is the sole authority on what a model may do. Every enforcement path calls it;
  none re-implements it.
- **Manifests must not contain `allowed_modes`.** The manifest declares *what an
  artifact is*, never *what it may do*. There is no field for `allowed_modes` to
  parse into (`src/core/models/manifest.h`), so a stray key in a file on disk is
  inert and can never become a second authority. This is what makes an operator
  edit in the models directory unable to widen a model's authorization.
- **Ball Leveler remains unavailable in the current production UI.** `mode.target`
  may be set to `ball_leveler` and the mode persists, but the appliance lands on
  an explicit "not available in this release" surface: no wizard, no production
  `CameraStream`, no `DetectionProcessor`, no `ZoneHealth` wiring, no reporter.
- **No Floating Ball percentage, calibration, level result or reporting algorithm
  exists** anywhere in this repository. The Float *models* are packaged and
  declared; the *application feature* that would consume them is not implemented.

---

## 2. Compatibility matrix

```text
digitv3
  family: digit_numeric
  allowed mode: digit_reader

float-small
  family: float_ball
  allowed mode: ball_leveler

float-big
  family: float_ball
  allowed mode: ball_leveler
```

**The only production matrix is in `src/core/models/compatibility.cpp`.** It is
two `constexpr` tables — `kFamilyModes` (family → modes) and `kModels`
(canonical id → family) — guarded by three compile-time invariants:

- every registered family allows at least one mode;
- **no family is allowed in both modes** — models are mono-modal by construction;
- every model's declared family exists in the family→modes table.

**Changing authorization requires a reviewed code change.** Adding a family,
adding a model, or widening a mode means editing that translation unit, adding
tests, and passing review. There is no configuration file, environment variable
or database row that can do it.

**Copying this matrix into manifests, SQL, shell scripts or UI conditions is
forbidden.** The table above is documentation for humans. It is not a registry,
and nothing may read it. A second machine-consumed copy is a second authority,
and two authorities eventually disagree — silently, in the direction of granting
a privilege nobody reviewed.

---

## 3. Manifest schema 2

Declared in `src/core/models/manifest.h`; parsed by `parse_manifest`, checked by
`validate_manifest`. Two schemas coexist: schema 1 is the shipped legacy format
and its behaviour is unchanged; schema 2 adds the declared identity, the
per-backend `runtime` blocks and the `provenance` block.

A schema-2 generation declares:

| Field | Meaning |
|---|---|
| `canonical_id` | the identity the policy matches on (a safe token) |
| `family` | the family the policy maps to modes |
| `task` | `"detect"` |
| `input_size` | `640` |
| `class_count` | number of classes |
| `class_names` | **ordered** class names, as the artifact carries them |
| `provenance` | how the artifact was produced (source `.pt`, ONNX, opset, exporter versions, batch/dynamic/nms, precision, JetPack, the export commands) |
| `runtime` | per-backend artifact blocks; **at least one must be present** |

### Per-backend runtime blocks

**ONNX Runtime** — the model file, its hash, and where class metadata comes from:

```json
"runtime": {
  "onnxruntime": {
    "model": "<stem>.onnx",
    "model_sha256": "<64 hex>",
    "class_metadata_source": "onnx_metadata_names"
  }
}
```

**TensorRT** — the prebuilt plan, the sidecar it takes class names from, both
hashes, and the platform the plan was compiled for:

```json
"runtime": {
  "tensorrt": {
    "engine": "<stem>.engine",
    "engine_sha256": "<64 hex>",
    "sidecar": "<stem>.names.json",
    "sidecar_sha256": "<64 hex>",
    "class_metadata_source": "names_sidecar",
    "built_for": { "trt": "10.3", "cuda": "12.6", "sm": "87" }
  }
}
```

`class_metadata_source` is a **closed vocabulary**, not free text: only
`onnx_metadata_names` and `names_sidecar` are accepted
(`kSourceOnnxMetadataNames` / `kSourceNamesSidecar`). A merely non-empty check
would accept a typo that silently described the wrong source while every other
field validated cleanly.

### Immutable backend ownership in `ManifestView`

`denso::models::ManifestView` (`src/core/models/model_identity.h`) binds a parsed
manifest, a models directory and a **backend that is fixed at construction**. The
production two-argument constructor binds `active_backend()` — the one
compile-time platform split that decides which backend's manifest block is read,
mirroring the `BackendEngine` alias split in `engine_registry.h` — so a caller
cannot select the wrong one. The
three-argument form taking an explicit `Backend` is a **test seam only**, so both
platforms' resolution can be exercised off-host.

The backend is deliberately *not* a parameter on `resolve_model_metadata`,
`evaluate_integrity`, `set_camera_models`, `detection_for` or
`selectable_models`: threading it through five call sites would be five more
places to get it wrong.

### Schema-1 behaviour and undeclared resolution

- A **schema-1** generation carries no identity declaration. It parses and
  validates exactly as before, but it resolves `declared == false`, so every
  model it describes is rejected with `model_undeclared`.
- A generation with **no block for the active backend** likewise resolves as
  undeclared — the artifact is described, but not for the runtime this appliance
  actually loads.
- An **absent, unreadable or unparseable** `manifest.json` collapses to an empty
  manifest via `load_manifest_view`. Fail-closed by construction: this loader can
  only ever narrow what is authorized, never widen it. (A malformed file is
  *separately* reported as the `ManifestCorrupt` global blocker by
  `evaluate_integrity` — that classification lives in exactly one place.)

Consumers must read TensorRT artifact data through the schema-aware accessors on
`ModelGeneration` (`tensorrt_engine()`, `tensorrt_engine_sha256()`,
`built_for_trt()`, …), never the raw root fields — those are empty for schema 2,
and a direct read would silently compare against nothing.

---

## 4. TensorRT `built_for`

`built_for` is a **TensorRT platform assertion** and lives **only** inside
`runtime.tensorrt`. A root-level `built_for` is **rejected** for schema 2. Left at
the root it would be readable — and therefore eventually read — on a Windows /
ONNX Runtime deployment, where a TRT/CUDA/SM mismatch is meaningless. Nesting it
makes a cross-platform rejection structurally impossible rather than merely
discouraged.

### Raw provenance vs normalised comparison values

The device reports full versions:

```text
TensorRT 10.3.0.30
CUDA 12.6.68
```

The values compared at runtime are **normalised** (major.minor, plus the SM
number):

```text
trt  = 10.3
cuda = 12.6
sm   = 87
```

`denso::platform::measured_platform_info()` (`src/app/platform/platform_info.cpp`)
is the one provider: it probes the device and normalises, and the result is
handed to `denso_core` as a `PlatformInfo`. `denso_core` never probes a device
itself. A probe failure **fails closed** — an empty `PlatformInfo` corroborates no
`built_for`, so nothing is authorized. It is never a substituted constant.

### The comparison is intentionally exact

`runtime.tensorrt.built_for` is compared for **exact string equality** against the
normalised measured triple. This is a deliberate choice, not an oversight: a
TensorRT plan is a compiled artifact, and "close enough" is not a property a
serialized plan has.

**Putting full versions in `runtime.tensorrt.built_for` causes:**

```text
model_provenance_failed
```

This was measured, not assumed. During Slice 12 a negative control reverted only
the Float `built_for` values to the archived full versions
(`10.3.0.30` / `12.6.68` / `87`); both Float models immediately resolved
`provenance_ok = false` → `model_provenance_failed` in **both** modes —
permanently unloadable while every hash and signature still looked correct.

**Do not add fuzzy, prefix or range matching.** The correct fix for a mismatch is
to emit the normalised triple in the descriptor, or to requalify the artifact for
the new platform baseline. Loosening the comparison would convert a loud,
diagnosable refusal into a silent acceptance of a plan built for a platform that
is not this one.

---

## 5. Artifact integrity

- **Engine and sidecar SHA-256 values are the artifact-integrity authority.**
  `resolve_model_metadata` sets `provenance_ok` solely from the active backend's
  declared hashes (plus `built_for` on TensorRT). The packaging chain layers the
  same discipline: `packaging/models.approved` approves the **pair**,
  `manifest_matches_models_dir` (`packaging/lib/policy.sh`) proves a manifest
  describes exactly the pairs on disk with agreeing hashes, and the packaged
  `lib/SHA256SUMS` covers the shipped payload.
- **TensorRT deserialisation is not an integrity check.** A plan that
  deserialises successfully has proven that it is structurally a plan this
  runtime accepts — nothing more. It has not proven that it is *the* plan that
  was approved.
- **A modified byte may still deserialize successfully.** This is a measured
  finding (Slice 10, Finding 2), preserved deliberately and not weakened by any
  later slice.
- **Hash corroboration must occur before trusting or loading an artifact.** The
  policy runs on resolved metadata *before* `EngineRegistry::get()` is ever
  called, so a hash-faulted model is refused without being handed to the backend.
- **TensorRT header rejection does not replace SHA-256 validation.** A refusal at
  deserialisation time is a late, partial and backend-specific signal. It catches
  some corruption; it is not the gate. The gate is the hash.

Two corollaries worth stating plainly, because both have been misread before:

- **A plan that loads has not been shown to read correctly.** Where `denso --check`
  *does* deep-load an engine it constructs it and validates bindings, shapes and
  class names, but it never calls `infer()` (`src/app/cli/run_headless.cpp`);
  `EngineRegistry::warm_up()` runs the first real inference and discards the
  result. So neither `--check` nor `denso-setup verify: PASS` proves the appliance
  *reads digits correctly*. On a newly commissioned appliance, run one
  known-answer inference through the application before trusting it.
- **`--check` exit 0 does not by itself prove that any plan loaded.** It deep-loads
  the **union** of the engines the database references and those named explicitly
  with `--engine` — deliberately, so a fresh install with an empty database cannot
  be blocked by having no engines, while `denso-setup` can still force the packaged
  set to be checked by passing them. If that union is empty, **zero** engines are
  validated and `--check` can still exit 0 on the readiness verdict alone. Read the
  `check: engines load ok (N validated)` line: `N` is how many plans actually
  loaded.

---

## 6. Central policy and evaluation order

`denso::models::model_compatibility(mode, metadata)` evaluates in a fixed order.
**The first matching failure wins**, and the order is a contract — reordering it
would report the wrong reason for a doubly-faulted model.

```text
model_undeclared
model_unknown_id
model_family_mismatch
model_shape_unsupported
model_classes_mismatch
model_provenance_failed
model_mode_incompatible
model_allowed
```

| Reason code | Meaning |
|---|---|
| `model_undeclared` | no schema-2 generation with a block for the **active backend** declares this row's artifact. Covers an absent/corrupt/schema-1 manifest and a generation that declares only the other backend. |
| `model_unknown_id` | declared, but the `canonical_id` is not in the compiled registry — an identity this build has never been taught. |
| `model_family_mismatch` | the manifest's `family` disagrees with the family the registry records for that `canonical_id`. A file claiming a family it was not registered with cannot borrow that family's privileges. |
| `model_shape_unsupported` | `task != "detect"` or `input_size != 640`. The runtime pipeline is built around one task and one input size. |
| `model_classes_mismatch` | the artifact's ordered class names / class count disagree with the declaration. Class **order** matters: a stale sidecar with the right count and the wrong order makes readings silently wrong rather than failing. |
| `model_provenance_failed` | this backend's artifact hashes failed, or (TensorRT only) `built_for` does not match the measured normalised platform. |
| `model_mode_incompatible` | everything above passed — the artifact is genuinely what it says it is — but its family is not allowed in the **committed** operating mode. |
| `model_allowed` | authorized. |

`CompatibilityResult` defaults to `Verdict::RejectedUnknown`: **a path that
forgets to assign a verdict rejects, it does not permit.**

These strings are a **file format** — they reach `status.json`. Never rename,
never reuse, never renumber; only add.

### How a rejection surfaces

A rejected **attached** model becomes a camera-scoped issue:

```text
ZoneIssue::Kind::ModelCompatibilityRejected
reason        = model_compatibility_rejected      (health::reason_code)
policy_reason = <the exact central-policy reason>
```

The *kind* is deliberately not named after the wrong-mode branch. It covers every
way the policy can reject an attached model, of which wrong-mode is one of seven.
Naming the kind after a single branch would make six other failures describe
themselves as a mode problem and send an operator hunting for a mode switch that
never happened.

**Only a genuine valid wrong-mode model may carry `model_mode_incompatible`.** A
hash mismatch, an undeclared artifact or a class-order fault must never
self-describe as a mode problem — that is precisely why the reason codes are
distinct and why the evaluation order is fixed.

---

## 7. Five enforcement points

All five resolve identity through `models::resolve_model_metadata` and ask the
**same** central policy. None holds a rule of its own.

| # | Path | Entry point | Production caller |
|---|---|---|---|
| 1 | **Warm-up allow-list** | `models::loadable_model_files(mode, metadata)` → `EngineRegistry` `allow_list` | `src/app/ui/startup.cpp` |
| 2 | **Mode-filtered required set** | `detection::attached_model_filenames(db, mode, view, platform)` | `src/app/ui/startup.cpp`; `try_attached_model_filenames` in `src/app/cli/run_headless.cpp` |
| 3 | **Selectable-model list** | `detection::selectable_models(db, mode, view, platform)` | `src/app/ui/camera/dialog/models_page.cpp`, `src/app/ui/camera/wizard_controller.cpp` |
| 4 | **Attachment write + runtime resolution** | `detection::set_camera_models(...)` and `detection::detection_for(...)` | `src/app/ui/camera/wizard_controller.cpp`; `src/app/ui/camera/grid/camera_grid.cpp` |
| 5 | **Boot / integrity evaluation** | `health::evaluate_integrity(db, models_dir, mode, view, platform)` | `src/app/ui/startup.cpp`, `src/app/ui/camera/grid/camera_grid.cpp`, `src/app/cli/run_headless.cpp` |

**1 — Warm-up allow-list.** `EngineRegistry`'s constructor takes the allow-list;
the directory scan in `warm_up()` **skips** every file not in it, so a rejected
model is never deserialized and never runs a warm-up inference. `get()` called
with a filename outside the allow-list **throws** `std::logic_error` — reaching
it means a caller bypassed the policy, and failing loud beats silently
deserializing a rejected plan. There is **no default** allow-list: a forgotten
caller must fail to compile rather than silently authorize (or silently load
nothing by omission).

**2 — Mode-filtered required set.** `required` is the fail-loud set: a member that
is missing or fails to load aborts startup, honouring the engine-only /
no-fallback contract. Because rejected attachments are excluded *upstream*, an
incompatible attachment can never trigger that abort — it is a Degraded camera,
not a dead appliance.

**3 — Selectable-model list.** The unfiltered catalog (`detection::list_models`)
must never be rendered by a selection UI. `selectable_models` returns each allowed
row **paired with its resolved metadata** as one value, in catalog-id order, with
rejected entries removed. Fail-closed: an absent, schema-1 or invalid manifest
declares nothing, so the list comes back **empty** — it never falls back to the
raw catalog.

**4 — Attachment write and runtime resolution.** `set_camera_models` is the domain
chokepoint for binding a model to a camera: every requested model is judged
**before any row changes**, and if *any* member of the set is rejected the
**whole set is refused** and the transaction rolls back — a partial attachment
cannot exist. `detection_for` is the read counterpart: if any attached model is
rejected the camera is inhibited as a whole, `models` comes back **empty** (never
the allowed subset) with `compatibility_rejected` set.

**5 — Boot / integrity evaluation.** `evaluate_integrity` resolves every
**attached** catalog model and records a rejection as a camera-scoped
`ZoneIssue{ModelCompatibilityRejected}` carrying the real `camera_id` and the
policy's verbatim reason. An **unattached** artifact is never escalated into a
camera issue, whatever the policy thinks of it (see §9).

---

## 8. Camera-scoped behaviour

- **An incompatible attachment inhibits only its own camera.** It is never a
  whole-machine blocker. Bricking a four-camera appliance because one camera has
  a bad attachment is the opposite of the per-zone fail-closed contract.
- **Readiness becomes Degraded — exit 10.** Serviceable but not clean.
- **It is not Blocked / exit 78.** `EX_CONFIG` is reserved for whole-machine
  faults (`DbUnopenable`, `SchemaNewer`, `MigrationFailed`, `DbQueryFailed`,
  `ModelsDirUnreadable`, `ManifestCorrupt`).
- **No `DetectionProcessor` is created for the affected camera.** In
  `CameraGrid::start_one`, a `compatibility_rejected` camera is logged, shown
  Offline, and **not started** — it is never demoted to an orientation-only
  stream, because a silently-not-reading camera that looks healthy is the exact
  failure this design prevents.
- **The rejected engine is not requested.** `engines_->get()` is never called for
  it, so an incompatible plan is never deserialized and the fail-loud `TrtEngine`
  constructor is never the thing that discovers the problem.
- **Healthy sibling cameras continue.** They stream, infer and report normally.
- **A mixed allowed/rejected attachment set inhibits the entire affected
  camera.** `detection_for` returns an empty model set, not the allowed subset;
  `set_camera_models` refuses the whole write. Running a camera on the surviving
  half of an attachment set the operator did not choose would be a silent change
  of what that camera measures.

The inhibit cause reuses the existing `ZoneCause::ModelUnavailable` (`1u << 1`).
**No new `ZoneCause` bit was added**: to the camera, a model the policy rejected
and a model missing from disk are the same thing — it has no usable model. The
bitmask is a file format; the distinct diagnosis survives in the issue's reason
code and detail.

---

## 9. Idle wrong-mode artifacts (the locked rule)

A **declared, valid, provenance-clean and unattached** wrong-mode artifact is a
**normal state**, not a fault. It:

- **is skipped by warm-up** — the scan in `EngineRegistry::warm_up()` passes over
  it;
- **is never deserialized** — no `get()`, no `TrtEngine` construction, no
  inference;
- **is absent from the fail-loud required set** — it cannot abort startup;
- **creates no camera issue** — `evaluate_integrity` only judges *attached*
  models;
- **leaves the appliance Ready / exit 0**;
- **may emit only one informational line**, once per skipped file:
  `[warmup] skipping <filename> (not permitted by the compatibility policy in the
  current mode)`.

  Note precisely what that line contains: the **directory-entry name** from the
  models-directory scan, logged verbatim (`qInfo() << name`) — **no sanitizer is
  applied here**. It is not a database column and not a URL, so it cannot carry a
  camera credential the way a hand-edited catalog row could; that is why the
  operator-visible refusal paths, which *do* read database-controlled filenames,
  reduce them through `models::diagnostic_filename` while this one does not. If
  you ever route a DB-controlled or operator-supplied string into this log line,
  it must be reduced first.

**A `digit_reader` appliance may therefore carry all three model pairs while
loading only `digitv3`.** That is the intended fully-provisioned state of a
Release-B appliance: the package seeds `digitv3`, `float-small` and `float-big`,
and a `digit_reader` box warms exactly one of them and reports Ready.

This was proven on hardware without `strace`: every model file's atime was pinned
to 2020-01-01 before a `--check` run (ext4 `relatime` updates an atime older than
mtime on any read). Afterwards `digitv3.engine` and `digitv3.names.json` had moved
to the run time — the positive control, on the same filesystem — while all four
Float files were **still 2020-01-01**.

State what that proves precisely: **their contents were never read.** Deserializing
a TensorRT plan necessarily reads it, so zero Float `TrtEngine` constructions and
zero Float inferences follow. An unchanged atime does not exclude a metadata-only
touch (`stat`, `O_PATH`, open-without-read), so do not upgrade the claim to "never
opened".

---

## 10. Selection and hidden attachments

- **Selection follows the committed database mode.** The mode passed to every
  enforcement call is `denso::mode::load(db)` — the mode the appliance is actually
  running.
- **Temporary selector state does not authorize models.** The settings-page mode
  selector may hold a choice the operator has not applied. Attaching against an
  unconfirmed mode would let the wizard authorize a model the appliance is not
  running, so it is never consulted.
- **Rejected models are absent from the selectable list** — absent, not greyed
  out and not annotated. A model the appliance would refuse to load must not be
  offered.
- **A hidden attached rejected model is not silently detached.** Because it is
  absent from the Models page, the page's selection set cannot carry it, so a
  plain save would drop it — converting a diagnosable, inhibited
  `ModelCompatibilityRejected` camera into an apparently-healthy camera with no
  model that silently reads nothing.
- **The wizard requires explicit operator consent before removing it.**
  `CameraWizardController` recomputes the hidden set from the same three inputs
  the page was given (never from page state) and presents a named confirmation
  listing exactly what will be removed and what the consequence is.
- **Declining writes nothing.** The wizard stays on the Models step and no
  database change occurs.

It cannot simply be preserved either: `set_camera_models` refuses any set
containing a rejected model, and rightly so. Consent is the only correct
resolution.

---

## 11. Packaging and deployment ordering

The manifest is load-bearing: without it, no model is selectable or loadable.
Packaging cannot deliver it as a side effect of an upgrade — `postinst` is
structural by design, model seeding lives exclusively in `denso-setup configure`,
and `dpkg` places no payload file under `/opt/denso/data`. Shipping enforcement and the manifest
together would install enforcing code onto an appliance with no manifest and
inhibit every camera. Hence two releases:

- **Release A delivered schema-2 identity and contained no Float artifacts.** It
  shipped schema-2 support, a declaration for the *existing* `digitv3` artifacts,
  `seed-manifest`, `verify` reporting that **changes nothing under `models/`**
  (§12 — not globally read-only), and the export tooling — with **no application
  behaviour change**.
- **Gate A had to pass before Release B.** An upgrade rehearsal on the Jetson
  proved an existing appliance actually receives a declaration for its existing
  `digitv3`, that `verify` is non-mutating with respect to `models/`, and that the
  active models directory was free of Float artifacts. No Release-B slice merged
  before it passed.
- **The warm-up firewall existed before Float placement.** `warm_up()` scans the
  models directory and deserializes every runtime artifact it finds, attachment or
  not. A Float engine placed there during Release A would have been loaded and run
  on a Digital Number Reader appliance at every boot. The allow-list shipped
  first.
- **Slice 12 was the first package containing Float artifacts** — after the
  policy, the warm-up firewall, camera-scoped enforcement, UI filtering, native
  engine smoke tests and authorized-camera validation. This ordering is
  machine-enforced by `assert_float_seeding_guarded`
  (`packaging/lib/gen_payload.sh`), which refuses the build if a `float-*` stem is
  approved in `packaging/models.approved` without a real definition of
  `loadable_model_files` **and** a real call to it from `startup.cpp`.
- **The Release-B package carries three TensorRT engine/sidecar pairs** —
  `digitv3`, `float-small`, `float-big` — staged into `/opt/denso/models` with the
  generated schema-2 manifest, `models.approved` and `SHA256SUMS` in
  `/opt/denso/lib`.
- **Package installation is structural.** `postinst` does exactly two things:
  `mkdir -p /opt/denso/data/models /opt/denso/install-state` and
  `chmod 0755 /opt/denso/install-state`. It sets **no ownership** (deliberately —
  `denso-setup configure` owns that) and touches **no model artifact**.
  Installing or upgrading the `.deb` never seeds, replaces or removes an operator
  artifact, and the `.deb` payload contains no file under `/opt/denso/data`.
  State the guarantee that way: it is *artifact* non-interference, not "the
  directory is never written" — installation does create it.
- **A fresh `denso-setup configure` seeds the approved artifacts** into
  `/opt/denso/data/models` as the target user, pair-wise, via
  `seed_decision_pair`.
- **`seed-manifest` does not install models.** It places `manifest.json` and
  nothing else (§12).

Layout relevant to this document:

| Path | Owner | Contents |
|---|---|---|
| `/opt/denso/models/` | package | the approved `<stem>.engine` + `<stem>.names.json` pairs |
| `/opt/denso/lib/manifest.json` | package | the generated schema-2 manifest (the template) |
| `/opt/denso/lib/models.approved` | package | the approved pair hashes + build recipes |
| `/opt/denso/lib/SHA256SUMS` | package | checksums over the model payload |
| `/opt/denso/data/models/` | operator | the artifacts the app actually loads |
| `/opt/denso/data/models/manifest.json` | operator | **where the app reads the manifest** |

---

## 12. `seed-manifest` and `verify`

### `denso-setup seed-manifest`

- **Explicit.** Operator-invoked only — never called by `postinst`, `configure`,
  `verify` or any maintainer script.
- **Manifest-only.** It touches `manifest.json` and nothing else: it does not run
  configure, change ownership, autostart, autologin or the recorded user.
- **Atomic.** `install_manifest_atomic` copies to a unique `mktemp` file beside
  the destination, pins it 0644, `sync`s, then publishes with **`ln`** — an
  atomic create-if-absent hard link that *fails* if the destination already
  exists (rc 4, which the caller re-compares). It never overwrites or truncates.
- **Idempotent.** Running it twice is a no-op on the second run.
- **Accepts canonical equivalence.** A reformatted-but-equivalent manifest
  (whitespace, key order) is "already current". The comparison
  (`manifests_equivalent`, `packaging/lib/policy.sh`) is stricter than Python's
  `==`: duplicate object keys are rejected, NaN/Infinity are rejected, and types
  are compared identically (`True` is not `1`, `1` is not `1.0`).
- **Refuses a differing manifest.** A present-but-different, unreadable or
  malformed target is refused, never overwritten.
- **Refuses missing or mismatched artifacts.** If a packaged model is absent from
  the data directory, differs from the approved pair, or the data directory holds
  an engine the packaged manifest would not describe, it refuses — it will not
  seed a manifest that misdescribes what is on disk.
- **Never replaces models.** Adopting a differing artifact is
  `denso-setup replace-model <stem>`, a separate, explicit command.

### `denso-setup verify`

- **Observes manifest and model state.** It reports: no manifest → `NOTE`;
  matching pair → `ok`; differing pair → `WARN`; mismatch → `FAIL`.
- **Does not create or rewrite the manifest.** Repair is `seed-manifest`.
- **Has no `--repair`.** That entry point does not exist and must not be added.
- **Changes nothing under `models/`.**
- **Retains pre-existing database-backup behaviour outside `models/`.**

> **`verify` is not globally read-only.** It deliberately writes a database backup
> directory, which predates this work and stays. The testable, enforced rule is
> narrower and exact: **`verify` changes nothing under `models/`.** Do not
> document or rely on it as a no-mutation command.

`verify` also requires root (`need_root`): it chowns throwaway directories and
runs `runuser`. A non-sudo `verify` cannot work.

---

## 13. Backend portability

| | TensorRT (Jetson) | ONNX Runtime (Windows / MSYS2 dev) |
|---|---|---|
| Artifact | `<stem>.engine` | `<stem>.onnx` |
| Class names | `<stem>.names.json` sidecar | the ONNX file's own `names` metadata |
| Platform check | measured TRT / CUDA / SM vs `built_for` | **none** — `built_for` is not read |
| Manifest block | `runtime.tensorrt` | `runtime.onnxruntime` |

- **TensorRT** uses the `.engine`, its `.names.json` sidecar, and the measured
  normalised TRT/CUDA/SM triple.
- **ONNX Runtime** uses `.onnx` metadata and **ignores TensorRT `built_for`
  entirely**. This is structural, not conventional: `built_for` exists only inside
  `runtime.tensorrt`, which the ONNX Runtime resolution path never reads. A
  TensorRT platform mismatch therefore **cannot** reject a Windows deployment.
- **The central compatibility policy is backend-independent.**
  `model_compatibility` never sees a `Backend` at all — the backend is resolved
  away by `resolve_model_metadata` before the policy is reached, so the policy is
  a pure function of mode + identity.
- **Backend selection belongs to `ManifestView`** (§3), fixed at construction.
- **`denso_core` remains free of TensorRT, CUDA, ONNX Runtime, OpenCV and Qt
  Widgets dependencies.** `compatibility.{h,cpp}` is pure (mode + STL);
  `model_identity.{h,cpp}` may touch Qt Core, the manifest/catalog abstractions
  and file hashing, and nothing more.

**Jetson-only validation does not narrow this contract.** Slice 13's acceptance
testing ran on the Jetson because that is the deployment target and the only place
`sm_87` and real TensorRT exist. The committed ONNX Runtime architecture is
unchanged, remains compiled and remains unit-tested through the `ManifestView`
test seam, which exercises **both** backends' resolution off-host.

---

## 14. Adding a model — checklist

Adding a model is a **code change with a review and tests**, not a data change.

1. **Train / export outside CMake.** Model production is not a build step
   (`tools/export_float_onnx.py` is the pinned exporter).
2. **Record full provenance** — source `.pt` and its hash, ONNX and its hash,
   opset, training/export Ultralytics versions, batch, dynamic, nms, precision,
   JetPack baseline, and the verbatim export commands.
3. **Generate the names sidecar** (`tools/gen_names_sidecar.py`) — class **order**
   is part of the artifact's identity.
4. **Generate a schema-2 descriptor and generation**
   (`packaging/manifest/<stem>.descriptor.json` → `tools/gen_model_manifest.py`).
   Hashes are **measured, never copied**; provenance is **extracted, never
   invented** — a value supplied by the descriptor requires an explicit
   `provenance_evidence` citation naming where it came from.
5. **Add the canonical id / family / mode rule in
   `src/core/models/compatibility.cpp`.** A new family also needs a `kFamilyModes`
   entry; the compile-time invariants will refuse a malformed registry.
6. **Add policy matrix and rejection tests** — the full (mode × model) matrix plus
   every rejection branch (`tests/test_model_compatibility.cpp`).
7. **Add backend-resolution tests** — both backends, through the `ManifestView`
   test seam (`tests/test_model_identity.cpp`).
8. **Add package approval only after enforcement exists.** Approve the pair in
   `packaging/models.approved` only once the policy, the allow-list and its real
   caller are in the tree. For a `float-*` stem this is machine-enforced.
9. **Run the gates**: the warm-up firewall check, the camera-scoped enforcement
   tests, the UI filtering tests, the native engine smoke test on the Jetson, and
   `tests/packaging/run.sh` + `tests/manual/repro_build.sh`.
10. **Obtain Codex review** — per the standing collaboration cadence on this repo.
11. **Never infer identity from filenames or class names.** Renaming a file must
    not change what it is allowed to do, and two models with the same class
    signature are not the same model.

**Editing only the manifest cannot authorize a new family.** A manifest can
declare `family: "something_new"`, and the policy will reject it with
`model_unknown_id` (unregistered id) or `model_family_mismatch` (registered id,
contradicted family). Authorization requires the compiled registry.

---

## 15. Current limitations

Stated explicitly, because the packaged Float artifacts make it easy to assume
more exists than does:

- **`ball_leveler` may be persisted.** `mode.target` accepts it and
  `switch_mode` will commit it (non-destructively — see **Operating modes** in
  `docs/ARCHITECTURE.md`). Since schema v14 a Ball Leveler binding + calibration
  also has a durable home, `ball_level_calibration`; the operator-facing setup
  surface is still guarded.
- **The repository policy may return Float models for that mode.**
  `selectable_models`, `attached_model_filenames` and `model_compatibility` will
  all authorize `float-small` / `float-big` under `ball_leveler` — the policy layer
  is complete.
- **Production camera setup remains unavailable in `ball_leveler`.** The appliance
  lands on an explicit "not available in this release" surface;
  `mode_setup_required` stays permanently `true` and the top-bar Camera button is
  disabled.
- **No production Leveler `CameraStream`, `DetectionProcessor`, `ZoneHealth`
  wiring or reporter is constructed.**
- **No ball position, percentage, calibration or final level result exists.** No
  Floating Ball algorithm is implemented anywhere in this repository.

What is complete is the **compatibility contract**: identity, corroboration,
authorization, and five enforcement paths that all ask one policy. What is not
implemented is the Floating Ball **application feature**. Unlocking it requires a
new approved design and plan — not a configuration change.
