# Model / Operating-Mode Compatibility — Locked Model Matrix and Declared Model Identity

Status: **APPROVED — Revision 3.**
Date: 2026-07-22
Revision: **3** (product-owner decisions locked — see §19)
Depends on:
- `2026-07-21-operating-modes-switch-and-reset-design.md` (APPROVED, Revision 3b,
  `c7c7a6b`) — the `TargetMode` domain and the `ball_leveler` unavailability rule,
  **both of which this design leaves intact**;
- `2026-07-20-model-lifecycle-24-7-design.md` — `manifest.json` as the artifact
  identity authority, and per-zone fail-closed;
- `2026-07-20-zone-readiness-inhibition-design.md` — `IntegrityVerdict`,
  `ZoneIssue`, `ZoneHealth`, the `0/10/78` readiness contract.

---

## 1. Scope

The appliance is gaining a second family of detection models for the Floating
Ball Leveler. Nothing today prevents a Digital Number Reader camera from being
attached to a Floating Ball model, or the reverse: `model_sync()` catalogs every
artifact it finds in the models directory, `ModelsPage` lists the whole catalog,
`set_camera_models` writes whatever it is given, and `detection_for` resolves
whatever is stored. A wrong attachment does not fail — it produces *confident,
silently wrong readings*, which is the worst failure this appliance can have.

**In scope:**

- An explicit, declared **canonical model identity** carried by the model
  manifest, replacing implicit identification by filename or by inference.
- **One central pure compatibility policy** — `model_compatibility(mode, model)` —
  consumed by every enforcement path. No path re-derives the rules.
- **Fail-closed classification** of unknown, undeclared and mismatched models.
- **Camera-scoped enforcement**: an incompatible attachment (including one
  introduced by a hand-edited or restored database) inhibits exactly that camera
  and never constructs its `DetectionProcessor` or loads its engine.
- The **UI filtering seam**, wired for `digit_reader` now and *ready* for a future
  Leveler UI, without exposing any Leveler setup UI in this release.
- The **artifact pipeline** for `float-small` and `float-big`: `.pt` → `.onnx` on
  the Windows dev machine, `.onnx` → `.engine` on Jetson `192.168.1.15`,
  `.names.json` sidecar generation, and full recorded provenance.
- **Headless** engine validation and authorized-camera frame validation on `.15`.

**Explicitly out of scope:**

- **The Floating Ball position / percentage / calibration algorithm.** Nothing in
  this design computes, reports, stores or displays a ball level.
- **Unlocking the Ball Leveler production UI** — see §6.2 and §13.
- Any change to the operating-mode switch-and-reset transaction, the `0/10/78`
  readiness contract, the brazing reporter, or the zone namespace.
- Any DB schema migration. See §14.
- Retraining, evaluating or improving any model's accuracy.

### 1.1 What this design does NOT claim

- It does not claim the generated TensorRT engines are portable. A plan is
  compiled for one TensorRT version and one GPU architecture; §8.6 states the
  qualification boundary and nothing wider.
- It does not claim the Floating Ball Leveler works, or is closer to working.
  Two engines that deserialize and accept frames are not a leveler.
- It does not claim an inference result from `float-small`/`float-big` is
  *correct*. §11 validates that frames flow and boxes decode, not that the boxes
  are right.
- It does not claim compatibility can be enforced by the database. Foreign keys
  are inert (modes spec §3.3); enforcement is code with tests, and a restored DB
  is caught at read time, not prevented at write time.
- It does not claim an appliance whose models directory lacks a schema-2 manifest
  keeps working unchanged. It does not — see **R1**, the central risk of this
  design, and the mandatory deployment step in §8.7.

---

## 2. Locked model matrix

| `TargetMode` | Allowed canonical IDs | Rejected |
|---|---|---|
| `digit_reader` | `digitv3` | `float-small`, `float-big`, anything unknown |
| `ball_leveler` | `float-small`, `float-big` | `digitv3`, anything unknown |

There is no model allowed in both modes, and no mode with an empty allow-list.
The matrix is **total**: for every (mode, model) pair the policy returns a
verdict; there is no "don't know, allow it" branch.

### 2.1 Model families

| Family token | Canonical IDs | Allowed modes | Meaning |
|---|---|---|---|
| `digit_numeric` | `digitv3` | `digit_reader` | reads 7-segment digits; classes are numerals |
| `float_ball` | `float-small`, `float-big` | `ball_leveler` | locates a floating ball |

Family → allowed-modes is **compiled into the policy** (§4.3). Canonical ID →
family is likewise compiled in. The manifest declares which of these a given
artifact *is*; it does not get to invent new ones.

---

## 3. Model identity — declared, not inferred

### 3.1 Why the obvious candidates were rejected

| Candidate | Why rejected |
|---|---|
| **Filename / filename substring** | `models/` is entirely git-ignored (CLAUDE.md), filenames are operator-chosen, and `model_sync` derives the catalog `name` from the filename stem. A file renamed `digitv3.engine` would acquire an identity it does not have. `float-small`/`float-big` share the `float-` prefix by convention only — convention is not a contract. |
| **Display label** (`model.name`) | Editable, cosmetic, and derived from the filename. |
| **Ordered class-name signature alone** | Genuinely useful, and it *would* separate today's three models (`["0"…"9"]` / `["Small"]` / `["Big"]`). But it is a coincidence of the current set: a future model may legitimately reuse a class vocabulary while belonging to a different family, and a signature carries no statement of intent. Rejected as the *sole* identity; retained as mandatory **corroboration** (§3.4). |
| **Artifact SHA-256 alone** | Precise but meaningless — it says *which bytes*, never *what they are for*, and it changes on every rebuild of the same model. Retained as corroboration. |

### 3.2 The authority: the model manifest, at schema 2

`src/core/models/manifest.{h,cpp}` already exists and is already described by
the model-lifecycle spec as *"the SOLE artifact-identity authority (SHA-bound)"*.
This design extends it rather than introducing a second, competing file.

Current `schema: 1` generation fields: `name`, `engine`, `engine_sha256`,
`sidecar`, `sidecar_sha256`, `class_names`, `built_for{trt,cuda,sm}`,
`installed_utc`, `state`.

**Schema 2 adds a mandatory compatibility declaration, a per-backend runtime
artifact block, and a provenance block:**

```json
{
  "schema": 2,
  "generations": [
    {
      "name": "digitv3",
      "canonical_id": "digitv3",
      "family": "digit_numeric",
      "task": "detect",
      "input_size": 640,
      "class_names": ["0","1","2","3","4","5","6","7","8","9"],
      "class_count": 10,
      "runtime": {
        "onnxruntime": {
          "model": "digitv3.onnx",
          "model_sha256": "<64 lowercase hex>",
          "class_metadata_source": "onnx_metadata_names"
        },
        "tensorrt": {
          "engine": "digitv3.engine",
          "engine_sha256": "<64 lowercase hex>",
          "sidecar": "digitv3.names.json",
          "sidecar_sha256": "<64 lowercase hex>",
          "class_metadata_source": "names_sidecar",
          "built_for": { "trt": "10.3", "cuda": "12.6", "sm": "87" }
        }
      },
      "provenance": {
        "source_pt": "digitv3.pt",
        "source_pt_sha256": "<64 lowercase hex>",
        "onnx": "digitv3.onnx",
        "onnx_sha256": "<64 lowercase hex>",
        "onnx_opset": 12,
        "training_ultralytics": "8.4.33",
        "export_ultralytics": "8.4.33",
        "batch": 1,
        "dynamic": false,
        "nms": false,
        "precision": "fp16",
        "jetpack": "6.2",
        "export_onnx_command": "<verbatim>",
        "export_engine_command": "<verbatim>"
      },
      "installed_utc": "2026-07-22T00:00:00Z",
      "state": "installed"
    }
  ]
}
```

Required canonical IDs for this scope: **`digitv3`, `float-small`, `float-big`.**

**The manifest deliberately does NOT carry `allowed_modes`.** Allowed modes are
*resolved* from `canonical_id` → `family` → modes through the compiled registry
(§4.3). Storing them in the manifest as well would put the matrix in two places:
the second copy could never grant anything (the compiled table decides), so its
only possible effect is drift — a correct artifact rendered unusable by a stale
declaration. One authority, no redundant assertion.

The manifest is **deployed alongside the model artifacts** — it lives in the
models directory next to the runtime artifacts, is seeded by
`denso-setup seed-manifest` (§8.8), and is covered by the packaging approval
flow (§8.7).

#### 3.2.1 One identity, two runtimes — locked

A logical model generation has **one** `canonical_id` on every platform. What
differs is the artifact that runtime actually loads. The `runtime` block makes
that explicit instead of leaving the Windows case implied.

| | Windows / MSYS2 (`OrtEngine`) | Jetson (`TrtEngine`) |
|---|---|---|
| Artifact verified | `runtime.onnxruntime.model` + `model_sha256` | `runtime.tensorrt.engine` + `engine_sha256` **and** `sidecar` + `sidecar_sha256` |
| Class names sourced from | ONNX `names` metadata | the `.names.json` sidecar |
| `built_for` checked | **never** | yes — TensorRT / CUDA / `sm` against the running device |

**Locked rules:**

1. Each platform verifies **only its own** runtime block. A Windows appliance
   never hashes an `.engine`; a Jetson never hashes an `.onnx`.
2. **`built_for` lives inside `runtime.tensorrt` and is evaluated only there.**
   A TensorRT platform mismatch can never reject an ONNX Runtime deployment —
   structurally impossible, because the ORT resolution path does not read the
   field. This is the whole reason `built_for` moved out of the generation root.
