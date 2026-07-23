# Model / Operating-Mode Compatibility — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Revision 3 — product-owner decisions CLOSED.** See the spec's §19 (APPROVED,
Revision 3). The decisions that shape this plan: **Release A is identity
deployment only and carries no Float artifact of any kind** (the first package
release containing one is Slice 12, and the ordering is build-enforced); the
manifest gains **per-backend runtime blocks**; the issue kind is
**`ModelCompatibilityRejected` + a real `policy_reason`**; the seeding command is
**`denso-setup seed-manifest`**, with `verify` **non-mutating with respect to
`models/`** rather than globally read-only; and an **unattached wrong-mode
artifact must not make the appliance Degraded**. No open questions remain.

**Goal:** Make model↔mode compatibility an explicit, declared, centrally-enforced
property, and produce the two Floating Ball Leveler artifacts (`float-small`,
`float-big`) with full provenance — **without** implementing the Leveler
algorithm and **without** unlocking the Ball Leveler production UI.

**Architecture:** One pure policy, `denso::models::model_compatibility(TargetMode,
ModelMetadata)`, in `denso_core`. Identity is **declared** in `manifest.json`
(schema 1 → 2) and corroborated against the loaded artifact; the mode matrix is
**compiled in and exists nowhere else** — the manifest does not carry
`allowed_modes`. Five enforcement paths — warm-up allow-list, selection list,
attachment write, runtime resolve, boot integrity — all call that one function.

**Tech Stack:** C++20 / Qt6 Core+Sql / CMake / Catch2 v3. Windows dev on MSYS2
UCRT64; native TensorRT validation on Jetson `192.168.1.15`. Python +
Ultralytics for ONNX export only (outside the CMake graph).

**Authoritative spec:** `docs/superpowers/specs/2026-07-22-model-mode-compatibility-design.md`
(Revision 3). Also binding and **not to be reinterpreted**:
`2026-07-21-operating-modes-switch-and-reset-design.md` (Revision 3b, `c7c7a6b`).

**Baseline:** `HEAD == origin/main == c7c7a6b`. Branch:
`feature/model-mode-compatibility`.

---

## Release structure — READ THIS FIRST

The manifest becomes load-bearing: without it, no model is selectable or
loadable. The packaging cannot deliver it as a side effect of an upgrade —
`packaging/debian/postinst` is **structural only by design** (`postinst:3-24`),
model seeding lives **exclusively** in `denso-setup cmd_configure`
(`denso-setup:36-90`), and `dpkg` never touches `/opt/denso/data`. Shipping
enforcement and the manifest together would therefore install enforcing code onto
an appliance with no manifest and inhibit every camera.

| | Slices | Ships | Behaviour change |
|---|---|---|---|
| **Release A** | 1–5 | schema-2 support, a declaration for the **existing `digitv3`** artifacts, `seed-manifest`, `verify` reporting that is non-mutating with respect to `models/` and manifest state, export tooling, provenance. **No Float artifact of any kind.** | **none** — no application authorization or warm-up behaviour changes |
| **Gate A** | — | upgrade rehearsal on `.15` | must pass before any Release-B slice is merged to `main` |
| **Release B** | 6–13 | the policy, all five enforcement paths, and **only then** Float artifact placement | enforcement goes live |

**No Release-B slice may merge to `main` before Gate A passes.** Release-B work
may be *developed* on the branch in parallel; it may not ship.

### The artifact-placement rule (locked, and build-enforced)

> **Release A carries no Float artifact at all.** The first package release
> containing `float-small.onnx`, `float-small.engine`, `float-small.names.json`,
> `float-big.onnx`, `float-big.engine` or `float-big.names.json` is **Slice 12**
> — after the policy, the warm-up firewall, camera-scoped enforcement, UI
> filtering, native engine smoke tests, and authorized-camera validation.

`EngineRegistry::warm_up()` scans the active models directory and deserializes
**every** runtime artifact it finds, attachment or not
(`engine_registry.cpp:46-88`). Release A has no firewall. So a Float engine
placed there during Release A would be loaded and run on a Digital Number Reader
appliance at every boot — an application behaviour change in the release defined
as having none.

During Release A, Float artifacts live **only** in the development/export
workspace, an isolated temporary directory on `.15`, and non-package artifact
storage — never in the `.deb`. A staging path inside the package was considered
and rejected (spec §8.7.1). Three mechanisms enforce this rather than trusting
it:

1. **Slice 5** adds a packaging assertion that fails the build if a Float stem is
   approved for *seeding* while the Slice-7 allow-list symbol is absent from the
   tree.
2. **`tests/packaging/run.sh`** asserts the Release A payload contains no
   `float-*` artifact anywhere, under any path, and creates no staging dir.
3. **Gate A** inspects the device's active models directory and fails if any
   Float artifact is present.

Float placement is **Slice 12**, after every enforcement slice and after
on-device validation.

---

## Global Constraints

Copied from the spec and repo hard rules. Every task implicitly includes these.

- **Schema stays v13.** No SQLite migration in any slice. Only the *manifest file
  format* goes 1 → 2. Assert `PRAGMA user_version == 13` after every slice.
- **`denso_core` never links `Qt6::Widgets`, OpenCV, ORT, or TensorRT.** The
  policy is pure: no Qt Widgets, no filesystem, no SQL, no backend.
- **The compiled family→modes registry is the ONLY place the matrix exists.** Not
  in the manifest, not in a UI `if`, not in a repo `WHERE`, not in a shell test.
- **`ball_leveler` stays an unavailable destination.** No wizard, no production
  `CameraStream`, no `DetectionProcessor`, no `ZoneHealth` wiring, no reporter.
  No slice may add a partial Leveler setup UI.
- **No Floating Ball algorithm.** No ball position, percentage, calibration or
  Leveler reporting in any slice.
- **`192.168.1.81` is RESERVED** for the user's manual `.deb` testing — never
  accessed, configured, operated or referenced by any automated or remote step.
  On-device work is `192.168.1.15` ONLY.
- **Authorized cameras: `192.168.1.185`, `.186`, `.187`, `.188`** only.
- **Credentials never appear** in console output, logs, screenshots, reports,
  specs, plans, the manifest, or commits. Reuse `logging/redact.cpp`
  `sanitize_url`; do not write a second redactor. Destructive on-device work runs
  only under an isolated `$DENSO_DATA_DIR`.
- **`models/` is git-ignored by pattern** (`*.pt`, `*.onnx`, `*.engine`,
  `*.names.json`). **Never `git add -A` in this repo** — it has swept a 38 MB
  model in before. Stage explicit paths.
- **Do not modify, stage or delete `packaging/denso-digitalreader.service`**
  (pre-existing untracked file).
- **No new `ZoneIssue::Kind`, `GlobalBlocker::Kind` or `ZoneCause` bit without a
  real producer.** This plan adds exactly one `ZoneIssue::Kind`
  (`ModelCompatibilityRejected`, real producer in Slice 8) and **no** new
  `ZoneCause` bit — it reuses `ModelUnavailable = 1u << 1`.
- **A declared, valid, UNATTACHED wrong-mode artifact is a NORMAL state.** It is
  skipped by warm-up, never deserialized, never in the fail-loud required set,
  and **must not make the appliance Degraded** (spec §5.1). A fully provisioned
  appliance carries all three families while running one mode.
- **`denso-setup verify` gains observation only.** It must not create, rewrite
  or adopt a manifest or any artifact; repair is `denso-setup seed-manifest`, and
  `verify --repair` must not be implemented. NOTE: `verify` is not a strictly
  read-only command today — it deliberately writes a DB backup dir
  (`denso-setup:254-286`), which is out of scope and stays. The testable rule is
  that `verify` changes nothing under `models/` (spec §8.8).
