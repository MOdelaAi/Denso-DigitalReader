# Packaged manifest descriptors

Hand-maintained **packaging inputs** — the same class of file as
`packaging/models.approved` and `packaging/debian/control.in`. They are NOT
generated artifacts and NOT candidates; they are the reviewed source from which
the release `manifest.json` is produced at package-build time.

## What `<stem>.descriptor.json` is

A generation descriptor for `tools/gen_model_manifest.py`. It declares an
artifact's **identity** (`canonical_id`, `family`, `task`, `input_size`), its
TensorRT `built_for` platform triple, the **expected** engine/sidecar SHA-256
(which must equal the `packaging/models.approved` row for that stem), fully
**cited** provenance (`provenance` + `provenance_evidence`, every value measured,
never invented), and an **`approval`** block.

## Engine-only artifact policy

The production artifact pair is `<model-id>.engine` + `<model-id>.names.json`,
and the approved **engine bytes are the provenance authority**. No `.onnx` or
`.pt` is required, packaged, searched for or relied upon — not by runtime
loading, approval, provenance enforcement, manifest generation, packaging,
deployment or acceptance testing.

`gen_model_manifest.py` therefore **refuses** any descriptor carrying a
source-chain provenance key (`onnx*`, `source_pt*`, `training_ultralytics`,
`export_ultralytics`, `export_onnx_command`), so a generated manifest cannot
publish a lineage claim this project does not make. The C++ parser stays
tolerant of those keys purely so a manifest already installed on an appliance
keeps validating; generation is where the line is held.

## `approval` — repo-controlled, never published

Each TensorRT descriptor carries an `approval` block recording the **direct
on-device validation of those exact engine bytes**: the device, the
TRT/CUDA/SM platform, both SHA-256 values, and an observation for every check in
`APPROVAL_CHECKS` (regular file, sha256 recorded, deserialization, one real
synthetic inference, input/output bindings and shapes, class count vs the
canonical sidecar, sidecar present and valid, identity and family, decoder match,
target platform).

It is validated against the hashes the generator just **measured** — so an
approval record can never drift onto a different artifact — and is deliberately
**not** emitted into `manifest.json`: the runtime authorizes on hashes, not on a
narrative.

A rebuilt engine is a **new artifact, not a hash correction**. TensorRT plans may
differ byte-for-byte after a rebuild; that is expected and is not grounds for
rejection, but the new bytes need their own explicit approval before
`models.approved` and the descriptor change. Hash enforcement is never relaxed.

The descriptor deliberately omits `tensorrt.engine_path` / `tensorrt.sidecar_path`:
`tools/build_package.sh` injects them at build time (via a Python JSON edit, never
a text substitution) so the manifest is generated against the **exact** engine and
sidecar being packaged. The generator re-measures those files and refuses on any
hash mismatch against the descriptor's `expected_*` values — so the manifest can
never be silently regenerated from different model bytes.

## Reproducibility and the identity assertion

`gen_model_manifest.py` reads no clock (`installed_utc` is an explicit, fixed
input), and pins key order, separators and the trailing newline. Identical inputs
therefore yield byte-identical output. `build_package.sh` additionally asserts the
generated `digitv3` manifest hashes to
`1e6eb46206dcc03352496f3643f94d4e83927645a4a6396c869c5fe9b6c27e91`, and the
three-model set to
`ca8e9d6d991e52e0845060e9b75b1b7d393460abaef229ed1a5030a590cc7c16`. Any edit to a
descriptor that changes the output will fail the build until the new candidate is
re-reviewed and that constant is updated. **Both constants changed when the
engine-only policy dropped the source-chain fields from the emitted manifest** —
the artifacts themselves did not move.

## `installed_utc`

`installed_utc` is the **release artifact's recorded installation epoch** for this
declaration (`2026-07-23T00:00:00Z` for the Release-A digitv3 cut). It is **not**
the package build time and **not** the appliance install time; it is fixed so the
manifest is byte-reproducible.

## Release scope

Release A is the `digitv3`-only cut. Release B additionally ships
`float-small` and `float-big`, in that reviewed order — order is part of the
manifest bytes, so any other permutation is refused outright rather than built
unpinned. The Float ordering guard (`assert_float_seeding_guarded` in
`packaging/lib/gen_payload.sh`) still requires the Slice-7 warm-up allow-list to
exist before a `float-*` stem may be approved for seeding.