3. The same `canonical_id` and `family` govern both platforms, so the
   compatibility matrix is platform-independent and stated once.
4. **The active backend is an immutable property of `ManifestView`**, fixed at
   construction by the one `#ifdef _WIN32` split the tree already uses, and
   thereafter carried with the view. `resolve_model_metadata` reads it from the
   view and returns metadata whose artifact identity, hashes and class-name
   source are already backend-appropriate. This is deliberate: `denso_core`
   cannot and must not infer the application's backend, and threading a
   `Backend` parameter through `evaluate_integrity`, `set_camera_models`,
   `detection_for` and `selectable_models` would create four more places to get
   it wrong. Every one of those already takes the view. **`model_compatibility`
   never sees a backend at all** — it receives resolved data and stays a pure
   function of mode + identity.
5. A generation with **no block for the running backend** is not available on
   that platform: it resolves `declared == false` → `RejectedUnknown` /
   `model_undeclared`. A generation whose block is present but whose artifact is
   missing, unreadable or hash-mismatched → `RejectedProvenance` /
   `model_provenance_failed`. Both fail closed.
6. At least one runtime block must be present, or the generation is structurally
   invalid (§3.5).

#### 3.2.2 Schema-1 compatibility, and the two consumers nesting would break

Schema-1 generations keep their root-level `engine`, `sidecar`, `*_sha256` and
`built_for` fields and are read exactly as today. But **two existing consumers
read those root fields unconditionally**, and nesting them for schema 2 would
silently feed both an empty string. Neither is optional to fix, and both belong
to Slice 1 — the slice that introduces the nesting.

**(a) `evaluate_integrity`'s unmanifested check.** It builds its declared set
from the root `g.engine` alone (`integrity.cpp:140`) and then reports every
on-disk `.engine`/`.onnx` not in that set as `EnginesUnmanifested` → **Degraded**
(`integrity.cpp:148-151`). Under schema 2 the root field is empty, so the
appliance's own correctly-declared `digitv3.engine` would be reported
unmanifested and Release A — whose defining property is that it changes no
behaviour — would degrade every appliance it touched.

> **Locked rule.** The `manifested` set is the union of **every runtime artifact
> filename declared in either block**, across all generations, **irrespective of
> the committed mode and irrespective of the active backend.** A file is
> "manifested" because it is *described*, not because it is *usable here*.
> Compatibility rejection applies to **attachments**, never to this bookkeeping.

That keeps both genuine faults intact: a file nobody declared is still
`EnginesUnmanifested`, and an attached-but-missing engine is still
`EngineMissing`.

**(b) `--migrate-model`'s coordinator.** `find_by_engine` matches the root
`engine` for schema 1 and `runtime.tensorrt.engine` for schema 2 — but after
matching, `migrate_coordinator.cpp` dereferences `gen->engine` (`:83`),
`gen->sidecar` (`:85`), `gen->engine_sha256` / `gen->sidecar_sha256` (`:91-99`)
and `gen->class_names` (`:105`). Against a schema-2 generation those would be
empty, so the path-escape guard and both hash comparisons would compare against
nothing.

> **Locked rule.** `ModelGeneration` exposes **schema-aware accessors** —
> `tensorrt_engine()`, `tensorrt_sidecar()`, `tensorrt_engine_sha256()`,
> `tensorrt_sidecar_sha256()`, `built_for_trt/cuda/sm()` — returning the root
> field for schema 1 and the nested one for schema 2. Every existing consumer
> switches to the accessor; **no consumer reads the raw field.** This keeps the
> coordinator's diff to a handful of call sites rather than duplicating
> schema logic across the tree.

### 3.3 Schema-1 manifests, and manifest absence

`parse_manifest` currently rejects any `schema != 1`. Schema 2 must be accepted
**without changing schema-1 behaviour for the paths that already consume it**
(`--migrate-model`, `evaluate_integrity`). Therefore:

| Manifest state | Structural verdict | Compatibility verdict |
|---|---|---|
| Absent | unchanged: not `ManifestCorrupt`; on-disk engines reported `EnginesUnmanifested` (Degraded, never blocking) | every model **Undeclared → Unknown → rejected** |
| `schema: 1`, valid | unchanged: parses and validates exactly as today | every model **Undeclared → Unknown → rejected** |
| `schema: 2`, valid | parses; the new fields validate per §3.5 | per-generation declaration is used |
| Malformed / invalid at either schema | `ManifestCorrupt` global blocker (unchanged) | not reached |

A schema-1 or absent manifest is **not** a corrupt manifest and must not become
one — that would turn today's production appliance from *degraded* into
*blocked*, which this design explicitly refuses to do. It does mean an appliance
without a schema-2 manifest has no selectable and no runnable models. That is
the intended fail-closed outcome and the reason §8.7 makes manifest deployment a
hard, gated prerequisite rather than an afterthought. See **R1**.

### 3.4 Corroboration — the artifact must match its declaration

A declaration is authoritative for *identity* but is not trusted blindly about
*content*. Before a model is usable, the loaded artifact's own metadata must
agree with the manifest entry:

| Check | Source of truth for the artifact | Applies on | Failure verdict |
|---|---|---|---|
| ordered class names, exactly | `model.class_names` in the catalog — from the `.names.json` sidecar on Jetson, from ONNX metadata on Windows | both | `RejectedMetadataMismatch` |
| class count | `class_names.size()` on both sides | both | `RejectedMetadataMismatch` |
| `class_count` field equals `class_names.size()` | within the manifest entry itself | both | `ManifestCorrupt` (structural — §3.5) |
| `runtime.onnxruntime.model_sha256` | `models::file_sha256` | **Windows only** | `RejectedProvenance` |
| `runtime.tensorrt.engine_sha256`, `sidecar_sha256` | `models::file_sha256` | **Jetson only** | `RejectedProvenance` |
| `runtime.tensorrt.built_for` matches the device | TensorRT / CUDA / `sm` of the running device | **Jetson only** | `RejectedProvenance` |

`model.class_names` is used **only** for this corroboration. It is never used to
*determine* which model something is.

Hash verification is I/O and must not run per frame: it runs at boot integrity
evaluation and at `--check`, and its result is what the runtime consults. This
mirrors the existing `--migrate-model` behaviour and inherits its accepted
TOCTOU limitation (model-lifecycle memo; `models_dir` is root-owned on a
single-operator appliance).

### 3.5 Structural validation added at schema 2

`validate_manifest` gains, for schema-2 generations only:

- `canonical_id` non-empty, and a safe token (`[A-Za-z0-9._-]+`, no path
  separators — the same discipline `is_basename` already applies to filenames);
- `canonical_id` unique across generations;
- `family` non-empty and a safe token;
- `task` non-empty; `input_size` a positive integer;
- `runtime` present, an object, with **at least one** of `onnxruntime` /
  `tensorrt`, and no unrecognised backend key;
- `runtime.onnxruntime`, when present: `model` a safe basename ending `.onnx`;
  `model_sha256` 64 lowercase hex; `class_metadata_source` **exactly
  `"onnx_metadata_names"`**;
- `runtime.tensorrt`, when present: `engine` and `sidecar` safe basenames with a
  matching stem (the existing schema-1 rule, applied inside the block);
  `engine_sha256` and `sidecar_sha256` 64 lowercase hex; `class_metadata_source`
  **exactly `"names_sidecar"`**; `built_for.trt`, `.cuda`, `.sm` all non-empty;

  (`class_metadata_source` is a **closed vocabulary**, not free text. A merely
  non-empty check would accept a typo that silently described the wrong source
  while every other field validated.)
- `built_for` must **not** appear at the generation root in a schema-2 entry —
  it belongs to the TensorRT block, and a root copy would resurrect the
  cross-platform rejection §3.2.1 rule 2 exists to prevent;
- `class_count` present, positive, and **equal to `class_names.size()`**;
- `provenance` present as an object with non-empty `source_pt_sha256`,
  `onnx_sha256`, `export_ultralytics`, `precision`, and a non-empty
  `export_engine_command`.

Every existing schema-1 rule is retained unchanged.

---

## 4. The central compatibility policy

### 4.1 Location and shape

New file **`src/core/models/compatibility.{h,cpp}`** in `denso_core`. Pure: no
Qt Widgets, no OpenCV, no inference backend, no filesystem, no SQL. It takes
data and returns a verdict, so it is exhaustively unit-testable without a GPU,
a display or a database.

```cpp
#include "mode/mode.h"

namespace denso::models {

/// What the appliance knows about one model — resolved from its manifest entry
/// FOR THE ACTIVE BACKEND and corroborated against the catalog row. Never
/// constructed from a filename. The backend is already resolved away by
/// resolve_model_metadata: the policy below is platform-independent.
struct ModelMetadata {
    std::string canonical_id;                 // "" when undeclared
    std::string family;                       // "" when undeclared
    std::string task;                         // "detect"
    int         input_size   = 0;             // 640
    int         class_count  = 0;
    std::vector<std::string> class_names;     // ordered, from the artifact
    std::string filename;                     // the ACTIVE backend's artifact —
                                              // .onnx on Windows, .engine on
                                              // Jetson. Diagnostics + the
                                              // warm-up allow-list key.
    bool        declared          = false;    // a schema-2 entry WITH a block
                                              // for the active backend exists
    bool        artifact_matches  = false;    // §3.4 class corroboration passed
    bool        provenance_ok     = false;    // this backend's hashes (+ built_for
                                              // on Jetson only) passed
};

enum class Verdict {
    Allowed,
    RejectedWrongMode,          // declared, known family, not allowed in this mode
    RejectedUnknown,            // undeclared, or an unrecognised canonical_id/family
    RejectedMetadataMismatch,   // artifact disagrees with its declaration
    RejectedProvenance,         // hash / platform mismatch
};

/// Every model filename the appliance may load in `mode`, given the catalog and
/// the manifest. The ONLY set EngineRegistry may ever see (§7.0).
std::set<std::string> loadable_model_files(denso::mode::TargetMode mode,
                                           const std::vector<ModelMetadata>&);

struct CompatibilityResult {
    Verdict     verdict = Verdict::RejectedUnknown;   // fail-closed default
    std::string reason_code;   // stable string; a FILE FORMAT (status.json)
    std::string detail;        // camera/model identity — NEVER a credential (§12)
    bool allowed() const { return verdict == Verdict::Allowed; }
};

/// THE policy. Every enforcement path calls exactly this.
CompatibilityResult model_compatibility(denso::mode::TargetMode mode,
                                        const ModelMetadata& model);

} // namespace denso::models
```