- **`status.json` reason codes are a FILE FORMAT**: never renumber, never reuse,
  only add.
- **Catch2 test names are CLI arguments** — ASCII only, never start with `--`,
  no `→`.
- **Build/test (Windows):** `export PATH=/c/msys64/ucrt64/bin:$PATH;
  cmake -S . -B build -G Ninja && cmake --build build && ctest --test-dir build`.
  Tag subsets via `./build/tests/denso_tests "[tag]"` — **not** `ctest -R`, which
  reports "no tests" as success.
- **Build/test (Jetson):** `cmake -S . -B build -G "Unix Makefiles"`,
  `cmake --build build -- -j4`, `QT_QPA_PLATFORM=offscreen ctest --test-dir build`.
  No `ninja`, no `sqlite3` CLI (use `python3 -c 'import sqlite3'`).
- **Do not hard-code an absolute test total.** Capture `ctest -N` before starting
  and after each slice; require every non-intentionally-skipped test to pass. The
  only permitted skip is the known Windows symlink case, which runs on the Jetson.
- **`tests/packaging/run.sh` must be run for any packaging change** — `ctest`
  covers nothing in that tree.

---

## Changed-signature caller inventory (binding)

Every slice that changes one of these signatures **must** update every caller
listed. This inventory was produced by grep over `src/` and `tests/`; a slice
whose commit does not compile standalone has not followed it.

| Signature | Production callers | Test callers |
|---|---|---|
| `health::evaluate_integrity` | `src/app/ui/startup.cpp:133`, `src/app/ui/camera/grid/camera_grid.cpp:117`, `:145`, `src/app/cli/run_headless.cpp:268` | `tests/test_integrity.cpp` — 7 calls (`:20,:29,:43,:58,:79,:96,:116`) |
| `detection::set_camera_models` | `src/app/ui/camera/wizard_controller.cpp:229` | `tests/test_detection_repo.cpp:113,:130,:132,:142,:172,:173`; `tests/test_migrate.cpp:21,:150`; `tests/test_migrate_coordinator.cpp:32` |
| `detection::detection_for` | `src/app/ui/camera/grid/camera_grid.cpp:208`, `:256` | `tests/test_detection_repo.cpp:144,:157`; `tests/test_migrate.cpp:94,:115,:131,:159,:160`; `tests/test_migrate_coordinator.cpp:76,:104,:136,:322` |
| `detection::list_models` (→ `selectable_models`) | `src/app/ui/camera/dialog/models_page.cpp:84` | `tests/test_detection_repo.cpp:51,:70` (`:42` is the `using` declaration) |
| `models::ModelGeneration` raw field access (→ accessors, Slice 1) | `src/app/cli/migrate_coordinator.cpp:83,:85,:91,:99,:105`; `src/core/health/integrity.cpp:140` | `tests/test_migrate_coordinator.cpp`, `tests/test_integrity.cpp`, `tests/test_manifest.cpp` |
| `models::resolve_model_metadata` (new, Slice 6) | called from `integrity.cpp`, `detection/repo.cpp`, `startup.cpp` — **takes no `Backend` parameter**: the backend is an immutable property of `ManifestView`, fixed at construction (spec §3.2.1 rule 4) | `tests/test_model_identity.cpp` |
| `detection::attached_model_filenames` | `src/app/ui/startup.cpp:155` | `tests/test_detection_repo.cpp:161,:165,:175` |
| `detection::try_attached_model_filenames` | `src/app/cli/run_headless.cpp:125` | `tests/test_detection_repo.cpp:180,:187,:197` |
| `EngineRegistry` ctor / `warm_up` | `src/app/ui/startup.cpp:152`, `src/app/detection/engine_registry.cpp:46-88` | integration tests (Slice 8) |

**Preferred technique:** add the new parameters with a defaulted or overloaded
form only where it does not weaken safety. For `evaluate_integrity`,
`set_camera_models` and the warm-up accessors a **default is forbidden** — a
defaulted mode would let a forgotten call site silently authorize. Update the
call sites explicitly.

---

## File Structure

New files, each created by its owning slice:

| Path | Target | Responsibility |
|---|---|---|
| `src/core/models/compatibility.{h,cpp}` | `denso_core` | `ModelMetadata`, `Verdict`, `CompatibilityResult`, the compiled registry, `model_compatibility()`, `loadable_model_files()`. Pure. |
| `src/core/models/model_identity.{h,cpp}` | `denso_core` | `ManifestView` + `resolve_model_metadata()` — joins a manifest generation with a catalog row and performs the spec §3.4 corroboration. Qt Core only. |
| `tools/export_float_onnx.py` | outside CMake | Pinned-version `.pt` → `.onnx` exporter + property assertions. |
| `tools/gen_names_sidecar.py` | outside CMake | `.names.json` generation from artifact metadata + validation. |
| `tools/gen_model_manifest.py` | outside CMake | Emits a schema-2 `manifest.json` from measured hashes/provenance. |
| `tools/replay_frames.py` *(or a small C++ harness)* | outside CMake | Slice 11: replay captured stills through an engine headlessly. |
| `tests/test_model_compatibility.cpp` | `denso_tests` | The (mode × model) matrix + every rejection branch. |
| `tests/test_model_identity.cpp` | `denso_tests` | Manifest schema-2 parse/validate + corroboration. |
| `tests/test_warmup_allowlist.cpp` | `denso_tests` | `loadable_model_files` + the mode-filtered required set. |
| `tests/test_selectable_models.cpp` | `denso_tests` | Mode-filtered catalog + attachment refusal over an in-memory DB. |
| `tests/test_model_mode_enforcement.cpp` | `denso_integration_tests` | No `DetectionProcessor`, no `EngineRegistry::get()` for a rejected model. |
| `docs/MODEL_COMPATIBILITY.md` | docs | Operator/maintainer reference: the matrix, the manifest format, how to add a model. |

Modified: `src/core/models/manifest.{h,cpp}`, `src/core/detection/repo.{h,cpp}`,
`src/core/health/integrity.{h,cpp}`, `src/app/detection/engine_registry.{h,cpp}`,
`src/app/ui/startup.cpp`, `src/app/ui/camera/dialog/models_page.{h,cpp}`,
`src/app/ui/camera/wizard_controller.cpp`, `src/app/ui/camera/grid/camera_grid.cpp`,
`src/app/cli/run_headless.cpp`, `src/core/CMakeLists.txt`, `tests/CMakeLists.txt`,
plus every test file in the caller inventory, `packaging/models.approved`,
`packaging/denso-setup`, `packaging/debian/*`, `tools/build_package.sh`,
`tests/packaging/run.sh`, `CLAUDE.md`, `AGENTS.md`, `docs/ARCHITECTURE.md`.

---

# RELEASE A — artifact delivery, no enforcement

## Slice 1 — Model identity: manifest schema 2

**Goal:** The manifest can express a canonical identity, and validates it
strictly — with schema-1 behaviour untouched.

**Dependencies:** none.

**Files**
- Modify `src/core/models/manifest.h`: add to `ModelGeneration` —
  `canonical_id`, `family`, `task`, `input_size` (`int`), `class_count` (`int`),
  a **`Runtime` block** (`OnnxRuntimeArtifact{model, model_sha256,
  class_metadata_source}` and `TensorRtArtifact{engine, engine_sha256, sidecar,
  sidecar_sha256, class_metadata_source, built_for{trt,cuda,sm}}`, each
  `std::optional`), a `Provenance` sub-struct (spec §3.2), and `bool declared`
  (true iff sourced from schema 2). **No `allowed_modes` field** — the matrix is
  compiled in (spec §3.2, §4.3). **`built_for` moves inside the TensorRT block**
  for schema 2; the schema-1 root field stays exactly where it is.
