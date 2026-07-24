# Packaged manifest descriptors

Hand-maintained **packaging inputs** — the same class of file as
`packaging/models.approved` and `packaging/debian/control.in`. They are NOT
generated artifacts and NOT candidates; they are the reviewed source from which
the release `manifest.json` is produced at package-build time.

## What `<stem>.descriptor.json` is

A generation descriptor for `tools/gen_model_manifest.py`. It declares an
artifact's **identity** (`canonical_id`, `family`, `task`, `input_size`), its
TensorRT `built_for` platform triple, the **expected** engine/sidecar SHA-256
(which must equal the `packaging/models.approved` row for that stem), and fully
**cited** provenance (`provenance` + `provenance_evidence`, every value measured,
never invented).

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
`fb26074d8e618caaf6f8d41737631c4b378aaa9197506aaa05586fc2ee898efd` — the
hard-gate-reviewed Release-A candidate. Any edit to `digitv3.descriptor.json` that
changes the output will fail the build until the new candidate is re-reviewed and
that constant is updated.

## `installed_utc`

`installed_utc` is the **release artifact's recorded installation epoch** for this
declaration (`2026-07-23T00:00:00Z` for the Release-A digitv3 cut). It is **not**
the package build time and **not** the appliance install time; it is fixed so the
manifest is byte-reproducible.

## Release A scope

Release A ships **only** `digitv3.descriptor.json`. No Float (`float-*`) descriptor
exists here, and no Float artifact enters the package — see the ordering assertion
in `packaging/lib/policy.sh` (`assert_float_seeding_guarded`) and
`packaging/lib/gen_payload.sh`.