The default-constructed `CompatibilityResult` is a rejection. A path that
forgets to assign a verdict denies, it does not permit.

### 4.2 Evaluation order (fail-closed at every step)

1. `!model.declared` → `RejectedUnknown` / `model_undeclared`.
2. `canonical_id` not in the compiled registry → `RejectedUnknown` /
   `model_unknown_id`.
3. `family` ≠ the registry's family for that `canonical_id` → `RejectedUnknown` /
   `model_family_mismatch`.
4. `task` ≠ `"detect"` or `input_size` ≠ 640 → `RejectedMetadataMismatch` /
   `model_shape_unsupported`.
5. `!model.artifact_matches` → `RejectedMetadataMismatch` /
   `model_classes_mismatch`.
6. `!model.provenance_ok` → `RejectedProvenance` / `model_provenance_failed`.
7. `mode` not in the compiled family→modes set → `RejectedWrongMode` /
   `model_mode_incompatible`.
8. otherwise → `Allowed` / `model_allowed`.

### 4.3 The compiled registry — why the matrix is not manifest-controlled

```
canonical_id  → family          → allowed modes
digitv3       → digit_numeric   → { digit_reader }
float-small   → float_ball      → { ball_leveler }
float-big     → float_ball      → { ball_leveler }
```

The manifest is a file in an operator-writable directory. If it were the sole
authority on *what a model may do*, the safety property would be one text edit
deep. So the manifest declares **what an artifact is** (identity, contents,
provenance) and the compiled table alone decides **what that thing may do**. The
manifest cannot widen a model's privileges because it never states them (§3.2).

This is the literal sense in which the matrix lives in exactly one place: the
table above is the only expression of mode↔model authorization anywhere in the
tree, and every path reaches it through `model_compatibility`. Acceptance
criterion 14 is a grep-able assertion of precisely that.

Adding a model family is therefore a code change with a review and a test — the
appropriate weight for a decision that governs whether an appliance can read the
wrong thing confidently.

---

## 5. Behaviour for unknown and unclassified models

Every one of the following is `Rejected*` in **both** modes:

- a model with no manifest at all, or with only a schema-1 manifest;
- a model file present on disk with no generation entry;
- a generation whose `canonical_id` is not one of the three registered;
- a generation whose declared `family` disagrees with the compiled registry's
  family for its `canonical_id`;
- a generation whose `class_names` or count disagree with the loaded artifact;
- a generation with no `runtime` block for the active backend (§3.2.1 rule 5);
- a generation whose active-backend artifact hash does not match, or — on Jetson
  only — whose `runtime.tensorrt.built_for` does not match the running device.

A rejected model:

- **does not appear** in any model-selection list (§6);
- **cannot be attached** — the write is refused (§7.1);
- **is never loaded**: it is absent from the warm-up allow-list and from the
  fail-loud required set, so `EngineRegistry::get()` is never called for it and
  no incompatible plan is ever deserialized — including when the file merely
  sits in the models directory unattached (§7.0);
- **inhibits only its camera** at boot, *if it is attached to one* (§7.3).

`EnginesUnmanifested` keeps its current meaning and stays **Degraded, never
blocking** (modes spec §1 out-of-scope list). Rejection is expressed through the
new camera-scoped issue, not by escalating that one.

### 5.1 The known-but-wrong-mode artifact is a NORMAL state, not a fault

A correctly provisioned appliance may carry all three model families on disk
while running one committed mode — indeed, after this work ships, that is the
expected shape of a Denso appliance. A `float-small.engine` sitting in the models
directory of a `digit_reader` appliance, **declared, valid, provenance-clean, and
attached to no camera**, is not a problem to be reported as damage.

Such an artifact:

- is **skipped by warm-up** and never deserialized (§7.0);
- **never enters the fail-loud required set**;
- produces **at most one redaction-safe informational line** per boot, and
  appears in verbose `--check` and `denso-setup verify` output so an operator can
  see it is deliberately idle;
- **must NOT make the appliance Degraded.** Readiness stays `Ready`, exit code
  `0`. Reporting an ordinary, intended installation state as degraded would
  train operators to ignore the field that exists to tell them something is
  wrong.

This is precisely scoped. It applies only to an artifact that is **declared,
valid and unattached**, and that the policy rejects **solely** because of the
committed mode (`model_mode_incompatible`). Everything else keeps its existing
or new diagnostic behaviour:

| On-disk artifact | Attached? | Verdict |
|---|---|---|
| declared, valid, wrong mode | no | **Ready** + informational line (§5.1) |
| declared, valid, wrong mode | yes | camera-scoped issue, **Degraded (10)** (§7.3) |
| unmanifested | either | `EnginesUnmanifested` — **Degraded**, unchanged |
| corrupt / hash-mismatched / undeclared | no | informational; escalates only if something depends on it |
| corrupt / hash-mismatched / undeclared | yes | camera-scoped issue, **Degraded (10)** |
| manifest itself malformed | — | `ManifestCorrupt` — **Blocked (78)**, unchanged |

---

## 6. UI filtering

### 6.1 The seam

`ModelsPage::load_for` today calls `detection::list_models(db_)` and renders the
whole catalog. It changes to call a new mode-aware accessor:

```cpp
// src/core/detection/repo.h

/// A catalog row and its resolved identity, kept as ONE value. Deliberately not
/// a vector of rows plus a parallel metadata lookup: two containers indexed in
/// step is a bug waiting for the first filter or sort, and every consumer needs
/// both halves together.
struct SelectableModel {
    DetectionModel               row;
    denso::models::ModelMetadata metadata;
};

/// The catalog filtered to the models the policy allows in `mode`, with each
/// model's resolved metadata. The ONLY list any selection UI may render.
std::vector<SelectableModel>
selectable_models(const QSqlDatabase& db, denso::mode::TargetMode mode,
                  const denso::models::ManifestView& manifest);
```

`ModelsPage` gains no rules of its own — it renders what it is given. The
mode is read from `mode::load_target(db)`, i.e. the **committed** mode, never an
uncommitted settings-page selector value (§6.3).

### 6.2 What this release actually shows

- **`digit_reader`** (the only reachable mode with a wizard): the Models step
  lists `digitv3` only. `float-small` and `float-big` are absent — not greyed,
  not annotated, absent.
- **`ball_leveler`**: unchanged from Revision 3b. **No wizard is exposed, so
  there is no selection list to filter.** The mode admission gate that already
  prevents all pipeline construction is untouched.

This design deliberately ships the policy and the seam *before* the Leveler UI
exists, so that when a future approved Leveler spec adds that UI it inherits a
filtered list and cannot expose `digitv3` by omission. Adding a partial Leveler
model-selection UI now — one that could attach a model but never reach a valid
Leveler setup — is explicitly rejected: it would create attachable state for a
pipeline that cannot run, and would contradict Revision 3b §2.1.

### 6.3 The committed mode, not a selector value

Availability is a function of `mode.target` **as stored in the database**. The
settings page's mode combo box holds a *proposed* destination until the operator
confirms the destructive switch (modes spec §7.1); reading it would make models
appear or vanish from an unrelated dialog because someone opened a drop-down.
This is an acceptance criterion (§15.13), not a note.

---

## 7. Backend and runtime enforcement

**Five paths, one policy.** None of them restates a rule.

### 7.0 Warm-up — the path that actually loads engines

This is the load-bearing one, and the earlier draft of this design got it wrong.

`EngineRegistry::warm_up()` does **not** load the models a camera is attached
to. It iterates **every** `*.engine` (Jetson) / `*.onnx` (Windows) file in the
models directory and calls `get(filename)` followed by `infer(blank)` on each
(`engine_registry.cpp:46-88`). Attachment is consulted only afterwards, by
`missing_required_models(required_, warmed)`, and only to decide whether to
**throw** — which routes to `WarmupWorker` → `app.exit(1)`.

Two consequences the design must handle, neither of which camera-scoped
inhibition can reach, because warm-up runs in `startup.cpp:152` long before
`CameraGrid` installs any cause (`camera_grid.cpp:198`):

1. **Merely placing `float-small.engine` in the models directory of a
   `digit_reader` appliance deserializes and runs it at every boot.** No
   attachment is required. A filter on `detection_for` is far too late.
2. **`required_` is built from unfiltered `attached_model_filenames`**
   (`startup.cpp:155`, and `try_attached_model_filenames` in
   `run_headless.cpp:125`). If an incompatible attachment named a model that was
   correctly skipped by the compatibility filter, warm-up would throw and take
   the **whole appliance** down — the exact opposite of the camera-scoped
   guarantee in §7.3.

**Therefore:**

- `EngineRegistry` is constructed with an **allow-list** — `loadable_model_files(mode, …)`
  — and its directory scan skips any file not in it. A rejected model is never
  passed to `get()`, so no incompatible plan is ever deserialized.