- Modify `src/core/models/manifest.cpp`: accept `schema ∈ {1,2}`; parse the
  `runtime` block and the new fields for schema 2 only; extend
  `validate_manifest` per spec §3.5; make `find_by_engine` match the root
  `engine` for schema 1 and `runtime.tensorrt.engine` for schema 2.
- **Add schema-aware accessors to `ModelGeneration`** (spec §3.2.2 rule b):
  `tensorrt_engine()`, `tensorrt_sidecar()`, `tensorrt_engine_sha256()`,
  `tensorrt_sidecar_sha256()`, `built_for_trt/cuda/sm()` — root field for
  schema 1, nested for schema 2. **No consumer may read the raw field.**
- **Modify `src/core/health/integrity.cpp:140`** — build `manifested` from
  **every** runtime artifact filename in **either** block, across all
  generations, regardless of mode or backend (spec §3.2.2 rule a). Without this,
  a schema-2 manifest makes the appliance's own declared `digitv3.engine` report
  `EnginesUnmanifested` → **Degraded**, and Release A would degrade every
  appliance it touched. **This is the one integrity change in Release A, and it
  is bookkeeping, not authorization** — no compatibility policy is consulted.
- **Modify `src/app/cli/migrate_coordinator.cpp:83,85,91,99,105`** — switch from
  `gen->engine` / `gen->sidecar` / `gen->*_sha256` to the accessors. Against a
  schema-2 generation the raw fields are empty, so the path-escape guard and both
  hash comparisons would compare against nothing (spec §3.2.2 rule b).
- Create `tests/test_model_identity.cpp` (schema portion).
- Modify `tests/test_integrity.cpp`, `tests/test_migrate_coordinator.cpp`.
- Modify `tests/CMakeLists.txt`.

**Ownership:** `denso::models`, `denso_core`, Qt Core (QJson) only.

**Test cases**
- schema 1 valid → parses, validates, `declared == false`; every existing
  schema-1 test passes byte-identically; `find_by_engine` still resolves it.
- schema 2 valid, both blocks → parses, `declared == true`, every field
  populated; `find_by_engine` resolves via `runtime.tensorrt.engine`.
- schema 2 with **only** `onnxruntime` → valid; with **only** `tensorrt` → valid;
  with **neither** (or an empty/absent `runtime`) → rejected.
- An unrecognised key under `runtime` → rejected (no silent third backend).
- `runtime.tensorrt.engine`/`sidecar` stem mismatch → rejected (the schema-1 rule
  applied inside the block).
- **Hash-field validation is two different rules — do not conflate them
  (spec §3.5):**
  - **Runtime artifact hashes** — `runtime.onnxruntime.model_sha256`,
    `runtime.tensorrt.engine_sha256`, `runtime.tensorrt.sidecar_sha256` (and the
    schema-1 root `engine_sha256` / `sidecar_sha256`) — must be **exactly 64
    lowercase hexadecimal characters**; anything else → rejected. These are the
    runtime authorization inputs: they are what `resolve_model_metadata`
    compares `models::file_sha256` against, so a malformed one must never reach
    a comparison.
  - **Provenance record fields** — `provenance.source_pt_sha256`,
    `provenance.onnx_sha256` — are required to be **non-empty** in Revision 3,
    per spec §3.5, and are **not** subject to the 64-lowercase-hex rule. They
    are descriptive provenance for humans and for the packaging approval flow;
    they are **not runtime authorization inputs** and are never compared against
    a file at boot. Assert the non-empty rule for them, and do **not** add a
    hex-format assertion the spec does not require.
- `built_for` present at the **generation root** of a schema-2 entry → rejected
  (spec §3.5 — a root copy would resurrect cross-platform rejection).
- Missing `built_for.trt` / `.cuda` / `.sm` inside the TensorRT block → rejected.
- schema 0 / 3 / `"2"` / `1.5` → rejected exactly as today.
- Each new required field missing / blank / wrong type → a distinct validation
  error naming the generation.