- The **`required_` fail-loud set is mode-filtered too**: an incompatible
  attachment is removed from it, so it can never trigger the global
  `app.exit(1)`. It is reported as a camera-scoped issue instead (§7.3).
- `attached_model_filenames` / `try_attached_model_filenames` gain the mode and
  manifest view, exactly like the other repo accessors.

The existing engine-only, no-fallback, fail-loud contract is otherwise
**unchanged**: a model that is allowed and attached but missing or invalid still
aborts startup, as it does today. Only *rejected* models leave the required set.

### 7.1 Attachment / save validation

`detection::set_camera_models` gains a `TargetMode` parameter and refuses the
whole transaction if any model in the set is not `Allowed`. It is the domain
chokepoint: the wizard, any future Leveler UI, and any future CLI all go
through it. Refusal is a rolled-back transaction and a typed failure, so a
partial attachment cannot exist.

Because a refusal must be diagnosable, the failure carries the camera id, the
canonical ID (or `"<undeclared>"`), the filename and the reason code — and
nothing else (§12).

### 7.2 Runtime model loading

`detection::detection_for` resolves a camera's attached models for the runtime.
It gains the same mode-aware filter, and `CameraGrid::start_one` consults the
resolved result:

- a camera whose every attached model is rejected gets **no**
  `DetectionProcessor` — it is inhibited (§7.3), not silently demoted to
  `OrientationProcessor`, because a demotion would look like a working camera
  that has quietly stopped reading;
- `engines_->get()` is **not** called for a rejected model, so an incompatible
  plan is never deserialized and the fail-loud `TrtEngine` ctor is never the
  thing that discovers the problem;
- a camera with a mix of allowed and rejected models is inhibited as a whole.
  Running the allowed subset would silently change what the camera reports.

### 7.3 Startup / integrity evaluation and camera-scoped inhibition

`health::evaluate_integrity` gains the mode and the manifest view, and emits a
**new, real-producer** issue kind:

```cpp
struct ZoneIssue {
    enum class Kind {
        EngineMissing, EnginesUnmanifested, ModelCompatibilityRejected
    };
    Kind    kind;
    int64_t camera_id = 0;
    QString detail;
    QString policy_reason;   // the policy's stable reason code (see below)
};
```

`ZoneIssue` already carries `camera_id` and is already used camera-scoped by
`EngineMissing` — no fake zone is invented and no structure is bent.

**The kind is `ModelCompatibilityRejected`, not `ModelModeIncompatible`.** The
issue covers every way the policy can reject an attached model, of which
wrong-mode is only one; naming the kind after a single branch would make six
other failures self-describe as a mode problem and send an operator hunting for
a mode switch that never happened. The kind says *what happened to the camera*;
the **`policy_reason` carries the actual `reason_code` verbatim** from §4.2:

| `policy_reason` | Means |
|---|---|
| `model_undeclared` | no schema-2 entry — or none for the active backend (§3.2.1 rule 5) |
| `model_unknown_id` | `canonical_id` not in the compiled registry |
| `model_family_mismatch` | declared family ≠ the registry's family for that id |
| `model_shape_unsupported` | `task` ≠ `detect`, or `input_size` ≠ 640 |
| `model_classes_mismatch` | artifact class names/count ≠ the declaration |
| `model_provenance_failed` | hash mismatch, or `built_for` mismatch (Jetson only) |
| `model_mode_incompatible` | **only** the wrong-mode case: declared, valid, and not allowed in the committed mode |

Both strings are a FILE FORMAT: `reason_code(ModelCompatibilityRejected)` is
`"model_compatibility_rejected"`, and every `policy_reason` value is stable,
never renumbered, never reused, only added to. Both appear in `status.json` so a
diagnosis names the real cause.

**No new `ZoneCause` bit.** `CameraGrid` maps the new issue onto the existing
`ZoneCause::ModelUnavailable`, exactly as it already maps `EngineMissing`
(`camera_grid.cpp:201-205`). The cause bitmask is a file format; reusing the
semantically correct existing bit ("this camera has no usable model") is
preferable to spending `1u << 5`, and the distinct diagnostic survives in the
issue's reason code and detail.

**Readiness classification: Degraded, not Blocked.** An incompatible attachment
is camera-scoped by construction, so it maps to `Readiness::Degraded` → exit
code **10**, and healthy Digital Reader cameras keep running and keep reporting.
It is not a `GlobalBlocker`: bricking a four-camera appliance because one camera
has a bad attachment is the opposite of the per-zone fail-closed contract.

### 7.4 The restored / hand-edited database case

This is the case the design exists for. An operator restores a database backup,
or edits `camera_model` by hand, attaching `float-small` to a `digit_reader`
camera. Nothing in the write path ran, so §7.1 never saw it. The sequence is:

1. boot → `evaluate_integrity` resolves each attachment through the policy →
   `ModelCompatibilityRejected` for that camera, carrying
   `policy_reason = model_mode_incompatible`;
2. warm-up (§7.0) receives an allow-list that excludes the rejected model and a
   required set that no longer names it, so the engine is neither deserialized
   nor able to abort startup;
3. `CameraGrid::reload()` sets `ZoneCause::ModelUnavailable` on that camera id
   before any stream starts; `start_one` constructs no `DetectionProcessor` and
   requests no engine;
4. `ZoneReporter::set_camera_inhibited(id, true)` → that camera's zones stop
   reporting and its recorded zones are evicted;
5. the tile shows the inhibit banner; `status.json` carries the issue with its
   reason code and camera id;
6. every other camera is untouched.

"Must not start a pipeline" is therefore satisfied per camera, and the engine is
never loaded — at step 2, before any GPU work at all.

---

## 8. Artifact pipeline

### 8.1 Inputs, verified

Both source files are present at `models/` (git-ignored, as the rule requires):

| | `float-small.pt` | `float-big.pt` | `digitv3.pt` (reference) |
|---|---|---|---|
| SHA-256 | `e9a2294757cc13c1041f643f83d2651616434d016d3f4537d81517ea10d7330f` | `3cf0a655af70bcaa960cf96e174cffedf1c8ca41d70293fd19a88fe07727c3b1` | `a1917b1c1320e62d00ae7e3c6d1ffc80b17c4e0e7a81ed94286635e6cbc4944c` |
| architecture | YOLOv8n (`DetectionModel`, base `yolov8n.pt`) | YOLOv8n (base `yolov8n.pt`) | YOLO26s |
| task | `detect` | `detect` | `detect` |
| classes (ordered) | `["Small"]` | `["Big"]` | `["0"…"9"]` |
| class count | 1 | 1 | 10 |
| train `imgsz` | 640 | 640 | 640 |
| training Ultralytics | **8.4.33** | **8.4.21** | 8.4.33 |
| trained | 2026-04-01 | 2026-03-13 | 2026-07-16 |
| stride | 8/16/32 | 8/16/32 | 8/16/32 |

No substitution was made and no metadata was fabricated; every value above was
read from the checkpoints.

### 8.2 Loader compatibility — the decode path differs from digitv3

This is the single most important technical finding of the inspection.

`digitv3.onnx` was exported **end-to-end (NMS-free)**: its ONNX metadata records
`end2end: True`, `args: {batch: 1, half: False, dynamic: False, simplify: True,
opset: 12, nms: False}`, input `images [1,3,640,640]`, output
`output0 [1,300,6]`. `TrtEngine` sees `output_shape_.d[2] == 6` and takes
`decode_yolo_end2end` (`trt_engine.cpp:271`).

The float models are classic YOLOv8 detection heads. Exported the same way they
will produce `[1, 4 + nc, anchors]` = **`[1, 5, 8400]`**, so `d[2] == 8400`,
and `TrtEngine` takes the **`decode_yolo`** path with `num_classes = 5 - 4 = 1`
and class-agnostic NMS. Both branches already exist, are shared with the ONNX
Runtime backend, and are unit-tested.

Consequences that constrain the export (§8.3):

- **`nms=False` is mandatory.** An ultralytics export with `nms=True` would
  produce a different output layout and an unsupported graph.
- **`dynamic=False`, `batch=1`** — `TrtEngine` requires a static `[1,3,640,640]`
  input or a profile admitting batch 1; `digitv3.onnx` is static batch=1 and
  AGENTS.md records that a dynamic `opt=4` engine would need a re-export.
- **FP32 input/output tensors** — `TrtEngine` throws
  `"input and output tensors must both use FP32"` if the I/O is not FP32. FP16
  is a *build precision* (`--fp16` lets TensorRT use FP16 kernels internally);
  it must not become an FP16 binding type.
- **`imgsz=640`** — `is_input_shape_640` rejects anything else.

A theoretical ambiguity exists in the shape heuristic — a model whose anchor
count were exactly 6 would be misread as end-to-end — but 8400 anchors at
imgsz 640 makes it unreachable here. It is recorded, not acted on.

### 8.3 Stage 1 — `.pt` → `.onnx`, on the Windows dev machine

Exporter versions are **pinned to each model's recorded training version**, in
isolated environments. Silently exporting both with 8.4.33 is not acceptable:

| Model | Export environment |
|---|---|
| `float-small.pt` | Ultralytics **8.4.33** |
| `float-big.pt` | Ultralytics **8.4.21** |

`D:\workspace\train_venv` is 8.4.33 / torch 2.5.1+cu121 and serves `float-small`
directly. A **separate, isolated** virtual environment is created for 8.4.21;
`train_venv` is not mutated.

**Fallback for `float-big` — approved, controlled (locked).** 8.4.21 is attempted
first. If it cannot be installed against an available torch, cannot load the
checkpoint, or cannot export it, **Ultralytics 8.4.33 is an approved deviation**,
subject to all of:

- the 8.4.21 failure is **captured verbatim** and recorded in the provenance;
- `training_ultralytics: "8.4.21"` and `export_ultralytics: "8.4.33"` are
  recorded as **separate fields** — the manifest never collapses them;
- **the source checkpoint is not modified** in any way;
- every ONNX assertion in this section still passes — shape, dtype, class order,
  class count, and the `args` metadata;
- the resulting engine must **deserialize and complete a real inference natively
  on `.15`** (§10) before it is approved.

**Retraining is not triggered by a version difference alone** and is outside this
scope. It becomes a question only if one of the assertions above fails, or if
`.15` validation shows a correctness or compatibility failure.

Export arguments follow the repository's existing digitv3 process rather than a
guess. `train_model/train.py:86` invokes
`export(format="onnx", imgsz=640, simplify=True, opset=13)`, and the resulting
`digitv3.onnx` records `opset: 12` — Ultralytics settled on 12. So: invoke with
the same arguments the repository uses, then **read back the emitted `args`
metadata and record the actual opset** rather than asserting one.

Required properties of each emitted ONNX, all asserted before proceeding:

```
task=detect   imgsz=640   batch=1   dynamic=False   nms=False
input  images  [1,3,640,640]  FP32
output output0 [1,5,8400]      FP32
metadata names == {0: "Small"}  /  {0: "Big"}
```

No engine is built on Windows; `tools/build_trt_engine.sh` targets an `sm_89`
RTX and is explicitly not for this.

### 8.4 Stage 2 — `.onnx` → `.engine`, on Jetson `192.168.1.15`

Built on-device with the installed `/usr/src/tensorrt/bin/trtexec`, following
the recorded `digitv3` recipe in `packaging/models.approved`:

```
trtexec --onnx=float-small.onnx --saveEngine=float-small.engine --fp16
trtexec --onnx=float-big.onnx   --saveEngine=float-big.engine   --fp16
```

FP16 build precision, batch 1, static input `1x3x640x640`. Platform:
JetPack 6.2 / L4T R36.5, TensorRT 10.3, CUDA 12.6, `sm_87`.

**`192.168.1.81` is not used for any step.** It is reserved for the user's manual
`.deb` installation and testing.

### 8.5 `.names.json` sidecars — the existing format, exactly

The repository's sidecar format is a **bare JSON array of class-name strings,
index == class id**, produced beside the engine as `<stem>.names.json`.
Verified: `models/digitv3.names.json` is exactly
`["0","1","2","3","4","5","6","7","8","9"]`, and `read_names_sidecar` requires a
JSON array whose every element is a string and which is non-empty.

Therefore:

```
float-small.names.json  →  ["Small"]
float-big.names.json    →  ["Big"]
```

No new JSON structure is invented; no object wrapper, no metadata keys.
Generated from the exported artifact's own `names` metadata, not typed by hand.

Validation before an engine is approved:

1. the file parses as a JSON array of non-empty strings;
2. exactly **one** element, in the order emitted by the model;
3. the count equals the engine's decoded class count — for `[1,5,8400]`,
   `d[1] - 4 == 1`;
4. the array equals the manifest generation's `class_names` and `class_count`
   (§3.4);
5. `models::file_sha256` of the sidecar equals the manifest's `sidecar_sha256`.

### 8.6 Portability — stated, and not overstated

A TensorRT plan is compiled for the TensorRT version and GPU architecture it was
built against. These engines are qualified **only** for the supported deployment
configuration: Jetson Orin Nano, L4T R36.5.0, TensorRT 10.3, CUDA 12.6, `sm_87`.
No claim of portability to another TensorRT version, another CUDA version,
another JetPack, or another GPU architecture is made anywhere in this design,
in the manifest, or in any report produced by it.

The `[trt] Using an engine plan file across different models of devices` warning
is a known benign artifact of TRT 10.3 on Orin (AGENTS.md, measured 2026-07-21
on `.15` against an engine `.15` itself built). It is not a portability signal
and must not be chased. It becomes actionable only alongside a deserialization
failure, CUDA errors, wrong results, or a changed platform baseline.

### 8.7 Deployment of the manifest — TWO RELEASES, not one gate

The compatibility policy is only as available as the manifest, and the current
packaging cannot deliver it as a side effect of an upgrade. This was checked, not
assumed:

- `packaging/debian/postinst` is **structural only by deliberate design** — it
  creates `/opt/denso/data/models` and prints instructions; everything requiring
  judgement lives in `denso-setup`, which the operator runs explicitly
  (`postinst:3-24`);
- model seeding lives **exclusively** in `denso-setup cmd_configure`
  (`denso-setup:36-90`), which an operator runs at commissioning and which
  *refuses a user change* on a second run;
- `dpkg` never touches `/opt/denso/data`, so an `apt install` of a newer `.deb`
  over an existing installation seeds nothing.

So shipping enforcement and the manifest in one release would install
compatibility-enforcing code onto an appliance whose data dir still has no
manifest, and **every camera would be inhibited until an operator ran an extra
step.** A rehearsal would observe that breakage; it would not prevent it.

**Therefore this work ships as two releases, in order.**

#### The artifact-placement rule that shapes the split

Revision 2 of this spec put the Float artifacts into Release A. **That was wrong,
for exactly the reason §7.0 exists:** `EngineRegistry::warm_up()` scans the
active models directory and deserializes *every* runtime artifact it finds,
attachment or not (`engine_registry.cpp:46-88`). Release A has no warm-up
firewall — that is Release B's Slice 7. So placing `float-small.engine` into
`DENSO_DATA_DIR/models` during Release A would deserialize and run a Floating
Ball model on a Digital Number Reader appliance at every boot. That is an
application behaviour change, in the release whose entire premise is that it
changes no application behaviour.

> **Locked ordering rule.** **Release A carries no Float artifact at all** — not
> in the `.deb`, not approved for seeding, not installed under `/opt/denso`, not
> in any application-visible directory, and **not in any staging location**
> (§8.7.1). Until Release B they exist only in the development/export workspace,
> an isolated temporary directory on `.15`, and non-package artifact storage.
> The first package release containing one is Release B, Slice 12.

**Release A — identity deployment only.** Ships:

- schema-2 manifest parsing, generation and validation support;
- the explicit `denso-setup seed-manifest` command (§8.8);
- `denso-setup verify` reporting of manifest state — **non-mutating with
  respect to `models/` and manifest state** (§8.8);
- **a schema-2 declaration for the appliance's existing, already-approved
  `digitv3` artifacts** — this is the whole point: the manifest that Release B
  will require is put in place for the model that is already running;
- the export tooling and the recorded artifact provenance.

**Release A carries no Float artifact of any kind — locked, not a default.** No
`float-*.onnx`, `float-*.engine` or `float-*.names.json` is in the Release A
`.deb`, approved for seeding, installed under `/opt/denso`, or placed in any
application-visible directory. Until Release B they live only in the
development/export workspace, an isolated temporary directory on `.15`, and
non-package artifact storage. **No staging directory is created or used** — see
§8.7.1.

Release A changes **no application authorization and no warm-up behaviour.** No
new artifact becomes visible to `sync_models`, `warm_up` or `evaluate_integrity`.

**Release B — enforcement, then Float placement.** Introduces, in order:

1. the central compatibility policy;
2. **the warm-up allow-list and filtered required set** — the firewall;
3. camera-scoped backend enforcement;
4. UI filtering;
5. **only now**, installation and seeding of `float-small` and `float-big` into
   the active models directory.

Step 5 must not precede step 2 in the commit order, and the merge gate enforces
it (§8.7.2).

**Gate A between the releases:** §15.19a and the plan's Gate A — an upgrade
rehearsal on `.15` against an isolated `DENSO_DATA_DIR` simulating an existing
commissioned installation, proving `seed-manifest` reaches it, `digitv3` still
loads, no camera is newly inhibited, and **no Float engine is in the active
directory**. Until that passes, Release B does not ship. See R1.

#### 8.7.1 No package location is inert — which is why Release A carries nothing

A staging location that nothing scans does not exist today, and this design
deliberately does **not** invent one.

`/opt/denso/models` (`$PKG_MODELS`, `denso-setup:21`) is **not** inert. It is
globbed for `*.engine` by three separate loops: `cmd_configure` seeds every one
of them into the active data dir (`denso-setup:73-84`), `cmd_verify` enumerates
them (`:226`, `:320`), and `cmd_replace_model` resolves from it (`:426`).
`tools/build_package.sh:189-191` installs every `--model` there. Dropping a Float
engine into the package's model directory during Release A would therefore make
`configure` seed it straight into the scanned active directory — the exact
outcome §8.7 exists to prevent.

Nor is the active `DENSO_DATA_DIR/models` inert: it is independently scanned by
`EngineRegistry::warm_up` (`engine_registry.cpp:39-48`), `sync_models`
(`model_sync.cpp:23,46`) and `evaluate_integrity` (`integrity.cpp:108,148`).

A new `/opt/denso/models-staging/` was considered and **rejected**. A directory
that is inert only because no loop currently globs it is one refactor away from
not being inert, and it would add a package path whose only purpose is to hold
artifacts nothing is allowed to use yet. **Release A simply carries no Float
artifact**, which needs no new path, no new exclusion rule, and no test to prove
a directory stays unread.

If a genuine need for staged-but-unseeded artifacts arises later, it can be
proposed as a separate packaging change on its own merits.