- `class_count != class_names.size()` → rejected.
- `canonical_id` containing `/`, `\`, `..` → rejected; duplicates rejected.
- `input_size` zero/negative → rejected.
- `provenance` absent, or with a blank required field → rejected.
- A schema-2 manifest carrying a stray `allowed_modes` key is **ignored**, not
  honoured (it must never become a second authority).
- `class_metadata_source` must be **exactly** `onnx_metadata_names` /
  `names_sidecar`; any other value — including a plausible typo — is rejected.
- **`EnginesUnmanifested` regression (spec §3.2.2a):** a schema-2 manifest
  declaring `digitv3` leaves an on-disk `digitv3.engine` **manifested**, so
  readiness stays as before. Asserted for: engine declared only in the TRT block,
  ONNX declared only in the ORT block, both declared, and — the negative control
  — a genuinely undeclared file still reported `EnginesUnmanifested`.
- The manifested set is **mode- and backend-independent**: the same manifest
  yields the same set under `digit_reader` and `ball_leveler`, and on both
  backends.
- **`--migrate-model` regression (spec §3.2.2b):** the coordinator resolves
  engine, sidecar and both hashes correctly against a **schema-2** manifest —
  path-escape guard, hash mismatch (exit 7) and success paths all behave as they
  do for schema 1. Every existing exit code (0/2/3/4/5/6/7/8) is unchanged.

**Commit boundary:** one commit — `feat(models): manifest schema 2 with declared
model identity`. Parsing only; no consumer behaviour changes.

**Rollback/failure:** an absent or schema-1 manifest must remain **not corrupt**
(spec §3.3). A regression here flips today's production Jetson from Degraded to
Blocked and is the single thing to watch in review.

**Unchanged:** `sync_models`; every `--migrate-model` exit code and its observable
behaviour; `evaluate_integrity`'s **verdict** for every input that exists today;
schema v13. **Deliberately changed:** `evaluate_integrity`'s internal
`manifested`-set construction and the coordinator's field access — both are the
minimum required to keep the two behaviours above *unchanged* once filenames
nest. No authorization logic is introduced in this slice.

---

## Slice 2 — ONNX export (`.pt` → `.onnx`, Windows, pinned versions)

**Goal:** Two ONNX files satisfying every `TrtEngine` precondition, exported by
each model's own Ultralytics version, with recorded provenance.

**Dependencies:** none. **Blocks 3–5 and 10–11.**

**Files:** create `tools/export_float_onnx.py`. No production source touched.

**Procedure**
1. `float-small.pt` → export in `D:\workspace\train_venv` (Ultralytics **8.4.33**,
   torch 2.5.1+cu121). Do not mutate that venv.
2. `float-big.pt` → a **separate isolated** venv pinned to Ultralytics **8.4.21**.
3. Arguments follow the repository's digitv3 process:
   `export(format="onnx", imgsz=640, simplify=True, opset=13)`
   (`train_model/train.py:86`). Ultralytics settled digitv3 on `opset: 12` —
   **read back the emitted `args` metadata and record the actual opset**.
4. `sha256sum` each `.pt` and each `.onnx`.

**Assertions before an ONNX is accepted**
```
metadata: task=detect, batch=1, imgsz=[640,640], channels=3
metadata args: dynamic=False, nms=False, half=False, simplify=True
metadata names == {0: "Small"}  /  {0: "Big"}
input  images  [1,3,640,640]  FP32
output output0 [1,5,8400]      FP32       ← classic YOLOv8 head, NOT [1,N,6]
```
An output of `[1,N,6]` means an end-to-end export slipped through — **stop**.

**Commit boundary:** one commit — `tools: pinned ONNX exporter for the float
models`. The `.onnx` files are **not** committed (git-ignored).

**Rollback/failure — R2 (approved controlled deviation, spec §8.3 / §18 / R2):**
8.4.21 is attempted **first**. If it cannot be installed against an available
torch, cannot load `float-big.pt`, or cannot export it, **Ultralytics 8.4.33 is
an approved controlled deviation** — not a stop, and not a silent substitution.
All of the following are mandatory conditions of using it:

- the 8.4.21 failure is **captured verbatim** into the provenance record;
- the source checkpoint is **not modified** in any way;
- `training_ultralytics: "8.4.21"` and `export_ultralytics: "8.4.33"` are
  recorded as **separate fields**, together with the captured failure and the
  reason the fallback was required — the manifest never collapses them;
- **every ONNX assertion in this slice still passes** — shape, dtype, class
  order, class count, and the `args` metadata;
- the resulting engine must later **deserialize and complete a real inference
  natively on `.15`** (Slice 10) before it is approved.

The deviation must be **explicit and recorded**; an undocumented or unrecorded
version switch is still forbidden. **Retraining is not triggered by an exporter
version difference alone** and is outside this scope — it becomes a question
only if one of the assertions above fails, or if `.15` validation shows a
correctness or compatibility failure.

**Unchanged:** every production source file, the CMake graph, the app.

---

## Slice 3 — TensorRT engine build on `.15`

**Goal:** Two `.engine` files built natively on the supported platform.

**Dependencies:** Slice 2.

**Files:** none in the repo — artifacts land in an **isolated**
`$DENSO_DATA_DIR/models` on `.15`. `tools/build_trt_engine.sh` is **not** used
(it targets an `sm_89` Windows RTX).

**Procedure**
1. `scp` the two `.onnx` to `192.168.1.15`. **`.81` is not contacted.**
2. ```
   /usr/src/tensorrt/bin/trtexec --onnx=float-small.onnx \
       --saveEngine=float-small.engine --fp16
   /usr/src/tensorrt/bin/trtexec --onnx=float-big.onnx   \
       --saveEngine=float-big.engine   --fp16
   ```
   — the recipe already recorded for `digitv3` in `packaging/models.approved`.
3. `sha256sum` both engines. Record TensorRT 10.3, CUDA 12.6, JetPack 6.2 /
   L4T R36.5, `sm_87`, and the exact command lines, read from the device.

**Test cases:** `trtexec` exits 0 for both; files non-empty and readable by the
target user; platform values read from the device, not copied from this plan.

**Commit boundary:** no code commit. A provenance record feeds Slice 4.

**Rollback/failure:** a `trtexec` failure means the ONNX is wrong — return to
Slice 2. Do not hand-edit an engine or relax `TrtEngine`'s checks. Delete any
partial `.engine` so no untested plan can be picked up by a directory scan.

**Portability:** qualified **only** for Jetson Orin Nano, L4T R36.5.0,
TensorRT 10.3, CUDA 12.6, `sm_87`. The benign `[trt] Using an engine plan file
across different models of devices` warning is **not** a portability signal.

**Unchanged:** the production data dir, the installed `.deb`, `digitv3.engine`,
every device outside `.15`.

---

## Slice 4 — `.names.json` and schema-2 manifest generation

**Goal:** Two sidecars in the repository's exact existing format, plus the
manifest generator — generated, not hand-written. **The Float artifacts produced
here remain in the development/export workspace and the isolated temporary
directory on `.15`. They are not packaged, not approved for seeding, and not
placed in any staging or application-visible directory — the first package
release containing one is Slice 12.**

**Dependencies:** Slices 2–3.

**Files:** create `tools/gen_names_sidecar.py`, `tools/gen_model_manifest.py`.

**Format** (verified against `models/digitv3.names.json`): a **bare JSON array of
strings**, index == class id.
```json
["Small"]
["Big"]
```
**No object wrapper, no metadata keys, no new structure.** `read_names_sidecar`
requires a non-empty JSON array of strings (`class_names_sidecar.cpp:36-63`).

**Test cases**
- Each sidecar parses under the real `read_names_sidecar`.
- Exactly one element; exact string; exact order.
- Count equals the engine's decoded class count: for `[1,5,8400]`, `d[1]-4 == 1`.
- Count and contents equal the manifest generation's `class_names`/`class_count`.
- `sha256sum` of each sidecar recorded into the manifest.
- The generated manifest passes Slice 1's `validate_manifest`.
- The generator emits **both** runtime blocks when both artifacts exist, and a
  single block when only one does — with `built_for` **only** inside
  `runtime.tensorrt`, never at the root.
- It emits a schema-2 declaration for the **existing `digitv3`** artifacts (the
  Release A deliverable) from `packaging/models.approved` plus measured hashes.
- When §8.3's exporter fallback was used, the deviation record is present and
  `training_ultralytics` / `export_ultralytics` differ and are both recorded.
- Negative controls: empty array, non-string element, two-element array — each
  rejected by the validator.

**Commit boundary:** one commit — `tools: names-sidecar and schema-2 manifest
generators`. Generated artifacts are **not** committed.

**Rollback/failure:** a sidecar↔engine class-count mismatch is a **stop** —
AGENTS.md records that a stale sidecar keeps an old class order and makes
readings silently *wrong* rather than failing. Never hand-edit a sidecar to make
a check pass.

**Unchanged:** `digitv3.names.json` and every existing sidecar consumer.

---

## Slice 5 — Packaging: `seed-manifest`, non-mutating `verify`, digitv3 declaration

**Goal:** Every appliance ends up with a correct schema-2 `manifest.json`
declaring **its existing `digitv3` artifacts**, before any code depends on one —
and **no Float artifact reaches the active models directory.**

**Dependencies:** Slices 1, 4.

**Files**
- Modify `tools/build_package.sh` + `packaging/debian/*`:
  - add `manifest.json` to the payload, `install -m 0644` (never `>`;
    `dpkg-deb` normalizes *control* modes but **not** payload modes);
  - **install `packaging/models.approved` to `/opt/denso/lib/models.approved`**
    (`0644`, beside `policy.sh`). It is a source-tree file today and never ships
    (`build_package.sh:181-193`), so an on-device `seed-manifest` cannot read it
    (spec §8.8);
  - **Release A carries NO Float artifact — locked.** No `float-*.onnx`,
    `float-*.engine` or `float-*.names.json` in the `.deb`, approved for
    seeding, installed under `/opt/denso`, or in any application-visible
    directory. **No staging directory is created or used** (spec §8.7.1).
- Modify `packaging/denso-setup`: add **`cmd_seed_manifest`** implementing the
  spec §8.8 contract — atomic single-file write (`.manifest.json.tmp` → `sync` →
  `mv`; `install_pair` is engine+sidecar specific and cannot be reused), and
  **canonical-content** comparison of an existing manifest, not byte comparison.
  Extend `cmd_verify` — **observation only** — to report manifest presence,
  packaged-vs-data-dir agreement, per-artifact hash state, and the §5.1
  informational idle-artifact list.
- Modify `tests/packaging/run.sh`: assert the payload file and its `0644` mode,
  its presence in `SHA256SUMS`, **and that no `float-*.engine` / `float-*.onnx`
  is staged into the active models dir.**
- **Do NOT** add the Float lines to `packaging/models.approved` in this slice —
  that is Slice 12 (see the ordering assertion below).

**`seed-manifest` contract (locked, spec §8.8)**

| Situation | Behaviour |
|---|---|
| no manifest, artifacts match the approved set | generate + seed |
| manifest present and identical | no change, "already current", exit 0 |
| manifest present but **differing** | refuse, overwrite nothing, non-zero exit |
| artifacts do not match the approved set | refuse, seed nothing |
| run twice | second run is a no-op |

Explicitly mutating; never invoked by a maintainer script; does not run
configure; touches no ownership/autostart/autologin/recorded user; runs as the
target user for data-dir writes; **needs no credential**.

**Ordering assertion (spec §8.7.2):** the packaging build fails if a `float-*`
stem is approved for seeding while the Slice-7 allow-list is absent. It must test
for the **compiled symbol** — the `loadable_model_files` definition in
`src/core/models/compatibility.cpp` **and** its use in `src/app/ui/startup.cpp` —
not for a comment or declaration, which a stray mention could satisfy. Add it
here so it is live before a Float line could ever be added.

**Ownership:** the packaging tree, which `ctest` does not cover.

**Test cases**
- `tests/packaging/run.sh` passes (130 native / 124 on MSYS2, plus the new
  assertions).
- A fresh `configure` seeds engine + sidecar + manifest together.
- `seed-manifest` on an appliance with `digitv3` artifacts and no manifest →
  generates a valid schema-2 declaration for them.
- `seed-manifest` run twice → **idempotent**; second run reports "already
  current" and changes no byte.
- `seed-manifest` against a differing manifest → **refuses**, overwrites nothing,
  names what differs, exits non-zero.
- `seed-manifest` when data-dir artifacts do not match the approved set →
  refuses, seeds nothing.
- **`verify` changes nothing under `models/`** — a hash + mtime snapshot of the
  `models/` subtree is identical before and after, excluding the pre-existing
  DB-backup directory `verify` writes elsewhere in the data dir. A required
  assertion, not a review note.
- No `verify --repair` entry point exists (grep assertion).
- `verify` distinguishes missing, differing and matching manifests.
- `verify` lists an idle wrong-mode artifact informationally without changing the
  overall PASS.
- **The Release A payload contains no `float-*` artifact anywhere** — no
  `.onnx`, no `.engine`, no `.names.json`, under any path — and creates no
  staging directory. Asserted by full payload inspection.
- `models.approved` is installed at `/opt/denso/lib/models.approved`, `0644`.
- `seed-manifest` writes atomically: an interrupted run leaves either the old
  manifest or the new one, never a truncated file, and never a stray `.tmp`.
- An existing manifest that is **reformatted but canonically equivalent** is
  treated as "already current", not as a conflict.
- The ordering assertion fires: adding a `float-*` line to `models.approved`
  without the Slice-7 compiled symbol fails the build; a comment mentioning
  `loadable_model_files` does **not** satisfy it.
- `tests/manual/repro_build.sh` — a clean build stays byte-reproducible with the
  new payload file. **Must run exclusively** (it refuses a dirty tree and makes
  then reverts its own `packaging/lib/policy.sh` edits).

**Commit boundary:** one commit — `feat(packaging): seed-manifest and schema-2
manifest delivery`. **This is the Release A cut.**

**Rollback/failure:** a seeding defect here is recoverable because nothing yet
depends on the manifest — exactly why this slice precedes enforcement. A defect
that lets a Float artifact into the active models dir is **not** recoverable in
the same sense: it changes boot behaviour on a Digital Number Reader appliance.
That is what the three ordering mechanisms exist to prevent.

**Unchanged:** all application behaviour. No app source file is modified.

---

## GATE A — upgrade rehearsal on `.15` (blocking)

On `192.168.1.15`, isolated `DENSO_DATA_DIR`, production data dir untouched.
**`.81` is not contacted.**

- [ ] Simulate an existing commissioned installation: `digitv3.engine` +
      `digitv3.names.json`, **no manifest** — the current production state.
- [ ] Install the Release A package over it.
- [ ] `sudo denso-setup seed-manifest` → a correct schema-2 declaration for the
      **existing `digitv3` artifacts** appears, **without re-running full
      `configure`**.
- [ ] Re-run `seed-manifest` → **idempotent**; reports "already current",
      changes no byte.
- [ ] `seed-manifest` against a hand-modified manifest → **refuses**, overwrites
      nothing.
- [ ] `denso-setup verify` → reports the matching manifest, and a hash + mtime
      snapshot of the **`models/` subtree** is **identical before and after**
      (excluding the DB-backup dir `verify` deliberately creates).
- [ ] **`digitv3` still loads**: `denso --check` passes as before.
- [ ] **No camera is newly inhibited.**
- [ ] **No Float `.engine` or `.onnx` is present in the active models
      directory** — listed and asserted explicitly.
- [ ] Application behaviour is otherwise **unchanged**.

**No Release-B slice merges to `main` until every box above is ticked.** If
seeding cannot reach an existing appliance, stop and escalate — do not proceed to
enforcement.

---

# RELEASE B — enforcement

## Slice 6 — The central compatibility policy + unit tests

**Goal:** One pure function answering every (mode, model) question, with the
compiled registry as the sole home of the matrix.

**Dependencies:** Slice 1. **Gate A must pass before this merges to `main`.**

**Files**
- Create `src/core/models/compatibility.{h,cpp}` — `ModelMetadata`, `Verdict`,
  `CompatibilityResult`, the compiled registry, `model_compatibility()`, and
  `loadable_model_files()`.
- Create `src/core/models/model_identity.{h,cpp}` — `ManifestView` +
  `resolve_model_metadata(view, DetectionModel, PlatformInfo)` performing the
  spec §3.4 corroboration **for the view's backend only**. `Backend` is
  `{OnnxRuntime, TensorRt}` and is an **immutable property of `ManifestView`**,
  fixed at construction by the single `#ifdef _WIN32` split the tree already
  uses (spec §3.2.1 rule 4). It is deliberately **not** a parameter on
  `resolve_model_metadata`, `evaluate_integrity`, `set_camera_models`,
  `detection_for` or `selectable_models` — all of which already take the view —
  because `denso_core` must not infer the application's backend and four extra
  parameters would be four more places to get it wrong. This function is where
  the backend is resolved away: `model_compatibility` never sees one.
- Create `tests/test_model_compatibility.cpp`; extend `tests/test_model_identity.cpp`.
- Modify `src/core/CMakeLists.txt`, `tests/CMakeLists.txt`.

**API:** exactly spec §4.1. Evaluation order exactly spec §4.2. Registry exactly
spec §4.3 — one static table, with a static assertion that every registered
family has a non-empty mode set. `compatibility.*` has **no** Qt include at all.

**Test cases** — tagged `[compat]`:
- `digit_reader + digitv3` → `Allowed`.
- `digit_reader + float-small` / `+ float-big` → `RejectedWrongMode`.
- `ball_leveler + float-small` / `+ float-big` → `Allowed`.
- `ball_leveler + digitv3` → `RejectedWrongMode`.
- `declared == false` → `RejectedUnknown` / `model_undeclared`, **both modes**.
- unknown `canonical_id` → `model_unknown_id`, both modes.
- declared family ≠ registry family → `model_family_mismatch`.
- `task != "detect"`, `input_size != 640` → `model_shape_unsupported`.
- class names reordered / renamed / wrong count → `model_classes_mismatch`.
- `provenance_ok == false` → `model_provenance_failed`.
- **Precedence:** undeclared *and* wrong-mode reports `model_undeclared`.
- A default-constructed `CompatibilityResult` has `allowed() == false`.
- Every `reason_code` unique, lowercase-snake, stable (table-driven test that
  fails if a string changes).
- `resolve_model_metadata`: no matching generation → `declared == false`; hash
  mismatch → `provenance_ok == false`.
- **Backend resolution (spec §3.2.1) — the locked cross-platform cases:**
  - `Backend::TensorRt` resolves `filename` to the `.engine`, hashes engine **and**
    sidecar, and checks `built_for`.
  - `Backend::OnnxRuntime` resolves `filename` to the `.onnx`, hashes only the
    ONNX, and **never reads `built_for`**.
  - **A manifest whose `runtime.tensorrt.built_for` is deliberately wrong still
    resolves `Allowed` under `Backend::OnnxRuntime`** — the assertion that a
    TensorRT platform check cannot reject a Windows deployment.
  - The same wrong `built_for` yields `RejectedProvenance` under
    `Backend::TensorRt`.
  - A generation with only an `onnxruntime` block → `declared == false` under
    `Backend::TensorRt` (`model_undeclared`), and fully declared under
    `Backend::OnnxRuntime`. And the mirror case.
  - The **same `canonical_id`** resolves on both backends: identity is
    platform-independent, only the artifact differs.
- `loadable_model_files` returns only allowed filenames — the **active backend's**
  filenames — and `{}` for an undeclared catalog.

**Commit boundary:** one commit — `feat(models): central model/mode compatibility
policy`. No production caller yet: dead code with live tests, deliberately, so it
reviews on its own.

**Rollback/failure:** nothing calls it; a defect cannot affect a running
appliance. Single-commit revert.

**Unchanged:** everything.

---

## Slice 7 — Warm-up firewall: allow-list + mode-filtered required set

**Goal:** No rejected engine is ever deserialized, and no rejected attachment can
abort startup. **This is the slice the whole safety claim rests on.**

**Dependencies:** Slice 6.

**Why it exists:** `EngineRegistry::warm_up()` iterates **every**
`*.engine`/`*.onnx` in the models directory and calls `get()` then
`infer(blank)` on each, regardless of attachment
(`engine_registry.cpp:46-88`). Its `required_` fail-loud set comes from
unfiltered `attached_model_filenames` (`startup.cpp:155`). Both run at
`startup.cpp:152`, long before `CameraGrid` installs any inhibit cause
(`camera_grid.cpp:198`). Filtering `detection_for` alone is far too late.

**Files**
- Modify `src/app/detection/engine_registry.{h,cpp}`: the registry takes an
  **allow-list**; the directory scan skips any filename not in it. `get()` on a
  non-allowed filename is a programming error, not a silent load.
- Modify `src/core/detection/repo.{h,cpp}`: `attached_model_filenames` and
  `try_attached_model_filenames` take the mode + manifest view and exclude
  rejected models. **No defaulted parameter** — a forgotten call site must fail
  to compile, not silently authorize.
- Modify `src/app/ui/startup.cpp:152-156`: build the allow-list via
  `loadable_model_files` and pass the filtered required set.
- Modify `src/app/cli/run_headless.cpp:125`: same for `--check`.
- Update `tests/test_detection_repo.cpp:161,:165,:175,:180,:187,:197`.
- Create `tests/test_warmup_allowlist.cpp`.

**Test cases**
- `loadable_model_files` excludes a rejected model; the scan therefore never
  calls `get()` for it — asserted with a counting stub.
- **The unattached case:** `float-small.engine` present in the models directory,
  attached to nothing, mode `digit_reader` → **zero** `get()` calls, zero
  `infer()` calls for it. (This is the case Revision 1 of the spec missed.)
- An incompatible *attachment* is absent from `required_`, so `warm_up()` does
  **not** throw and the app does **not** `exit(1)`.
- An **allowed** attached model that is missing still throws — the fail-loud
  contract is unchanged. Regression-guarded explicitly.
- `digitv3` in `digit_reader` is warmed exactly as today.
- Windows (`.onnx`) and Jetson (`.engine`) branches both honour the allow-list,
  each using its own backend's filenames.
- **The idle-artifact rule, warm-up half (spec §5.1):** a declared, valid,
  unattached wrong-mode engine on disk is skipped, never `get()`-ed, never in
  `required_`, and emits **at most one** redaction-safe informational line per
  boot, visible in verbose `--check`.
  *(The readiness half — that this leaves the verdict `Ready`, exit 0 — is
  asserted in **Slice 8**, which owns the `evaluate_integrity` signature and
  compatibility wiring. Slice 7 must not assert a verdict it does not yet
  produce. What Slice 7 does guarantee is that Slice 1's manifested-set fix
  already keeps such a declared file out of `EnginesUnmanifested`.)*

**Commit boundary:** one commit — `feat(detection): compatibility allow-list for
engine warm-up`. First Release-B behaviour change.

**Rollback/failure:** over-filtering means an allowed model is not warmed → the
camera shows "Preparing model…" or the fail-loud check fires. Loud, not silent.
**Not acceptable** is the reverse: a rejected model reaching `get()`. The
counting-stub test makes that a hard failure.

**Unchanged:** the engine-only/no-fallback contract for allowed models; the cold
splash vs warm UI-first decision; `sync_models`' directory scan (deliberately
retained per the slice-b contract).

---

## Slice 8 — Camera-scoped backend enforcement

**Goal:** An incompatible attachment — however it entered the database —
inhibits exactly its own camera.

**Dependencies:** Slices 6–7.

**Files**
- Modify `src/core/health/integrity.h`: add
  `ZoneIssue::Kind::ModelCompatibilityRejected` + `reason_code`
  `"model_compatibility_rejected"`, and a **`QString policy_reason`** field on
  `ZoneIssue` carrying the policy's own stable code; `evaluate_integrity` takes
  `TargetMode` + the manifest view.
- Modify `src/core/health/integrity.cpp`: resolve every `camera_model` join row
  through `resolve_model_metadata` + `model_compatibility`; emit the issue with
  `camera_id`, the verbatim `policy_reason`, and a redaction-safe `detail`.
  **The kind is not named after the wrong-mode branch** — six other rejection
  causes flow through it, and `model_mode_incompatible` is only one of the seven
  reason codes (spec §7.3).
- Modify `src/core/health/status_file.cpp` (or wherever issues are serialized):
  emit `policy_reason` alongside the kind's reason code. Both are a file format.
- Modify `src/core/detection/repo.{h,cpp}`: `set_camera_models(..., mode,
  manifest_view)` refuses and rolls back if any model is not `Allowed`;
  `detection_for(..., mode, manifest_view)` returns an empty `models` set when
  any attached model is rejected.
- Modify `src/app/ui/camera/grid/camera_grid.cpp:117,:145,:201-205,:208,:256`:
  new signature; map the new issue kind onto the existing
  `ZoneCause::ModelUnavailable` beside the `EngineMissing` mapping; `start_one`
  skips `DetectionProcessor` for an inhibited camera.
- Modify `src/app/ui/startup.cpp:133`, `src/app/cli/run_headless.cpp:268`.
- Modify `src/app/ui/camera/wizard_controller.cpp:229`: pass the committed mode;
  surface a refusal as a named error, not a generic write failure.
- **Update every test in the caller inventory:** `tests/test_integrity.cpp` (7
  calls), `tests/test_detection_repo.cpp`, `tests/test_migrate.cpp`,
  `tests/test_migrate_coordinator.cpp`.
- Create `tests/test_selectable_models.cpp` (attachment portion) and
  `tests/test_model_mode_enforcement.cpp` (integration).

**Test cases**
- `set_camera_models` with a rejected model → false, and `camera_model` /
  `camera_model_class` row counts identical before and after.
- `set_camera_models` with an allowed model → unchanged from today.
- Directly-written incompatible `camera_model` row (the restored-DB case) →
  `ModelCompatibilityRejected` with the correct `camera_id`, readiness
  **Degraded (10)**, not Blocked (78).
- **One case per `policy_reason`** — `model_undeclared`, `model_unknown_id`,
  `model_family_mismatch`, `model_shape_unsupported`, `model_classes_mismatch`,
  `model_provenance_failed`, `model_mode_incompatible` — each asserted to arrive
  with its own correct code. A hash mismatch must **not** report itself as a mode
  problem, and only the genuine wrong-mode case may use
  `model_mode_incompatible`.
- A declared, valid, **unattached** wrong-mode artifact leaves readiness `Ready`
  (spec §5.1) — the counterpart to the attached case above.
- Two cameras, one incompatible: only that one is inhibited.
- A camera with a **mix** of allowed and rejected models is inhibited as a whole.
- Integration: a counting `EngineRegistry` stub records **zero** `get()` calls
  and zero `DetectionProcessor` constructions for that camera.
- `status.json` contains `model_compatibility_rejected` **plus the correct
  `policy_reason`**, the camera id, and **no** credential — asserted by scanning
  the emitted file for `rtsp://`, `@`, and the test password.
- `--check` exits 10 for this condition, not 0 and not 78.
- `reason_code(ModelCompatibilityRejected)` is exactly
  `"model_compatibility_rejected"`, and every `policy_reason` string is stable
  (table-driven format test).

**Commit boundary:** one commit — `feat(models): camera-scoped model/mode
enforcement`.

**Rollback/failure:** the failure mode is over-rejection — a correct `digitv3`
inhibited by a resolution bug. Loud (banner + status.json + exit 10), single-commit
revert.

**Unchanged:** compatible `digit_reader` cameras; the zone namespace; the brazing
payload; switch-and-reset; `EnginesUnmanifested` stays Degraded and non-blocking;
schema v13.

---

## Slice 9 — UI filtering seam (Ball Leveler UI stays locked)

**Goal:** The model-selection list is produced by the policy, for the committed
mode — with no Leveler UI exposed.

**Dependencies:** Slices 6–8.

**Files**
- Modify `src/core/detection/repo.{h,cpp}`: add `selectable_models(db, mode,
  manifest_view)` → `SelectableModel{DetectionModel row; models::ModelMetadata meta;}`
  for allowed models only.
- Modify `src/app/ui/camera/dialog/models_page.{h,cpp}:84`: call
  `selectable_models`; the page holds **no** rule.
- Modify `src/app/ui/camera/wizard_controller.cpp`: supply the committed mode
  from `mode::load_target(db)`.
- Extend `tests/test_selectable_models.cpp`; update
  `tests/test_detection_repo.cpp:42,:51,:70`.

**Test cases**
- All three models catalogued, `digit_reader` → exactly `[digitv3]`.
- Same catalog, `ball_leveler` → exactly `[float-small, float-big]`, ordered by
  catalog id.
- An undeclared model → absent in **both** modes.
- Empty/absent manifest → empty list in both modes (fail-closed).
- **Committed-mode test:** `mode.target = digit_reader` stored while a *different*
  value sits in the settings-page selector widget → the list still reflects
  `digit_reader` (spec §6.3, acceptance 13).
- The list filter and Slice 8's enforcement agree: a previously-attached,
  now-rejected model does not vanish from the list without the camera also being
  inhibited.
- **Ball Leveler UI lock regression:** in `ball_leveler` the camera dialog is not
  reachable, the top-bar Camera button stays disabled, and no `ModelsPage` is
  constructed. This test guards Revision 3b §2.1 against this very slice.

**Commit boundary:** one commit — `feat(ui): filter model selection through the
compatibility policy`.

**Rollback/failure:** worst case is an empty Models step in `digit_reader` —
visible, and caught by Slice 11's regression set. Enforcement (Slices 7–8)
remains safe without the filter.

**Unchanged:** the wizard's five-page flow, class-selection union logic, per-class
confidence, the Areas step, and the Ball Leveler unavailability.

---

## Slice 10 — Headless engine smoke tests on `.15`

**Goal:** Prove the app itself can load and run each float engine — headless,
with the Ball Leveler UI still locked.

**Dependencies:** Slices 3–4, 6–7.

**The fixture must satisfy the firewall, not bypass it.** Slice 7 exists to stop
Float engines loading on a `digit_reader` appliance — so a smoke test that seeds
them and expects them to load must run in a context where the policy **allows**
them. The isolated database's committed `mode.target` is therefore set to
**`ball_leveler`**, which is exactly the mode in which `float-small`/`float-big`
are allowed. This unlocks no UI: `ball_leveler` remains an unavailable
destination with no wizard, no `CameraStream`, no processor and no reporter, and
`--check` is headless. A fixture that instead weakened the allow-list would be
testing a build nobody ships.

**Procedure** (on `.15`, `DENSO_DATA_DIR=/tmp/denso-float-smoke`,
`QT_QPA_PLATFORM=offscreen`)
1. Build on-device; run `QT_QPA_PLATFORM=offscreen ctest --test-dir build`.
2. Seed the isolated data dir with both engines, both sidecars and a schema-2
   `manifest.json` **declaring both**; set `mode.target = ball_leveler` in the
   isolated DB (`python3 -c 'import sqlite3'`).
3. `./build/src/app/denso --check` → engines constructed through the real
   `TrtEngine` ctor (bindings, shapes, sidecar validated).
4. **One real inference per engine** — `--check` deliberately never calls
   `infer()`, so deserializing is not inferring. Assert output `[1,5,8400]`, the
   `decode_yolo` branch (not `decode_yolo_end2end`), `num_classes == 1`.

**Test cases**
- Both engines deserialize, and **`--check` exits 0**. Exit 10 is a **failure**
  here, not an accepted outcome: the fixture's manifest declares both engines, so
  `EnginesUnmanifested` would mean the Slice-1 manifested-set fix is wrong.
- **The mirror case, proving the firewall is live:** the same data dir with
  `mode.target = digit_reader` → neither Float engine is deserialized, `--check`
  still exits 0, and the §5.1 informational lines appear.
- One inference per engine completes without a CUDA error.
- Decode branch and class count asserted, not assumed.
- A deliberately corrupted engine byte → `TrtEngine` ctor throws and `--check`
  fails loud. Proves the fail-loud contract still holds.
- `/opt/denso/data` mtimes unchanged — the production data dir is not touched.

**Commit boundary:** no production commit. Results feed Slice 13.

**Rollback/failure:** any failure stops the pipeline here. Do not proceed to
camera validation with an engine that will not load.

**Unchanged:** `digitv3` continues to pass `--check` in the same run.

---

## Slice 11 — Authorized-camera validation (capture, then replay)

**Goal:** Real frames from the authorized cameras flow through each engine, and
`digit_reader` behaviour is provably unchanged — **without** constructing any
Leveler pipeline.

**Dependencies:** Slice 10.

**Cameras:** `192.168.1.185`, `.186`, `.187`, `.188` only. Credentials and RTSP
configuration read from the project's isolated test database. **`.81` is not
contacted.**

**Why capture-then-replay:** feeding live frames to a float engine while the
appliance is in `ball_leveler` would require the `CameraStream`/processor
construction Revision 3b forbids. Capture and mode are therefore decoupled.

**Part A — `digit_reader` regression (live, as today)**
- Models step lists `digitv3` and only `digitv3`; the float models are absent.
- Attach `digitv3`, stream from the authorized cameras, confirm digit reading is
  unchanged.
- Write an incompatible `float-small` attachment directly into the isolated DB
  (`python3 -c 'import sqlite3'` — no `sqlite3` CLI on-device), restart, and
  confirm: that camera is inhibited with `model_compatibility_rejected` /
  `policy_reason = model_mode_incompatible`, its engine is never loaded, the app
  does **not** exit, and the other cameras keep streaming and reporting.

**Part B — float engines, frames from file**
1. **Capture** stills from each authorized camera using the existing snapshot
   path or a standalone OpenCV/GStreamer utility. Capture only: no
   `CameraStream`, no `FrameProcessor`, no grid, no reporter, not tied to a mode.
2. **Replay** the saved frames through `float-small` and `float-big` in the
   Slice-10 headless harness. Assert the `decode_yolo` branch and
   `num_classes == 1`; **record observed latency and FPS** per engine per camera.

**Part C — mode lock, no engines involved**
- `ball_leveler` still persists, still lands on "not available in this release",
  and still constructs no wizard, no production `CameraStream`, no
  `DetectionProcessor`, no reporter.
- `digitv3` is non-selectable and policy-rejected there.

**Evidence and hygiene**
- Per camera: frames captured > 0, inference completed, latency and FPS recorded.
- **Redaction check:** every command output, log and report is scanned for
  `rtsp://`, `@`, the usernames and the passwords before being written down.
  Report redacted URLs only.
- Captured frames are real images from the authorized cameras: stored only under
  the isolated data dir and deleted with it.
- The isolated data dir is removed and the device restored afterwards.

**Explicitly not claimed:** any ball position, level percentage, calibration, or
final Leveler output. Detections are boxes; nothing interprets them.

**Commit boundary:** no production commit. Results feed Slice 13.

**Rollback/failure:** a camera that will not open is an environment issue —
record it, do not weaken an assertion. A `digit_reader` regression is a **stop**
and reverts Slice 8 or 9.

---

## Slice 12 — Float artifact placement into the active models directory

**Goal:** `float-small` and `float-big` become installable, seeded artifacts —
**only now**, with the firewall, enforcement and validation all already in place.

**Dependencies:** Slices 7 (firewall — hard), 8, 9, 10, 11. **This slice must not
land before Slice 7 under any circumstance** (spec §8.7.1).

**Files**
- Modify `packaging/models.approved`: add the `float-small` and `float-big` lines
  (stem, engine SHA-256, sidecar SHA-256, trtexec recipe) — existing format.
  This is the first moment these stems become approved for seeding, and the
  Slice-5 ordering assertion permits it only because the Slice-7 allow-list
  symbol now exists.
- Modify `tools/build_package.sh` / `packaging/debian/*`: install the Float
  artifacts into `/opt/denso/models` from the build host. **This is the first
  package release to contain any `float-*` artifact** (spec §8.7.1), and the
  moment they become seedable.
- Modify `packaging/denso-setup`: seed the Float engine/sidecar pairs alongside
  `digitv3`, using the existing `seed_decision_pair` pair-wise decision; extend
  the schema-2 manifest to declare all three generations.
- Modify `tests/packaging/run.sh`: the Release A "no Float in the active models
  dir" assertion is **replaced** by its Release B counterpart — Float artifacts
  present, approved, hash-matched, and paired with their sidecars.

**Test cases**
- The ordering assertion passes only because the allow-list symbol is present;
  a test that removes it must fail the build.
- Fresh `configure` seeds all three engine/sidecar pairs plus a three-generation
  manifest.
- `seed-manifest` on an appliance already carrying all three is idempotent.
- On a `digit_reader` appliance with all three artifacts present and only
  `digitv3` attached: warm-up loads **only** `digitv3`; the Float engines are
  skipped; readiness is **`Ready` / exit 0** with the §5.1 informational lines.
- `verify: PASS` with all three pairs.
- `tests/manual/repro_build.sh` still passes (**exclusive run**).

**Commit boundary:** one commit — `feat(packaging): seed the Floating Ball model
artifacts`. **This is the Release B artifact cut.**

**Rollback/failure:** reverting this commit returns the appliance to a
digitv3-only artifact set with enforcement still correctly in place — a safe
state, because the policy does not require the Float artifacts to exist.

**Unchanged:** all enforcement behaviour; `digitv3` operation; the Ball Leveler
UI lock.

---

## Slice 13 — Documentation and final regression

**Goal:** The rules are written down and the whole suite is green on both
platforms.

**Dependencies:** Slices 1–12.

**Files**
- Create `docs/MODEL_COMPATIBILITY.md`: the matrix, the schema-2 manifest format
  **including the per-backend `runtime` blocks**, the **five** enforcement
  points, the seven `policy_reason` codes and what each means, `seed-manifest`
  vs non-mutating `verify`, the §5.1 idle-artifact rule, how to add a model (a
  **code change** — spec §4.3), the two-release / artifact-placement ordering
  rule, and the portability boundary.
- Modify `CLAUDE.md`, `AGENTS.md`, `docs/ARCHITECTURE.md`: the policy, the new
  issue kind, the warm-up allow-list, the manifest schema bump, the R1
  deployment requirement.

**Test cases**
- Windows: full build + `ctest`; every non-skipped test passes (only the known
  symlink skip).
- Jetson `.15`: full build + offscreen `ctest`; the symlink case runs for real.
- `tests/packaging/run.sh` passes.
- `tests/manual/repro_build.sh` passes (**exclusive run**).
- `PRAGMA user_version == 13` on a freshly migrated DB.
- A grep-able assertion that the mode matrix appears in exactly one source file,
  that `allowed_modes` appears in no manifest artifact, and that no
  `verify --repair` entry point exists.

**Commit boundary:** one commit — `docs: model/mode compatibility reference`.
Then merge `--no-ff`.

**Rollback/failure:** documentation-only; safe to iterate.

**Unchanged:** the `.deb` layout, the launcher, the preflight guard's contract,
`digitv3` behaviour on a correctly provisioned appliance, schema v13.

---

## Review checkpoints

- **After Slice 1** — is a schema-1/absent manifest still merely Degraded? Do the
  per-backend blocks parse and validate, with `built_for` refused at the root?
- **After Slice 5 / GATE A** — does an existing appliance actually receive a
  declaration for its **existing `digitv3`**? Is `verify` provably non-mutating?
  Is the active models directory **free of Float artifacts**? This is the go/no-go
  for the whole of Release B.
- **After Slice 6** — is the matrix in exactly one place, is the evaluation order
  fail-closed, is every reason code stable, and can a TensorRT `built_for`
  mismatch provably **not** reject a Windows ONNX deployment?
- **After Slice 7** — is the engine firewall real? Specifically: an unattached
  rejected engine in the models directory is never `get()`-ed, an incompatible
  attachment cannot `exit(1)` the app, and a declared idle wrong-mode artifact
  leaves the appliance **`Ready`, not Degraded**.
- **After Slice 8** — camera-scoped, Degraded not Blocked, the **correct
  `policy_reason`** per case, no credential in `status.json`.
- **After Slice 9** — Ball Leveler UI still locked; availability follows the
  committed mode.
- **After Slice 11** — `digit_reader` unchanged; no credential in the evidence;
  no Leveler pipeline was ever constructed.
- **After Slice 12** — Float artifacts are seeded, and a `digit_reader` appliance
  carrying all three families still warms only `digitv3` and still reports
  `Ready`.
- **Before merge** — full regression on both platforms.

Codex reviews each slice, per the standing collaboration cadence on this repo.