**The first package release containing any of `float-small.onnx`,
`float-small.engine`, `float-small.names.json`, `float-big.onnx`,
`float-big.engine`, `float-big.names.json` is Release B, Slice 12** — after the
central policy, the warm-up firewall, camera-scoped enforcement, UI filtering,
native engine smoke tests, and authorized-camera capture/replay validation.

#### 8.7.2 How the ordering is enforced, not merely stated

- Float artifacts are added to `packaging/models.approved` and to the seeding
  path in the **same commit** as, or a later commit than, the warm-up allow-list
  (plan Slice 7). A packaging assertion fails the build if a Float stem is
  approved for seeding while the allow-list symbol is absent from the tree. The
  assertion must test for the **compiled symbol** — the `loadable_model_files`
  definition in `src/core/models/compatibility.cpp` and its use in
  `src/app/ui/startup.cpp` — not for a comment or a declaration, which a stray
  mention could satisfy.
- `tests/packaging/run.sh` asserts that the Release A package payload contains
  **no** `float-*` artifact anywhere — no `.onnx`, no `.engine`, no
  `.names.json`, under any path.
- Gate A explicitly checks the active models directory on the device for Float
  artifacts and fails if any is present.

### 8.8 `denso-setup seed-manifest` — the upgrade-safe entry point

An already-commissioned appliance cannot re-run `cmd_configure`: it refuses a
user change and does far more than manifest work (`denso-setup:36-90`). So the
upgrade path is **one explicit, narrowly-scoped, mutating command**:

```
sudo denso-setup seed-manifest
```

Locked contract:

| Situation | Behaviour |
|---|---|
| No manifest present, data-dir artifacts match the approved set | **generate and seed** a schema-2 manifest declaring them |
| A manifest present and already identical to what would be generated | **no change**, report "already current", exit 0 |
| A manifest present but **differing** | **refuse, overwrite nothing**, report what differs, non-zero exit |
| Data-dir artifacts do not match `packaging/models.approved` | **refuse**, report the mismatch, seed nothing |
| Run twice in a row | second run is a no-op — **idempotent** |

**The on-device approval authority.** `packaging/models.approved` is a
**source-tree** file and is never installed — `build_package.sh:181-193` ships
`policy.sh`, the exe, the launcher and the model pairs, but not it. So
`seed-manifest`, running on the appliance, cannot consult it. Two consequences,
both locked:

- `build_package.sh` **installs `models.approved` to `/opt/denso/lib/models.approved`**
  (mode `0644`, beside `policy.sh`), making the approval list available on-device
  for the recipe and provenance text;
- the operative authority for "do the data-dir artifacts match the approved set"
  remains the **packaged pair in `/opt/denso/models`** compared by the existing
  `seed_decision_pair`, because `build_package.sh:157-159` already refused to
  package anything whose engine *and* sidecar hashes were not approved. The
  installed list is corroboration and human-readable provenance, not a second
  source of truth.

**Writing the manifest.** `install_pair` (`policy.sh:131-145`) is engine+sidecar
specific and cannot be reused. The manifest is a single file, so it gets the
straightforward atomic form the pair helper could not have: write
`.manifest.json.tmp` in the destination directory, `sync`, `mv` into place. One
rename, genuinely atomic — unlike the two-file pair, whose ordered-not-atomic
compromise is documented in `policy.sh:126-130`.

**Comparing an existing manifest** is by **canonical content**, not raw bytes:
parse both, compare the normalized generation set. Byte comparison would report a
reformatted-but-equivalent manifest as a conflict and send the operator to a
refusal they cannot resolve.

Additional locked properties:

- it is **explicitly mutating**, never implicit — no maintainer script calls it,
  so an `apt` upgrade never silently rewrites operator data;
- it does **not** run configure, does not touch ownership, autostart, autologin
  or the recorded user;
- it runs as the target user for anything under the data dir, per the existing
  root-artifact rule;
- **no credential is read, required or emitted** — it deals only with model
  artifacts and hashes;
- refusal is the default on any ambiguity. It never resolves a conflict by
  guessing which manifest the operator meant.

#### `denso-setup verify` — non-mutating with respect to `models/`

The locked requirement is that `verify` never repairs, and specifically that it
never creates, rewrites or adopts a manifest. **A `verify --repair` is rejected
and must not be implemented**: `verify` is what an operator runs to find out
whether the appliance is sound, and a diagnostic that can change the thing it is
diagnosing cannot be trusted to report what was there before it ran. Repair is
`seed-manifest`, always explicitly invoked.

**A precision that must not be glossed over:** `verify` is *not* a strictly
read-only command today, and this design does not make it one. It deliberately
writes a database backup into the data dir before its migration smoke test
(`denso-setup:254-286` — a unique `mktemp -d` directory holding `denso.db` and
its `-wal`, created as the target user), and it creates throwaway temp dirs. That
behaviour is an intentional safety feature — "the remedy if a migration goes
wrong" — and it is **out of scope here**; removing it to satisfy a phrase would
delete a real protection.

So the binding, testable statement is narrower and true:

> Everything `verify` gains in this design is **observation only**. It reports
> manifest presence, packaged-vs-data-dir agreement, per-artifact hash state, and
> the §5.1 informational idle-artifact list. It creates no manifest, rewrites no
> manifest, adopts no artifact, and changes no file in `models/`.

Acceptance criterion 19c asserts exactly that: the **`models/` subtree** — every
engine, sidecar and `manifest.json` — is byte- and mtime-identical before and
after a `verify` run. The pre-existing backup directory is expected and is
explicitly excluded from that comparison.

---

## 9. Engine / sidecar checksum metadata

Recorded per generation in the manifest and reproduced verbatim in the export
report:

| Field | Source |
|---|---|
| source `.pt` SHA-256 | `sha256sum` on the dev machine |
| `.onnx` SHA-256 | `sha256sum` after export |
| `.engine` SHA-256 | `sha256sum` on `.15` after the build |
| `.names.json` SHA-256 | `sha256sum` on `.15` |
| ordered class names, class count | exported artifact metadata |
| task, input size, batch, dynamic, nms | exported artifact metadata |
| ONNX opset | emitted `args` metadata, read back |
| training Ultralytics version | the `.pt` checkpoint — recorded **separately** from the export version, never collapsed |
| export Ultralytics version | the export environment |
| exporter deviation record | present only when §8.3's fallback was used: the verbatim 8.4.21 failure and why 8.4.33 was substituted |
| precision | `--fp16` |
| TensorRT / CUDA / JetPack / `sm` | the `.15` platform |
| exact export commands | verbatim, both stages |
| engine deserialization result | §10 |
| smoke-inference result | §10 |

The runtime already re-verifies engine and sidecar hashes (§3.4); the remaining
fields are provenance for humans and for the packaging approval flow.

---

## 10. Headless engine validation on `.15`

The Ball Leveler production UI stays locked, so validation is headless. Two
levels, in order:

**Deserialization** — `denso --check` constructs each engine through the real
`TrtEngine` ctor and validates bindings, shapes and the class-names sidecar. It
is the repository's chosen validation path precisely because `trtexec
--loadEngine` proves only that TensorRT can read the file, not that the app can
(deploy slice-1 plan, `run_headless.cpp`). Run against an isolated
`DENSO_DATA_DIR`, never the production data dir.

**Inference** — `--check` deliberately never calls `infer()`, so deserializing is
not inferring (AGENTS.md). A headless smoke path therefore runs **one real
inference per engine** and asserts the output shape decodes on the expected
branch (`[1,5,8400]` → `decode_yolo`, `num_classes == 1`). This is a
build-and-decode proof, not an accuracy claim.

Both run under `QT_QPA_PLATFORM=offscreen` over SSH, on `192.168.1.15` only.

---

## 11. Authorized-camera validation

Authorized for this scope: **`192.168.1.185`, `.186`, `.187`, `.188`**.
Credentials and RTSP configuration are read from the project's isolated test
database. `192.168.1.81` is not contacted.

Run on `.15`, against an isolated `DENSO_DATA_DIR`, with the production data dir
untouched:

**`digit_reader` (regression — the behaviour that must not change):**

- the Models step lists `digitv3` and **only** `digitv3`;
- `float-small` and `float-big` are absent from the list;
- attaching `digitv3`, streaming from the authorized cameras, and reading digits
  behaves exactly as before this change;
- a hand-attached `float-small` (written directly to the DB) inhibits **that**
  camera only, and the others keep streaming and reporting.

**Floating Ball engines — frames-from-file, never a live Leveler pipeline**

Feeding live frames to a float engine *while the appliance is in `ball_leveler`*
would require the very `CameraStream`/processor construction Revision 3b
forbids. So camera validation and mode validation are **decoupled**:

1. **Capture** a set of still frames from each authorized camera using a
   standalone capture step — the existing snapshot path or a small offline
   OpenCV/GStreamer utility. This is capture only: no `CameraStream`, no
   `FrameProcessor`, no grid, no reporter, and it is not tied to any mode.
2. **Replay** those saved frames through `float-small` and `float-big` in a
   headless harness that constructs `TrtEngine` directly, exactly as the Slice-8
   smoke path does. Assert the output decodes on the `decode_yolo` branch with
   `num_classes == 1`, and **record observed latency and FPS** per engine per
   camera.

Separately, and without any engine work, confirm the mode itself is untouched:
`ball_leveler` still persists, still lands on the "not available in this release"
state, and still constructs no wizard, no production `CameraStream`, no
`DetectionProcessor` and no reporter.

The captured frames are real images from the authorized cameras and are treated
as sensitive: stored only under the isolated data dir and deleted with it. `digitv3`
remains non-selectable and policy-rejected in `ball_leveler` throughout.

No ball-level percentage, position or calibration output is produced, computed
or claimed anywhere in this validation.

---

## 12. Security and redaction

Binding on every step, artifact and report this design produces:

- camera passwords are **never** printed, logged, echoed, written to a file, or
  included in any report, screenshot or commit;
- credential-bearing RTSP URLs are **never** emitted; reports use redacted forms
  (`rtsp://<redacted>@192.168.1.185/...`). The repository already has
  `logging/redact.cpp` `sanitize_url` for exactly this — reuse it, do not write
  a second redactor;
- credentials are never placed in source code, documentation, specs, plans, or
  the manifest;
- credentials are read from the isolated test database only, and only where a
  step genuinely requires them;
- destructive operations run **only** against an isolated `DENSO_DATA_DIR`;
- the compatibility `detail` string carries camera id, canonical ID and model
  filename — never a URL, username or password;
- `192.168.1.81` is not accessed, configured, operated or referenced by any
  automated or remote step.

---

## 13. No Floating Ball algorithm in this scope

Nothing here implements or approximates ball position, level percentage,
calibration, thresholds, or any Leveler-specific reporting. `ball_leveler`
remains an unavailable destination exactly as Revision 3b defines it: no wizard,
no production `CameraStream`, no `DetectionProcessor`, no `ZoneHealth` wiring,
no reporter. The compatibility policy is built now so that the future Leveler
UI — which will arrive with its own approved specification — inherits a
correctly filtered model list instead of having to re-derive one.

---

## 14. Schema decision

**The SQLite schema stays at v13. No migration is added.**

Everything this design introduces is artifact and catalog metadata that lives in
`manifest.json` beside the model files, or is derived at runtime from data the
`model` table already stores (`filename`, `class_names`). The `TargetMode` it
consumes already rides the existing `settings` key/value table. Adding a
`canonical_id` column to `model` was considered and rejected: it would duplicate
the manifest's authority inside the database, where a restored backup could then
disagree with the artifacts on disk — creating precisely the class of
inconsistency this design exists to detect.

The **manifest** schema version goes 1 → 2 (§3.2). That is a file format, not
the database.

---

## 15. Acceptance criteria

1. `model_compatibility(digit_reader, digitv3)` → `Allowed`.
2. `model_compatibility(digit_reader, float-small)` → `RejectedWrongMode`.
3. `model_compatibility(digit_reader, float-big)` → `RejectedWrongMode`.
4. `model_compatibility(ball_leveler, float-small)` → `Allowed`.
5. `model_compatibility(ball_leveler, float-big)` → `Allowed`.
6. `model_compatibility(ball_leveler, digitv3)` → `RejectedWrongMode`.
7. An undeclared model, an unknown `canonical_id`, a declared-family mismatch, a
   class-name or class-count mismatch, and a hash/platform mismatch are each
   rejected — in **both** modes.
8. Rejected models are absent from `selectable_models` in every mode.
9. `set_camera_models` refuses a rejected model and leaves the database
   unchanged (rolled back — no partial attachment).
10. A directly-written incompatible `camera_model` row inhibits exactly that
    camera at boot, constructs no `DetectionProcessor`, and requests no engine
    from `EngineRegistry` — asserted, not inferred.
11. Other cameras with compatible attachments continue to stream and report
    while a sibling camera is inhibited; readiness is **Degraded (10)**, not
    Blocked (78).
12. No incompatible engine is ever deserialized: `TrtEngine` is not constructed
    for a rejected model — **including a rejected model that merely sits in the
    models directory with no attachment at all.** Asserted against warm-up, not
    only against `start_one`.
12a. A rejected model never enters the warm-up fail-loud required set, so an
    incompatible attachment cannot produce `app.exit(1)`. An *allowed* attached
    model that is missing or invalid still aborts startup, unchanged.
13. Availability follows the **committed** `mode.target`, not the settings
    page's uncommitted selector value.
14. The **five** enforcement paths (warm-up allow-list + required set, UI list,
    attachment, runtime resolve, integrity) all call `model_compatibility`; no
    second implementation of the matrix exists anywhere in the tree — asserted by
    review and by a grep-able single definition. The manifest does not carry
    `allowed_modes`.
15. `float-small.names.json` is exactly `["Small"]`; `float-big.names.json` is
    exactly `["Big"]`; each parses under the existing `read_names_sidecar` and
    its count equals the engine's decoded class count.
16. Each engine deserializes through the real `TrtEngine` on `.15` and completes
    one real inference decoding on the `decode_yolo` branch with
    `num_classes == 1`.
17. Every provenance field in §9 is recorded for both models.
18. No portability claim beyond the supported deployment configuration appears
    in any artifact, manifest or report.
19. Existing `digit_reader` behaviour is unchanged for an appliance carrying a
    correct schema-2 manifest: the full `ctest` suite passes (only the known
    Windows symlink skip), packaging assertions pass, and digit reading on the
    authorized cameras is unaffected.
19a. **Release A installs onto a simulated existing commissioned appliance;
    `denso-setup seed-manifest` seeds a schema-2 declaration for the *existing*
    `digitv3` artifacts without a full re-`configure`; `digitv3` still loads; no
    camera is newly inhibited; application behaviour is unchanged.** Proven on
    `.15` before Release B ships (§8.7).
19b. **Release A places no Float `.onnx` or `.engine` in the active models
    directory** — asserted against the package payload and against the device's
    active models dir at Gate A (§8.7.1–2).
19c. `seed-manifest` is idempotent; refuses a differing manifest without
    overwriting; refuses when data-dir artifacts do not match the approved set;
    needs no credential. **`denso-setup verify` changes nothing under
    `models/`** — asserted by comparing a hash+mtime snapshot of the `models/`
    subtree before and after a run, excluding the pre-existing DB-backup
    directory `verify` deliberately creates elsewhere in the data dir (§8.8).
    **No `verify --repair` entry point exists** — asserted by grep.
19d. **Windows verifies only the ONNX artifact; Jetson verifies only the engine,
    sidecar and `built_for`.** A TensorRT `built_for` mismatch **cannot** reject a
    model on Windows — asserted with a manifest whose `built_for` is deliberately
    wrong, which must still resolve `Allowed` under the ORT backend.
19e. A generation with no runtime block for the active backend is
    `RejectedUnknown` / `model_undeclared` on that platform, and unaffected on
    the other.
19f. A camera-scoped rejection carries the **correct** `policy_reason`: one
    assertion per reason code, so a hash mismatch never reports itself as a mode
    problem.
19g. **A declared, valid, unattached wrong-mode artifact leaves readiness `Ready`
    and exit code `0`**, emits at most one redaction-safe informational line, and
    appears in verbose `--check` / `verify` output (§5.1).
20. No credential appears in any log, report, artifact, spec, plan or commit
    produced by this work.
21. `ball_leveler` still constructs no production `CameraStream`, no
    `DetectionProcessor` and no reporter, and exposes no setup wizard.
22. Schema stays v13; `PRAGMA user_version` is unchanged by every code path here.
23. A schema-1 or absent manifest still parses/validates exactly as today for
    `--migrate-model` and `evaluate_integrity` — it is never reported as
    `ManifestCorrupt`.

---

## 16. Risks

- **R1 — the manifest becomes load-bearing, and an appliance without one has no
  usable models.** The direct, intended consequence of fail-closed identity, and
  the largest operational risk here. The first draft of this design proposed a
  single release with an upgrade rehearsal as the mitigation; that was wrong —
  `postinst` is structural-only and `dpkg` never touches `/opt/denso/data`, so
  the rehearsal would have *observed* the breakage rather than prevented it.
  **Mitigation: the two-release split in §8.7** — a schema-2 declaration for the
  *existing* `digitv3` artifacts is delivered and verified by `seed-manifest`
  *before* any enforcement code exists, with `denso-setup verify` making an
  un-migrated appliance visible in between. The residual failure remains
  camera-scoped and diagnosable, never silent.
- **R1a — Release A could become a behaviour change by accident.** Its safety
  rests entirely on Float artifacts staying out of every directory the
  application scans: `warm_up()` needs no attachment to load a file, so a single
  misplaced `.engine` is enough. §8.7.1–2 turn that from a documented intention
  into a packaging assertion and a Gate A check, because on the appliance the
  failure would surface only as an unexplained Floating Ball engine in a Digital
  Number Reader's boot log.
- **R2 — `float-big` should be exported by Ultralytics 8.4.21**, an older version
  than the working environment. It may not install cleanly against the available
  torch, or may not load the checkpoint. Mitigation: an isolated environment,
  and — per the locked decision in §8.3 — a **controlled, recorded** fall-back to
  8.4.33 rather than either a silent substitution or a stop. The control is that
  the failure is captured, both versions are recorded separately, the checkpoint
  is untouched, and the result must deserialize *and infer* natively on `.15`.
- **R3 — the decode-branch heuristic is shape-based.** `d[2] == 6` selects
  end-to-end decoding. It is unambiguous for these models (8400 anchors) but is
  a latent trap for a future model. Recorded; not addressed here.
- **R4 — hash verification is I/O at boot.** Two additional engine/sidecar
  hashes lengthen startup on the Jetson. Unmeasured; measurement on `.15` is a
  plan task. It inherits the accepted `--migrate-model` TOCTOU limitation.
- **R4a — warm-up becomes mode-dependent.** With §7.0, which engines are
  deserialized at boot now depends on the committed mode. A mode switch therefore
  changes the warm-up set, and `EngineRegistry` never unloads (R5), so after a
  switch the process may hold engines from both families. Bounded here by there
  being no `ball_leveler` pipeline at all; it must be measured when the Leveler
  ships.
- **R5 — engines are never unloaded** (`EngineRegistry` caches per filename and
  never erases). With a second model family existing, GPU memory across a mode
  switch becomes measurable for the first time. Modes spec R3 deferred this;
  this design does not close it, and must not be read as having done so.
- **R6 — the policy's compiled registry is a code-change gate.** Adding a model
  family requires a build. That is deliberate (§4.3), but it means a field model
  swap cannot be done by editing a file, and operators must know that.
- **R7 — no Leveler readiness vocabulary.** Unchanged from modes spec R6: the
  integrity verdict cannot express Leveler configuration health. The new issue
  kind is about *model compatibility*, not about Leveler correctness.

---

## 17. Testing strategy

**Pure unit (Catch2, `denso_tests` — no GPU, no display, no database):**

- the full (mode × model) matrix from §15.1–6, plus every rejection branch of
  §4.2 in order — including the case where two rejection conditions hold at once
  and the *first* is reported;
- default-constructed `CompatibilityResult` denies;
- manifest schema-2 parse/validate: each new required field missing, blank,
  malformed, duplicated; `class_count != class_names.size()`;
- schema-1 and absent-manifest behaviour is byte-for-byte what it is today;
- sidecar validation: `["Small"]`, `["Big"]`, empty array, non-string element,
  wrong count vs. declared, order swapped.

**Persistence (Catch2 over an in-memory DB):**

- `selectable_models` returns only `digitv3` in `digit_reader`, only the two
  float models in `ball_leveler`, and nothing for an undeclared catalog;
- `set_camera_models` refusal leaves the DB unchanged, asserted by row counts
  before/after;
- `evaluate_integrity` emits `ModelCompatibilityRejected` with the right
  `camera_id` **and the correct `policy_reason`** for a directly-written
  incompatible attachment, and `Degraded`, not `Blocked`. One case per reason
  code, so a mismatched hash does not report itself as a mode problem.
- A declared, valid, **unattached** wrong-mode artifact leaves readiness `Ready`
  and exit code `0` (§5.1) — asserted, because this is the normal state of a
  fully provisioned appliance.

**Integration (Qt offscreen, `denso_integration_tests`, model-less):**

- an inhibited camera constructs no `DetectionProcessor` and never calls
  `EngineRegistry::get()` — asserted with a counting stub;
- sibling cameras keep running;
- `status.json` carries the new reason code and camera id, and no credential.

**Native `.15` (`192.168.1.81` never contacted):**

- ONNX property assertions; engine build; sidecar generation and validation;
- `--check` deserialization; one real inference per engine; decode-branch and
  class-count assertions;
- authorized-camera frame flow with recorded latency/FPS;
- the `digit_reader` regression set of §11;
- the upgrade rehearsal from R1.

---

## 18. Open questions

**None.** Every question raised by Revision 2 was answered by the product owner
and is locked into this revision:

| Question | Locked answer | Where |
|---|---|---|
| Release boundary | Release A is **identity deployment only** and carries **no Float artifact at all**; the first package release containing one is Release B Slice 12 | §8.7, §8.7.1–2 |
| Upgrade-safe seeding entry point | `denso-setup seed-manifest`; **no `verify --repair`**; `verify` is non-mutating with respect to `models/` and manifest state — **not** globally read-only | §8.8 |
| Backend-specific artifacts | a `runtime` block per backend; TensorRT `built_for` can never reject an ORT deployment | §3.2, §3.2.1 |
| Issue semantics | `ZoneIssue::Kind::ModelCompatibilityRejected` + the real `policy_reason`; `model_mode_incompatible` only for the wrong-mode case | §7.3 |
| `float-big` at 8.4.21 | attempt 8.4.21; 8.4.33 is an **approved controlled deviation** with the failure captured and both versions recorded separately. No retraining on a version difference alone | §8.3 |
| `selectable_models` return type | a dedicated `SelectableModel{row, metadata}` — never parallel containers | §6.1 |
| Unattached rejected artifacts | a **normal** state: skipped, informational only, **never Degraded** | §5.1 |

---

## 19. Revision history

**Revision 3 (this document)** — product-owner decisions locked after Revision 2:

| Change | Why |
|---|---|
| **§8.7 Release A rescoped to identity deployment only** | Revision 2 put the Float artifacts in Release A, which has no warm-up firewall — so `warm_up()`'s directory scan would have deserialized and run a Floating Ball model on a Digital Number Reader appliance, an application behaviour change in the release defined as having none. Float artifacts now enter the active models directory only with or after the firewall, and §8.7.2 makes the ordering a build/packaging assertion rather than a promise. |
| **§3.2 / §3.2.1 backend-specific `runtime` blocks** | One `canonical_id`, two runtimes. `built_for` moved *inside* `runtime.tensorrt` so a TensorRT platform check is structurally incapable of rejecting a Windows ONNX deployment. |
| **§7.3 `ModelCompatibilityRejected` + `policy_reason`** | `ModelModeIncompatible` named one branch of seven; six other failures would have self-described as a mode problem. The kind now says what happened to the camera, the reason code says why. |
| **§8.8 `denso-setup seed-manifest`** | An explicit, idempotent, refuse-on-conflict mutating command. `verify --repair` rejected: a diagnostic that can change what it diagnoses cannot be trusted to report the prior state. |
| **§8.3 `float-big` fallback approved** | 8.4.33 is a controlled deviation with the 8.4.21 failure captured, both versions recorded separately, the checkpoint untouched, and native `.15` inference required. Retraining is not triggered by a version difference alone. |
| **§5.1 unattached wrong-mode artifacts are a NORMAL state** | A correctly provisioned appliance carries all three families while running one mode. Reporting that as Degraded would train operators to ignore the readiness field. |
| **§6.1 `SelectableModel` made concrete** | Row and resolved identity stay one value; parallel containers indexed in step are a bug awaiting the first sort. |
| Acceptance 19a–19g added | To make each of the above assertable rather than aspirational. |

**Revision 3 addenda — after the second Codex review**, which found five further
blockers in the first draft of Revision 3. All are corrected above:

| Finding | Correction |
|---|---|
| "A staging path nothing scans" did not exist | **§8.7.1** — `/opt/denso/models` is globbed and seeded wholesale by `cmd_configure` (`denso-setup:73-84`) and enumerated by `cmd_verify` (`:226,:320`) and `cmd_replace_model` (`:426`). Resolved by the product owner as **Release A carries no Float artifact at all**; the proposed `/opt/denso/models-staging/` was rejected rather than built. |
| Schema-2 nesting would break the unmanifested check | **§3.2.2a** — `integrity.cpp:140` builds `manifested` from the **root** `g.engine`, so a schema-2 manifest would report the appliance's own declared `digitv3.engine` as `EnginesUnmanifested` → Degraded, in the release defined as changing nothing. The set is now the union of every declared runtime filename, mode- and backend-independent. Fixed in Slice 1. |
| Schema-2 nesting would silently break `--migrate-model` | **§3.2.2b** — `migrate_coordinator.cpp:83-105` dereferences root fields after `find_by_engine`; against a schema-2 generation those are empty, so the path-escape guard and both hash comparisons would compare against nothing. Schema-aware accessors added; no consumer reads a raw field. Fixed in Slice 1. |
| `Backend` had no propagation design | **§3.2.1 rule 4** — the backend is an **immutable property of `ManifestView`**, not a parameter threaded through four core APIs that already take the view. |
| "`verify` is strictly read-only" was overbroad | **§8.8** — `cmd_verify` deliberately writes a DB backup into the data dir (`denso-setup:254-286`). That is an intentional safety feature and stays. The binding claim is narrowed to what is true and testable: `verify` changes nothing under `models/`, and no `verify --repair` exists. |

Also corrected: `class_metadata_source` is a **closed vocabulary** rather than
any non-empty string; `models.approved` is **installed to
`/opt/denso/lib/models.approved`** (it is a source-tree file today and never
shipped, so an on-device `seed-manifest` could not have read it); manifest writes
are **atomic** via tmp+sync+rename and manifest comparison is **canonical, not
byte-wise**; the Slice-10 smoke fixture sets `mode.target = ball_leveler` so it
satisfies the firewall instead of contradicting it, and expects **exit 0**; the
readiness half of the idle-artifact rule moved from Slice 7 to Slice 8, which
owns the verdict; and the ordering assertion must match the **compiled symbol**,
not a comment.

**Revision 2** — after the first Codex review, which found three
release-blocking defects:

| Change | Why |
|---|---|
| **§7.0 added: warm-up is the fifth enforcement path** | `EngineRegistry::warm_up()` scans the whole models directory and calls `get()` + `infer()` on every engine regardless of attachment (`engine_registry.cpp:46-88`), long before `CameraGrid` installs any inhibit cause. Revision 1's "no incompatible engine is ever deserialized" was therefore false, and a mode-filtered `required_` set was needed to stop an incompatible attachment causing a global `app.exit(1)`. |
| **§8.7 rewritten: two releases, not one gate** | `postinst` is structural-only and seeding lives solely in `denso-setup cmd_configure`, so an ordinary upgrade would have installed enforcement onto an appliance with no manifest. The rehearsal was a gate, not a mitigation. |
| **`allowed_modes` removed from the manifest** | It duplicated the compiled matrix. It could never grant privileges, so its only possible effect was drift. §4.2's `model_declaration_conflict` branch is gone with it. |
| **§11 Leveler validation decoupled into capture-then-replay** | "Real frames through the existing capture ladder" while in `ball_leveler` would have required the `CameraStream` construction Revision 3b forbids. |
| Acceptance 12/12a/14/19a added or tightened | To make each of the above assertable rather than aspirational. |

**Revision 1** — initial draft from repository inspection and `.pt` metadata.
