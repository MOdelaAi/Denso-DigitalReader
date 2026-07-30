#!/usr/bin/env python3
"""Emit a schema-2 `manifest.json` from measured artifacts and recorded evidence.

Release A, Slice 4 of the model/operating-mode compatibility work. Outside the
CMake graph; writes only into an isolated directory outside the repository and
outside every application-visible path.

WHAT THIS TOOL IS ALLOWED TO KNOW
---------------------------------
Almost nothing. The manifest declares WHAT AN ARTIFACT IS; what a model may
*do* is decided by a compiled registry in `denso_core` that this tool must never
duplicate. So there is no mode matrix here, no `allowed_modes` field, and no
built-in list of families or canonical ids — identity comes from a caller-
supplied generation descriptor, and the tool's job is to prove that identity
against the bytes on disk.

Two rules follow, and they are the reason this is a generator rather than a
template:

1. **Hashes are MEASURED, never copied.** Every `*_sha256` in the output is
   computed from the real file at generation time. A descriptor may state an
   expected hash, which is checked, but a stated hash is never emitted in place
   of a measured one. A manifest whose hashes came from a document rather than
   from the artifacts is exactly the failure the manifest exists to prevent.
2. **Provenance is EXTRACTED, never invented.** Fields come from recorded
   evidence files (the Slice-2 export provenance, the Slice-3 engine build
   report). A descriptor may supply a field the evidence files do not carry, but
   only alongside an explicit `provenance_evidence` citation naming where the
   value came from — a field with no citation is refused. That makes fabrication
   an active, visible act rather than a quiet default.

GENERATION DESCRIPTOR (JSON, supplied per `--generation`)
---------------------------------------------------------
    {
      "name":         "float-small",
      "canonical_id": "float-small",
      "family":       "float_ball",
      "task":         "detect",
      "input_size":   640,
      "state":        "installed",              // optional, defaults to installed
      "installed_utc": "2026-07-23T00:00:00Z",  // or --installed-utc

      "onnxruntime": {                          // optional
        "model_path":      "<path>/float-small.onnx",
        "expected_sha256": "<64 lowercase hex>" // optional cross-check
      },
      "tensorrt": {                             // optional
        "engine_path":              "<path>/float-small.engine",
        "expected_engine_sha256":   "...",      // optional cross-check
        "sidecar_path":             "<path>/float-small.names.json",
        "expected_sidecar_sha256":  "...",      // optional cross-check
        "built_for": { "trt": "10.3.0.30", "cuda": "12.6.68", "sm": "87" }
      },

      "provenance_from": ["<path>/float-small.provenance.json",
                          "<path>/slice3-engine-build.json"],
      "provenance":           { "precision": "fp16" },     // optional additions
      "provenance_evidence":  { "precision": "trtexec --fp16, Slice-3 log" }
    }

At least one runtime block must be present; a descriptor with neither is
refused, matching `validate_manifest`.

`built_for` is emitted ONLY inside `runtime.tensorrt`. There is no code path
here that can place it at the generation root — the root object is assembled
from a fixed key list that does not contain it. That is deliberate: a root
`built_for` would be a TensorRT platform assertion readable on a Windows ONNX
Runtime deployment, and schema 2 rejects it outright.

DETERMINISM
-----------
Identical inputs must produce byte-identical output, so nothing here reads the
clock: `installed_utc` is a required explicit input. Key order is fixed by
construction, generation order follows the command line, the separators and the
trailing newline are pinned, and only basenames are emitted. The output is
scanned for absolute paths, home directories and account names before it is
published — a leak is a refusal, not a note in a report.

Typical use:

    python tools/gen_model_manifest.py \\
        --generation <work>/float-small.descriptor.json \\
        --generation <work>/float-big.descriptor.json \\
        --installed-utc 2026-07-23T00:00:00Z \\
        --output <isolated-out>/manifest.json \\
        --report <isolated-out>/manifest-report.json
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import posixpath
import re
import sys
from pathlib import Path

# Importing a sibling tool would drop a `tools/__pycache__/` directory into the
# working tree. That is not cosmetic here: `tools/build_package.sh` REFUSES to
# package a dirty tree, so merely running this generator from the repo would
# block the next release build. Bytecode caching is disabled before the import.
sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
import export_float_onnx as ex  # noqa: E402
import gen_names_sidecar as sc  # noqa: E402

SCHEMA = 2
STATE_INSTALLED = "installed"

# The CLOSED vocabulary from `src/core/models/manifest.h`. Mirrored, not invented:
# the C++ validator compares against exactly these strings.
SOURCE_ONNX_METADATA_NAMES = "onnx_metadata_names"
SOURCE_NAMES_SIDECAR = "names_sidecar"

# Every provenance key the schema-2 parser reads. A descriptor may not introduce
# others: an unknown key would be silently dropped by the parser, so accepting it
# here would record a promise the manifest does not keep.
PROVENANCE_KEYS = (
    "source_pt", "source_pt_sha256", "onnx", "onnx_sha256", "onnx_opset",
    "training_ultralytics", "export_ultralytics", "batch", "dynamic", "nms",
    "precision", "jetpack", "export_onnx_command", "export_engine_command",
)
# The subset `validate_manifest` refuses to accept empty. Under the ENGINE-ONLY
# artifact policy this is just `precision`, a property of the engine plan itself.
PROVENANCE_REQUIRED = ("precision",)

# ENGINE-ONLY ARTIFACT POLICY — the emission-side enforcement.
#
# The production artifact pair is <model-id>.engine + <model-id>.names.json, and
# the approved ENGINE BYTES are the provenance authority. A .onnx or .pt is not
# required, not packaged, not searched for and not relied upon by runtime
# loading, approval, provenance, manifest generation, packaging or deployment.
#
# So a generated manifest may not carry a source-chain field AT ALL. This is
# refused here rather than merely "not required", because a stale descriptor key
# would otherwise silently publish a lineage claim the project does not make and
# cannot check. The C++ parser stays tolerant of these keys so manifests already
# installed on an appliance keep validating; generation is where the line is held.
PROVENANCE_FORBIDDEN = (
    "source_pt", "source_pt_sha256", "onnx", "onnx_sha256", "onnx_opset",
    "training_ultralytics", "export_ultralytics", "export_onnx_command",
)

TIMESTAMP_RE = re.compile(r"^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z$")


class ManifestError(RuntimeError):
    """Any fail-closed refusal. Reported to stderr; exit code 1."""


# --------------------------------------------------------------------------- #
# Mirrors of the C++ validation rules
# --------------------------------------------------------------------------- #

def is_basename(value: str) -> bool:
    return bool(value) and "/" not in value and "\\" not in value and ".." not in value


def is_safe_token(value: str) -> bool:
    return is_basename(value) and all(
        c.isascii() and (c.isalnum() or c in "._-") for c in value)


def is_lower_hex64(value: str) -> bool:
    return sc.ex_is_lower_hex64(value)


# `whole()` in manifest.cpp reads through QJsonValue::toInt, so a value outside
# the signed 32-bit range fails the parse and takes the whole manifest with it.
CPP_INT_MIN, CPP_INT_MAX = -(2 ** 31), 2 ** 31 - 1


def is_cpp_int(value) -> bool:
    return (sc.is_exact_int(value) and CPP_INT_MIN <= value <= CPP_INT_MAX)


# Which JSON type the schema-2 parser reads each provenance field through. A
# value of any other type is accepted by json.dumps, dropped by the C++ reader,
# and then — for the five required fields — fails validate_schema2 as "empty".
PROVENANCE_TYPES = {
    "source_pt": str, "source_pt_sha256": str, "onnx": str, "onnx_sha256": str,
    "onnx_opset": int, "training_ultralytics": str, "export_ultralytics": str,
    "batch": int, "dynamic": bool, "nms": bool, "precision": str,
    "jetpack": str, "export_onnx_command": str, "export_engine_command": str,
}


def is_publishable_provenance(key: str, value) -> bool:
    expected = PROVENANCE_TYPES.get(key)
    if expected is None:
        return False
    if expected is bool:
        return isinstance(value, bool)
    if expected is int:
        return is_cpp_int(value)
    return isinstance(value, str)


def stem_of(value: str) -> str:
    return value.rsplit(".", 1)[0] if "." in value else value


def sidecar_stem(value: str) -> str:
    """`x.names.json` -> `x`; mirrors the C++ two-extension strip."""
    parts = value.rsplit(".", 2)
    return parts[0] if len(parts) == 3 else stem_of(value)


# --------------------------------------------------------------------------- #
# Evidence extraction
# --------------------------------------------------------------------------- #

def _basename(value) -> str:
    """Record `models/float-small.pt` as `float-small.pt`.

    Provenance is read by other people on other machines; a path from the
    machine that happened to run the export is noise at best and an information
    leak at worst.
    """
    return Path(str(value)).name if value else ""


def _jetpack_from_report(entry: dict) -> str:
    """Describe JetPack honestly, from what the device actually reported.

    The measured device carries no `nvidia-jetpack` meta-package, so its version
    is an inference from the L4T release, not a reading. Recording a bare "6.2"
    would claim a package version nobody measured; this records the inference and
    what it was inferred from.
    """
    raw = str(entry.get("jetpack_version") or "")
    # `"".split()` is [], so indexing it raises IndexError and the guard below
    # would never run. Tokenise first, then check.
    l4t_tokens = str(entry.get("l4t_version") or "").split()
    l4t = l4t_tokens[0].lower() if l4t_tokens else ""
    inferred = "inferred" in raw.lower() or "not installed" in raw.lower()
    if inferred:
        if not l4t:
            raise ManifestError(
                "engine build report claims an inferred JetPack version but "
                "records no L4T version to have inferred it from")
        return f"inferred_from_l4t_{l4t}"
    measured_tokens = raw.split()
    if not measured_tokens:
        raise ManifestError("engine build report records no JetPack version")
    return measured_tokens[0]


def _precision_from_report(entry: dict) -> str:
    """Derive the build precision from the command that was actually run."""
    command = str(entry.get("exact_trtexec_command") or "")
    if not command:
        raise ManifestError("engine build report entry records no trtexec command")
    flags = [flag for flag in ("--fp16", "--int8", "--best", "--noTF32")
             if flag in command]
    if flags == ["--fp16"]:
        return "fp16"
    if not flags:
        return "fp32"
    raise ManifestError(
        f"cannot derive a single precision from the recorded trtexec flags "
        f"{flags}; state it explicitly in the descriptor with an evidence "
        "citation")


def check_slice2_record_is_valid(doc: dict, source_name: str) -> None:
    """A record that failed its OWN validation must not supply provenance.

    The Slice-2 exporter records whether the artifact it describes passed every
    shape/dtype/class assertion. Copying provenance out of a record that says it
    failed would attach a clean-looking history to a rejected export.
    """
    # Absence is NOT a pass. An authoritative Slice-2 record always emits the
    # field, so a record without it is either not authoritative or was edited —
    # both of which must fail closed rather than default to trusted.
    if doc.get("validation_passed") is not True:
        raise ManifestError(
            f"evidence file {source_name} records validation_passed="
            f"{doc.get('validation_passed')!r}; a record that failed its own "
            "validation must not supply provenance")
    # Type before truthiness: `{}`, `""` and `0` are all falsy, so a malformed
    # value would read as "no violations" rather than as malformed evidence.
    violations = doc.get("validation_violations", [])
    if not isinstance(violations, list):
        raise ManifestError(
            f"evidence file {source_name} records validation_violations "
            f"{violations!r}; a list is required — a malformed value must not "
            "read as an empty one")
    if violations:
        raise ManifestError(
            f"evidence file {source_name} records validation_violations "
            f"{violations!r}; provenance may only come from a clean record")


def check_engine_evidence(entry: dict, class_names: list, source_name: str,
                          onnx_facts: dict | None = None) -> None:
    """The plan's own binding evidence, not merely its hash and exit code.

    A correct engine hash says WHICH bytes; it says nothing about whether those
    bytes decode to the classes this generation declares.

    TWO decode layouts exist and only one encodes the class count. `TrtEngine`
    picks by output shape:

      classic head   [1, nc+4, anchors]  -> num_classes = channels - 4
      end-to-end     [1, N, 6]           -> NMS-free; channels say NOTHING about
                                            the class count

    `digitv3` is an end-to-end export (`[1,300,6]` with ten classes), so applying
    `channels - 4` universally would compute 296 classes and reject the very
    model this release exists to declare. The count is therefore cross-checked
    only for the classic layout; for end-to-end the sidecar remains the class
    authority (spec 8.5) and only the binding layout is verified here.
    """
    name = entry.get("engine_filename")
    shape = entry.get("output_shape")
    if (not isinstance(shape, list) or len(shape) != 3 or
            not all(sc.is_exact_int(d) and d > 0 for d in shape)):
        raise ManifestError(
            f"{source_name} entry {name!r} records output shape {shape!r}; a "
            "three-element list of positive integers is required")
    if shape[0] != 1:
        raise ManifestError(
            f"{source_name} entry {name!r} records output shape {shape}; batch "
            "must be 1")
    if shape[2] != 6:                                   # classic head
        decoded = shape[1] - 4
        if decoded != len(class_names):
            raise ManifestError(
                f"{source_name} entry {name!r} records output shape {shape}, "
                f"which decodes to {decoded} class(es), but this generation "
                f"declares {len(class_names)} ({class_names})")

    expected_name = (onnx_facts or {}).get("output_name")
    recorded_name = entry.get("output_name")
    if not isinstance(recorded_name, str) or not recorded_name:
        raise ManifestError(
            f"{source_name} entry {name!r} records output_name "
            f"{recorded_name!r}; a non-empty string is required")
    if expected_name and recorded_name != expected_name:
        raise ManifestError(
            f"{source_name} entry {name!r} records output name "
            f"{recorded_name!r}, but the ONNX emits {expected_name!r}")

    dtype = entry.get("output_dtype")
    if not isinstance(dtype, str) or not dtype.split():
        raise ManifestError(
            f"{source_name} entry {entry.get('engine_filename')!r} records "
            f"output dtype {dtype!r}; a non-empty string is required")
    if dtype.split()[0].upper() != "FP32":
        raise ManifestError(
            f"{source_name} entry {entry.get('engine_filename')!r} records "
            f"output dtype {dtype!r}; FP32 bindings are required")


def extract_from_slice2(doc: dict, source_name: str) -> tuple[dict, dict]:
    """Map a Slice-2 export provenance record onto the manifest's fields."""
    mapping = {
        "source_pt": _basename(doc.get("source_pt")),
        "source_pt_sha256": doc.get("source_pt_sha256"),
        "onnx": _basename(doc.get("onnx")),
        "onnx_sha256": doc.get("onnx_sha256"),
        "onnx_opset": doc.get("actual_onnx_opset"),
        "training_ultralytics": doc.get("training_ultralytics"),
        "export_ultralytics": doc.get("export_ultralytics"),
        "batch": doc.get("batch"),
        "dynamic": doc.get("dynamic"),
        "nms": doc.get("nms"),
        "export_onnx_command": doc.get("export_command"),
    }
    values = {k: v for k, v in mapping.items() if v is not None and v != ""}
    return values, {k: source_name for k in values}


def extract_from_slice3(entry: dict, source_name: str) -> tuple[dict, dict]:
    """Map one Slice-3 engine build entry onto the manifest's fields."""
    values = {
        "export_engine_command": entry.get("exact_trtexec_command"),
        "precision": _precision_from_report(entry),
        "jetpack": _jetpack_from_report(entry),
    }
    values = {k: v for k, v in values.items() if v}
    return values, {k: f"{source_name} [{entry.get('engine_filename')}]"
                    for k in values}


def select_build_entry(entries: list, onnx_sha256: str | None, onnx_name: str | None,
                       engine_sha256: str | None, source_name: str) -> dict:
    """Find this generation's build entry, keyed on whichever artifact it declares.

    An ONNX-declaring generation keys on the ONNX hash (the stronger link: it ties
    the record to the exact bytes that were compiled). A TensorRT-only generation
    has no ONNX to key on, so it keys on the measured engine hash instead. Keying
    on a filename is never enough — Slice 3 had four same-named ONNX files and
    only one carried the approved bytes.
    """
    for entry in entries:
        if not isinstance(entry, dict):
            raise ManifestError(
                f"{source_name} has a non-object engine entry ({entry!r})")
    if onnx_sha256 is not None:
        return sc.select_engine_entry(entries, onnx_sha256, onnx_name)
    if engine_sha256 is None:
        raise ManifestError(
            f"{source_name} is an engine build report, but this generation "
            "declares neither an ONNX nor an engine to key its entry on")
    matches = [e for e in entries if e.get("engine_sha256") == engine_sha256]
    if len(matches) != 1:
        raise ManifestError(
            f"{source_name} contains {len(matches)} entries whose engine_sha256 "
            f"is {engine_sha256}; expected exactly one")
    return matches[0]


def check_build_entry_backs_this_generation(entry: dict, engine_sha256: str | None,
                                            source_name: str) -> None:
    """The entry must describe a SUCCESSFUL build of THESE engine bytes.

    Without the hash comparison the entry is selected by ONNX hash alone, so a
    manifest can measure engine B's bytes while copying engine A's platform,
    precision and build command out of the report. Every field would look
    consistent and the provenance would describe an artifact that does not exist.
    """
    exit_code = entry.get("build_exit_code")
    if not sc.is_exact_int(exit_code) or exit_code != 0:
        raise ManifestError(
            f"{source_name} entry {entry.get('engine_filename')!r} records "
            f"build_exit_code {exit_code!r}; only an integer 0 (a successful "
            "build) may supply provenance")
    if engine_sha256 is None:
        return
    recorded = entry.get("engine_sha256")
    if recorded != engine_sha256:
        raise ManifestError(
            f"{source_name} describes engine {recorded}, but the declared "
            f"engine measures {engine_sha256}. The build report does not "
            "describe the bytes this manifest declares.")


def load_evidence(paths, onnx_sha256: str | None,
                  onnx_name: str | None,
                  engine_sha256: str | None = None,
                  class_names: list | None = None,
                  onnx_facts: dict | None = None) -> tuple[dict, dict]:
    """Read every evidence file and merge what it supports, recording sources."""
    values: dict = {}
    sources: dict = {}

    if isinstance(paths, str) or not isinstance(paths, list):
        raise ManifestError(
            f"provenance_from must be a list of evidence file paths, found "
            f"{type(paths).__name__}")

    docs: list = []
    for raw_path in paths:
        if not isinstance(raw_path, str):
            raise ManifestError(
                f"provenance_from entries must be paths, found {raw_path!r}")
        path = Path(raw_path)
        try:
            doc = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, ValueError) as exc:
            raise ManifestError(f"evidence file {path.name} is unreadable: {exc}")
        if not isinstance(doc, dict):
            raise ManifestError(f"evidence file {path.name} is not a JSON object")
        docs.append((path, doc))

    # The ONNX hash a Slice-2 export record must match to be considered part of
    # THIS chain. Declared directly when the generation has an ONNX block; for a
    # TensorRT-only generation it comes from the engine build entry, which
    # records the source ONNX it compiled. So the export provenance is still
    # linked by a measured hash rather than by the operator's say-so.
    link_sha = onnx_sha256
    entries: dict = {}
    for path, doc in docs:
        if isinstance(doc.get("engines"), list):
            entry = select_build_entry(doc["engines"], onnx_sha256, onnx_name,
                                       engine_sha256, path.name)
            check_build_entry_backs_this_generation(entry, engine_sha256,
                                                    path.name)
            # Only when this generation actually declares the engine: an
            # ONNX-only generation cites the report for its build command and
            # must not be held to a plan it does not ship.
            if engine_sha256 is not None and class_names is not None:
                check_engine_evidence(entry, class_names, path.name, onnx_facts)
            entries[path.name] = entry
            if link_sha is None:
                link_sha = entry.get("source_onnx_sha256")

    for path, doc in docs:
        if isinstance(doc.get("engines"), list):           # Slice-3 build report
            found, found_sources = extract_from_slice3(entries[path.name],
                                                       path.name)
        elif "onnx_sha256" in doc:                          # Slice-2 provenance
            # Unkeyed, a Slice-2 record could describe any artifact chain, and
            # its provenance would be attached to an engine it never produced.
            if link_sha is None:
                raise ManifestError(
                    f"evidence file {path.name} is an export provenance record, "
                    "but this generation declares no ONNX and supplies no engine "
                    "build report recording one, so there is nothing to link it "
                    "to. A record that cannot be tied to a declared artifact "
                    "must not supply its provenance.")
            if doc.get("onnx_sha256") != link_sha:
                raise ManifestError(
                    f"evidence file {path.name} records onnx_sha256 "
                    f"{doc.get('onnx_sha256')}, but this generation's chain uses "
                    f"{link_sha}; it describes a different artifact")
            check_slice2_record_is_valid(doc, path.name)
            found, found_sources = extract_from_slice2(doc, path.name)
        else:
            raise ManifestError(
                f"evidence file {path.name} is neither an export provenance "
                "record nor an engine build report")

        for key, value in found.items():
            if key in values and values[key] != value:
                raise ManifestError(
                    f"evidence conflict for provenance.{key}: {sources[key]} says "
                    f"{values[key]!r}, {path.name} says {value!r}")
            values[key] = value
            sources.setdefault(key, found_sources[key])
    return values, sources


# --------------------------------------------------------------------------- #
# Class-name authority
# --------------------------------------------------------------------------- #

def read_sidecar(path: Path) -> list[str]:
    try:
        doc = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as exc:
        raise ManifestError(f"sidecar {path.name} is unreadable: {exc}")
    if not isinstance(doc, list) or not doc:
        raise ManifestError(
            f"sidecar {path.name} must be a non-empty JSON array of strings")
    for index, value in enumerate(doc):
        if not isinstance(value, str) or not value.strip():
            raise ManifestError(
                f"sidecar {path.name} element {index} is not a non-empty string")
    return doc


def resolve_class_names(onnx_path: Path | None, sidecar_path: Path | None,
                        name: str, skip_structural: bool) -> tuple[list[str], dict]:
    """Derive the class vocabulary from the artifacts, and prove they agree."""
    facts = None
    from_onnx = None
    if onnx_path is not None:
        if not skip_structural:
            try:
                ex.check_graph_validity(onnx_path)
            except ex.ExportError as exc:
                raise ManifestError(str(exc))
        # A malformed `names`/`args` literal surfaces as a raw ValueError or
        # TypeError from inspect_onnx; wrapped so the CLI prints a redacted
        # refusal rather than a traceback, exactly as the sidecar tool does.
        try:
            facts = ex.inspect_onnx(onnx_path)
        except ex.ExportError:
            raise
        except Exception as exc:
            raise ManifestError(
                f"{onnx_path.name}: ONNX metadata is unreadable: "
                f"{type(exc).__name__}: {exc}")
        from_onnx = sc.check_onnx_class_metadata(facts, onnx_path.name)

    from_sidecar = read_sidecar(sidecar_path) if sidecar_path is not None else None

    if from_onnx is not None and from_sidecar is not None and from_onnx != from_sidecar:
        raise ManifestError(
            f"generation {name}: ONNX metadata declares {from_onnx} but the "
            f"sidecar declares {from_sidecar}; one of them is stale")
    names = from_onnx if from_onnx is not None else from_sidecar
    if not names:
        raise ManifestError(
            f"generation {name}: no class-name authority — neither an ONNX with "
            "`names` metadata nor a sidecar was supplied")
    authority = ("onnx_metadata_names" if from_onnx is not None
                 else "names_sidecar")
    return names, {"authority": authority, "onnx_facts": facts}


# --------------------------------------------------------------------------- #
# Generation assembly
# --------------------------------------------------------------------------- #

def measured_hash(path: Path, expected: str | None, label: str) -> str:
    if not path.is_file():
        raise ManifestError(f"{label}: {path} does not exist")
    digest = ex.sha256_file(path)
    if expected is not None and expected != digest:
        raise ManifestError(
            f"{label}: SHA-256 mismatch — descriptor expects {expected}, "
            f"the file measures {digest}")
    return digest


# --------------------------------------------------------------------------- #
# Direct engine approval (engine-only artifact policy)
# --------------------------------------------------------------------------- #

# The checks a candidate .engine must have passed on the target device before its
# bytes may be approved. Mirrors the project's approval policy; every one of them
# is a statement about the ENGINE ITSELF, which is why no source chain is needed.
APPROVAL_CHECKS = (
    "regular_file", "sha256_recorded", "deserialize", "synthetic_inference",
    "input_binding", "output_binding", "class_count_matches_sidecar",
    "sidecar_present", "sidecar_json_valid", "identity_and_family",
    "decoder_matches_runtime", "target_platform",
)


def validate_approval(descriptor: dict, descriptor_name: str, engine_sha: str,
                      sidecar_sha: str, built_for: dict, input_size: int,
                      class_names: list) -> None:
    """Refuse to build a generation whose engine bytes were never approved.

    The approval record is REPO-CONTROLLED REVIEW DATA and is deliberately NOT
    published into the manifest: the runtime authorizes on hashes, not on a
    narrative. Its job here is to make "these exact bytes were validated on the
    target device" a machine-checked precondition of generation rather than a
    claim in a commit message. It is cross-checked against the hashes actually
    MEASURED from the files, so an approval record can never drift onto a
    different artifact than the one being packaged.
    """
    ap = descriptor.get("approval")
    if not isinstance(ap, dict):
        raise ManifestError(
            f"{descriptor_name}: a tensorrt generation requires an `approval` "
            "object recording the direct on-device validation of these exact "
            "engine bytes (engine-only artifact policy)")

    for key in ("validated_on", "device", "engine_sha256", "sidecar_sha256",
                "trt", "cuda", "sm", "checks"):
        if key not in ap:
            raise ManifestError(
                f"{descriptor_name}: approval is missing required key {key!r}")

    if ap["engine_sha256"] != engine_sha:
        raise ManifestError(
            f"{descriptor_name}: approval.engine_sha256 ({ap['engine_sha256']}) "
            f"is not the engine that was measured ({engine_sha}). The approval "
            "record describes different bytes than the artifact being packaged.")
    if ap["sidecar_sha256"] != sidecar_sha:
        raise ManifestError(
            f"{descriptor_name}: approval.sidecar_sha256 ({ap['sidecar_sha256']}) "
            f"is not the sidecar that was measured ({sidecar_sha}).")

    # The engine must have been approved for the platform it declares it was
    # built for, or the approval attests to a run that proves nothing about the
    # target the policy will compare against.
    for key in ("trt", "cuda", "sm"):
        if str(ap[key]) != str(built_for[key]):
            raise ManifestError(
                f"{descriptor_name}: approval.{key} ({ap[key]!r}) does not match "
                f"tensorrt.built_for.{key} ({built_for[key]!r}); the engine was "
                "not approved on the platform it declares")

    checks = ap["checks"]
    if not isinstance(checks, dict):
        raise ManifestError(f"{descriptor_name}: approval.checks must be an object")
    missing = [c for c in APPROVAL_CHECKS if c not in checks]
    if missing:
        raise ManifestError(
            f"{descriptor_name}: approval.checks is missing {missing}. Every "
            "direct-validation check must be recorded with its observed result.")
    unrecorded = sorted(c for c in APPROVAL_CHECKS
                        if not str(checks.get(c) or "").strip())
    if unrecorded:
        raise ManifestError(
            f"{descriptor_name}: approval.checks entries {unrecorded} are empty. "
            "Record what was OBSERVED, not that a box was ticked.")

    # ── The observations above are the HUMAN record. They are prose, so on their
    #    own a non-empty rule would happily accept "not tested" — an approval that
    #    asserts nothing. The load-bearing outcomes are therefore ALSO required as
    #    machine-checkable values, and cross-checked against the artifact's own
    #    declaration. A descriptor cannot claim an approval whose measured shapes,
    #    class count or platform disagree with what it is declaring.
    for key, want_type in (("deserialize_ok", bool), ("inference_ok", bool),
                           ("input_shape", list), ("output_shape", list),
                           ("class_count", int)):
        if key not in ap:
            raise ManifestError(
                f"{descriptor_name}: approval is missing required outcome {key!r}. "
                "Prose observations alone cannot prove a check succeeded.")
        if want_type is bool and not isinstance(ap[key], bool):
            raise ManifestError(
                f"{descriptor_name}: approval.{key} must be a JSON boolean, found "
                f"{ap[key]!r}")
        if want_type is int and not sc.is_exact_int(ap[key]):
            raise ManifestError(
                f"{descriptor_name}: approval.{key} must be an integer, found "
                f"{ap[key]!r}")
        if want_type is list and not (isinstance(ap[key], list) and ap[key]):
            raise ManifestError(
                f"{descriptor_name}: approval.{key} must be a non-empty array, "
                f"found {ap[key]!r}")

    if ap["deserialize_ok"] is not True:
        raise ManifestError(
            f"{descriptor_name}: approval.deserialize_ok is not true — the engine "
            "did not deserialize on the target device and must not be approved")
    if ap["inference_ok"] is not True:
        raise ManifestError(
            f"{descriptor_name}: approval.inference_ok is not true — no synthetic "
            "inference succeeded on the target device; the engine must not be approved")

    want_input = [1, 3, input_size, input_size]
    if ap["input_shape"] != want_input:
        raise ManifestError(
            f"{descriptor_name}: approval.input_shape {ap['input_shape']} does not "
            f"match the declared input_size {input_size} (expected {want_input})")
    if ap["class_count"] != len(class_names):
        raise ManifestError(
            f"{descriptor_name}: approval.class_count {ap['class_count']} does not "
            f"match the {len(class_names)} class name(s) the sidecar declares")

    # The output shape must be one the runtime can actually decode, AND must be
    # the one implied by this model's own class count — the check that stops a
    # raw-YOLO plan being approved under an end2end declaration or vice versa.
    out_shape = ap["output_shape"]
    raw_yolo = [1, 4 + len(class_names), 8400]
    end2end_ok = (len(out_shape) == 3 and out_shape[0] == 1 and out_shape[2] == 6)
    if out_shape != raw_yolo and not end2end_ok:
        raise ManifestError(
            f"{descriptor_name}: approval.output_shape {out_shape} is neither the "
            f"raw-YOLO layout implied by {len(class_names)} class(es) ({raw_yolo}, "
            "decoded by decode_yolo) nor an end2end [1,N,6] layout (decoded by "
            "decode_yolo_end2end). The runtime has no decoder for it.")


def build_generation(descriptor: dict, descriptor_name: str,
                     default_installed_utc: str | None,
                     skip_structural: bool) -> tuple[dict, dict]:
    def need(key: str):
        if key not in descriptor:
            raise ManifestError(f"{descriptor_name}: missing required key {key!r}")
        return descriptor[key]

    unknown = set(descriptor) - {
        "name", "canonical_id", "family", "task", "input_size", "state",
        "installed_utc", "onnxruntime", "tensorrt", "provenance_from",
        "provenance", "provenance_evidence", "approval",
    }
    if unknown:
        raise ManifestError(
            f"{descriptor_name}: unknown descriptor key(s) {sorted(unknown)}")

    name = str(need("name"))
    canonical_id = str(need("canonical_id"))
    family = str(need("family"))
    task = str(need("task"))
    input_size = need("input_size")
    state = str(descriptor.get("state", STATE_INSTALLED))
    installed_utc = str(descriptor.get("installed_utc") or default_installed_utc or "")

    if not is_safe_token(canonical_id):
        raise ManifestError(f"{descriptor_name}: canonical_id is not a safe token: "
                            f"{canonical_id}")
    if not is_safe_token(family):
        raise ManifestError(f"{descriptor_name}: family is not a safe token: {family}")
    if not name:
        raise ManifestError(f"{descriptor_name}: name must be non-empty")
    if not task:
        raise ManifestError(f"{descriptor_name}: task must be non-empty")
    if not isinstance(input_size, int) or isinstance(input_size, bool) or input_size <= 0:
        raise ManifestError(
            f"{descriptor_name}: input_size must be a positive integer, "
            f"found {input_size!r}")
    if state != STATE_INSTALLED:
        raise ManifestError(
            f"{descriptor_name}: state must be {STATE_INSTALLED!r} "
            f"(the validator accepts nothing else), found {state!r}")
    if not TIMESTAMP_RE.match(installed_utc):
        raise ManifestError(
            f"{descriptor_name}: installed_utc must be an explicit "
            "YYYY-MM-DDTHH:MM:SSZ timestamp (this tool never reads the clock), "
            f"found {installed_utc!r}")

    ort_spec = descriptor.get("onnxruntime")
    trt_spec = descriptor.get("tensorrt")
    if ort_spec is None and trt_spec is None:
        raise ManifestError(
            f"{descriptor_name}: a generation must declare at least one runtime "
            "block (onnxruntime and/or tensorrt)")
    # A block that is PRESENT but empty/wrong-typed must be a refusal, never a
    # silent omission: spec 3.2.1 rule 5 makes a generation with no block for the
    # running backend unavailable there, so quietly dropping a block the operator
    # wrote would disable the model on that platform with exit 0.
    for label, spec in (("onnxruntime", ort_spec), ("tensorrt", trt_spec)):
        if spec is None:
            continue
        if not isinstance(spec, dict) or not spec:
            raise ManifestError(
                f"{descriptor_name}: runtime block {label!r} is present but "
                f"{spec!r}. Declare it completely or omit the key entirely — a "
                "half-declared backend would silently vanish from the manifest.")

    def need_path(spec: dict, key: str, label: str) -> Path:
        if key not in spec:
            raise ManifestError(f"{descriptor_name}: {label} requires {key!r}")
        return Path(str(spec[key]))

    onnx_path = need_path(ort_spec, "model_path", "onnxruntime") if ort_spec else None
    sidecar_path = need_path(trt_spec, "sidecar_path", "tensorrt") if trt_spec else None

    class_names, class_info = resolve_class_names(onnx_path, sidecar_path, name,
                                                  skip_structural)

    facts = class_info["onnx_facts"]
    if facts is not None:
        shape = facts.get("input_shape") or []
        if len(shape) == 4 and (shape[2] != input_size or shape[3] != input_size):
            raise ManifestError(
                f"{descriptor_name}: declared input_size {input_size} does not "
                f"match the ONNX input shape {shape}")
        onnx_task = facts.get("task")
        if onnx_task and onnx_task != task:
            raise ManifestError(
                f"{descriptor_name}: declared task {task!r} does not match the "
                f"ONNX metadata task {onnx_task!r}")

    runtime: dict = {}
    artifact_hashes: dict = {}
    onnx_sha = None
    engine_sha = None
    if ort_spec:
        unknown_ort = set(ort_spec) - {"model_path", "expected_sha256"}
        if unknown_ort:
            raise ManifestError(
                f"{descriptor_name}: unknown onnxruntime key(s) {sorted(unknown_ort)}")
        onnx_sha = measured_hash(onnx_path, ort_spec.get("expected_sha256"),
                                 f"{descriptor_name}: onnxruntime.model")
        model_name = onnx_path.name
        if not is_basename(model_name) or not model_name.endswith(".onnx"):
            raise ManifestError(
                f"{descriptor_name}: runtime.onnxruntime.model must be a .onnx "
                f"basename, found {model_name}")
        runtime["onnxruntime"] = {
            "model": model_name,
            "model_sha256": onnx_sha,
            "class_metadata_source": SOURCE_ONNX_METADATA_NAMES,
        }
        artifact_hashes["onnx"] = {"file": model_name, "sha256": onnx_sha}

    if trt_spec:
        unknown_trt = set(trt_spec) - {
            "engine_path", "expected_engine_sha256", "sidecar_path",
            "expected_sidecar_sha256", "built_for"}
        if unknown_trt:
            raise ManifestError(
                f"{descriptor_name}: unknown tensorrt key(s) {sorted(unknown_trt)}")
        engine_path = need_path(trt_spec, "engine_path", "tensorrt")
        engine_sha = measured_hash(engine_path,
                                   trt_spec.get("expected_engine_sha256"),
                                   f"{descriptor_name}: tensorrt.engine")
        sidecar_sha = measured_hash(sidecar_path,
                                    trt_spec.get("expected_sidecar_sha256"),
                                    f"{descriptor_name}: tensorrt.sidecar")
        engine_name, sidecar_name = engine_path.name, sidecar_path.name
        if not is_basename(engine_name) or not is_basename(sidecar_name):
            raise ManifestError(f"{descriptor_name}: tensorrt artifact names must "
                                "be basenames")
        if stem_of(engine_name) != sidecar_stem(sidecar_name):
            raise ManifestError(
                f"{descriptor_name}: engine/sidecar stem mismatch — "
                f"{engine_name} vs {sidecar_name}")
        built_for = trt_spec.get("built_for") or {}
        # A LIST of the three key names satisfies `set(...) == {...}` and then
        # raises AttributeError at `.items()`. Type first, contents second.
        if not isinstance(built_for, dict):
            raise ManifestError(
                f"{descriptor_name}: tensorrt.built_for must be an object, "
                f"found {type(built_for).__name__}")
        if set(built_for) != {"trt", "cuda", "sm"}:
            raise ManifestError(
                f"{descriptor_name}: tensorrt.built_for must carry exactly "
                f"trt, cuda and sm; found {sorted(built_for)}")
        for key, value in built_for.items():
            if not isinstance(value, str) or not value.strip():
                raise ManifestError(
                    f"{descriptor_name}: tensorrt.built_for.{key} must be a "
                    "non-empty string")
        runtime["tensorrt"] = {
            "engine": engine_name,
            "engine_sha256": engine_sha,
            "sidecar": sidecar_name,
            "sidecar_sha256": sidecar_sha,
            "class_metadata_source": SOURCE_NAMES_SIDECAR,
            # Nested HERE and nowhere else. See the module docstring.
            "built_for": {"trt": built_for["trt"], "cuda": built_for["cuda"],
                          "sm": built_for["sm"]},
        }
        artifact_hashes["engine"] = {"file": engine_name, "sha256": engine_sha}
        artifact_hashes["sidecar"] = {"file": sidecar_name, "sha256": sidecar_sha}
        # Engine-only policy: these exact bytes must carry a direct on-device
        # approval, cross-checked against what was just measured.
        validate_approval(descriptor, descriptor_name, engine_sha, sidecar_sha,
                          built_for, input_size, list(class_names))

    # -- provenance: extracted from evidence, then explicitly cited additions -- #
    values, sources = load_evidence(
        descriptor.get("provenance_from") or [], onnx_sha,
        onnx_path.name if onnx_path is not None else None, engine_sha,
        list(class_names), facts)

    explicit = descriptor.get("provenance") or {}
    citations = descriptor.get("provenance_evidence") or {}
    for label, container in (("provenance", explicit),
                             ("provenance_evidence", citations)):
        if not isinstance(container, dict):
            raise ManifestError(
                f"{descriptor_name}: {label} must be an object, found "
                f"{type(container).__name__}")
    for key, value in explicit.items():
        if key not in PROVENANCE_KEYS:
            raise ManifestError(
                f"{descriptor_name}: provenance.{key} is not a schema-2 "
                "provenance field; the parser would drop it silently")
        citation = str(citations.get(key) or "").strip()
        if not citation:
            raise ManifestError(
                f"{descriptor_name}: provenance.{key} is supplied directly but "
                "has no provenance_evidence citation. Every value not extracted "
                "from an evidence file must name its source.")
        # A descriptor may ADD what the evidence does not carry. It may never
        # CONTRADICT what the evidence recorded: this is the "extracted, never
        # invented" contract, and a silent override is precisely how a document
        # replaces a measurement.
        if key in values and values[key] != value:
            raise ManifestError(
                f"{descriptor_name}: provenance.{key} is supplied as {value!r} "
                f"but {sources[key]} recorded {values[key]!r}. A descriptor may "
                "add a field the evidence lacks; it must not contradict one the "
                "evidence measured.")
        values[key] = value
        sources[key] = f"descriptor: {citation}"
    for key in citations:
        if key not in explicit:
            raise ManifestError(
                f"{descriptor_name}: provenance_evidence cites {key!r}, which is "
                "not supplied in `provenance`")

    if onnx_sha is not None and values.get("onnx_sha256") not in (None, onnx_sha):
        raise ManifestError(
            f"{descriptor_name}: provenance.onnx_sha256 "
            f"({values.get('onnx_sha256')}) does not match the measured ONNX "
            f"({onnx_sha}); the provenance record describes a different artifact")

    # Engine-only: refuse a source-chain field regardless of whether a descriptor
    # supplied it directly or an evidence file injected it. Checked on the merged
    # result so neither route can slip one past.
    forbidden = sorted(k for k in PROVENANCE_FORBIDDEN if k in values)
    if forbidden:
        raise ManifestError(
            f"{descriptor_name}: provenance field(s) {forbidden} are forbidden by "
            "the engine-only artifact policy. The approved .engine bytes are the "
            "provenance authority; a manifest must not carry an ONNX/PT/source "
            "chain. Remove them from the descriptor (and from provenance_from).")

    missing = [k for k in PROVENANCE_REQUIRED
               if not str(values.get(k, "")).strip()]
    if missing:
        raise ManifestError(
            f"{descriptor_name}: provenance is missing required field(s) "
            f"{missing}. Supply them from an evidence file, or in the descriptor "
            "with a provenance_evidence citation — do not invent them.")

    provenance = {k: values[k] for k in PROVENANCE_KEYS if k in values}

    # Fixed key list, fixed order. `built_for` is not among them and cannot be.
    generation = {
        "name": name,
        "canonical_id": canonical_id,
        "family": family,
        "task": task,
        "input_size": input_size,
        "class_names": list(class_names),
        "class_count": len(class_names),
        "runtime": runtime,
        "provenance": provenance,
        "installed_utc": installed_utc,
        "state": state,
    }
    audit = {
        "descriptor": descriptor_name,
        "canonical_id": canonical_id,
        "class_name_authority": class_info["authority"],
        "class_names": list(class_names),
        "runtime_blocks": sorted(runtime),
        "measured_artifact_hashes": artifact_hashes,
        "provenance_field_sources": dict(sorted(sources.items())),
    }
    return generation, audit


# --------------------------------------------------------------------------- #
# Manifest-level validation (a mirror of validate_manifest, run before writing)
# --------------------------------------------------------------------------- #

def validate_manifest_object(manifest: dict) -> None:
    seen_names: set = set()
    seen_ids: set = set()
    seen_engines: set = set()
    for g in manifest["generations"]:
        name = g["name"]
        if name in seen_names:
            raise ManifestError(f"duplicate generation name: {name}")
        seen_names.add(name)
        if g["canonical_id"] in seen_ids:
            raise ManifestError(f"duplicate canonical_id: {g['canonical_id']}")
        seen_ids.add(g["canonical_id"])
        if g["class_count"] != len(g["class_names"]):
            raise ManifestError(f"class_count does not match class_names for {name}")
        # C++ validate_common rejects a repeated class name. Omitting it here
        # would let this tool publish a file the appliance parses and then fails
        # to validate — and spec 3.3 makes an invalid manifest ManifestCorrupt, a
        # GLOBAL blocker, not a degraded state. Every C++ rule must have a mirror.
        seen_classes: set = set()
        for c in g["class_names"]:
            if c in seen_classes:
                raise ManifestError(
                    f"duplicate class name '{c}' for generation {name}; the C++ "
                    "validator rejects it and the manifest would be ManifestCorrupt")
            seen_classes.add(c)
        if not is_cpp_int(g["input_size"]):
            raise ManifestError(
                f"input_size {g['input_size']} for generation {name} is outside "
                "the signed 32-bit range the C++ parser accepts")
        if not is_cpp_int(g["class_count"]):
            raise ManifestError(
                f"class_count for generation {name} is outside the signed "
                "32-bit range the C++ parser accepts")
        for key, value in g["provenance"].items():
            if not is_publishable_provenance(key, value):
                raise ManifestError(
                    f"provenance.{key} for generation {name} is {value!r}, whose "
                    "JSON type the C++ parser does not read into that field; it "
                    "would be silently dropped")
        if "built_for" in g:
            raise ManifestError(f"built_for must not appear at the root of {name}")
        if "allowed_modes" in g:
            raise ManifestError(f"allowed_modes must never be emitted ({name})")
        # --- validate_common + validate_schema2, rule for rule ---------------
        # These are enforced upstream in build_generation too. They are repeated
        # HERE deliberately: this is the last gate before bytes are published and
        # it is documented as a mirror, so it must reject on its own rather than
        # trust that some earlier caller already did.
        if not g["class_names"]:
            raise ManifestError(f"class_names must be non-empty for {name}")
        for c in g["class_names"]:
            if not isinstance(c, str) or not c.strip():
                raise ManifestError(f"class_names must not contain blanks for {name}")
        if not name:
            raise ManifestError("generation name must be non-empty")
        if not g["installed_utc"]:
            raise ManifestError(f"installed_utc must be non-empty for {name}")
        if not is_safe_token(g["canonical_id"]):
            raise ManifestError(f"canonical_id is not a safe token for {name}")
        if not g["family"] or not is_safe_token(g["family"]):
            raise ManifestError(f"family is not a safe token for {name}")
        if not g["task"]:
            raise ManifestError(f"task must be non-empty for {name}")
        if g["input_size"] <= 0:
            raise ManifestError(f"input_size must be positive for {name}")
        if g["class_count"] <= 0:
            raise ManifestError(f"class_count must be positive for {name}")
        if g["state"] != STATE_INSTALLED:
            raise ManifestError(
                f"state must be {STATE_INSTALLED!r} for {name}, found {g['state']!r}")

        runtime = g["runtime"]
        if not runtime:
            raise ManifestError(f"runtime must declare at least one backend for {name}")
        for backend in runtime:
            if backend not in ("onnxruntime", "tensorrt"):
                raise ManifestError(f"unknown runtime backend {backend!r} for {name}")
        ort = runtime.get("onnxruntime")
        if ort:
            if not is_basename(ort["model"]):
                raise ManifestError(
                    f"runtime.onnxruntime.model is not a safe basename for {name}")
            if not ort["model"].endswith(".onnx"):
                raise ManifestError(
                    f"runtime.onnxruntime.model must be a .onnx file for {name}")
            if not is_lower_hex64(ort["model_sha256"]):
                raise ManifestError(f"onnxruntime.model_sha256 invalid for {name}")
            if ort["class_metadata_source"] != SOURCE_ONNX_METADATA_NAMES:
                raise ManifestError(
                    f"onnxruntime.class_metadata_source invalid for {name}")
        trt = runtime.get("tensorrt")
        if trt:
            if not is_basename(trt["engine"]) or not is_basename(trt["sidecar"]):
                raise ManifestError(
                    f"runtime.tensorrt artifact names must be basenames for {name}")
            if stem_of(trt["engine"]) != sidecar_stem(trt["sidecar"]):
                raise ManifestError(
                    f"runtime.tensorrt engine/sidecar stem mismatch for {name}")
            if not is_lower_hex64(trt["engine_sha256"]):
                raise ManifestError(f"tensorrt.engine_sha256 invalid for {name}")
            if not is_lower_hex64(trt["sidecar_sha256"]):
                raise ManifestError(f"tensorrt.sidecar_sha256 invalid for {name}")
            if trt["class_metadata_source"] != SOURCE_NAMES_SIDECAR:
                raise ManifestError(
                    f"tensorrt.class_metadata_source invalid for {name}")
            if "built_for" not in trt:
                raise ManifestError(f"tensorrt.built_for missing for {name}")
            for key in ("trt", "cuda", "sm"):
                value = trt["built_for"].get(key)
                if not isinstance(value, str) or not value.strip():
                    raise ManifestError(
                        f"tensorrt.built_for.{key} must be non-empty for {name}")
            engine = trt["engine"]
            if engine in seen_engines:
                raise ManifestError(f"duplicate engine filename: {engine}")
            seen_engines.add(engine)

        for field in PROVENANCE_REQUIRED:
            if not str(g["provenance"].get(field, "")).strip():
                raise ManifestError(
                    f"provenance.{field} must be non-empty for {name}")


# --------------------------------------------------------------------------- #
# Serialization + leak scan
# --------------------------------------------------------------------------- #

def serialize(manifest: dict) -> str:
    """Strict RFC 8259 only.

    `allow_nan` defaults to True and emits bare `NaN`/`Infinity`, which Python
    round-trips happily and Qt's parser rejects outright — the manifest would
    become ManifestCorrupt on the appliance while looking valid here. A NaN can
    arrive from a descriptor or an evidence file, since `json.loads` accepts the
    same extension by default.
    """
    return json.dumps(manifest, indent=2, ensure_ascii=False,
                      allow_nan=False) + "\n"


def scan_for_leaks(text: str) -> list[str]:
    """Refuse to publish machine-specific or personal detail.

    The manifest ships to appliances and is read by other people. An absolute
    path from the machine that generated it is at best noise and at worst an
    account name. This runs on the SERIALIZED bytes, so it cannot be defeated by
    a value arriving through an unexpected field.
    """
    problems: list[str] = []
    patterns = [
        (r"[A-Za-z]:[\\/]", "a Windows absolute path"),
        (r"(?<![\w])/home/", "a POSIX home directory"),
        (r"(?<![\w])/Users/", "a macOS home directory"),
        (r"(?<![\w])/root/", "the root account's home directory"),
        (r"(?i)\bAppData\b", "an AppData path"),
        (r"(?i)\btemp[\\/]", "a temporary-directory path"),
        (r"(?i)\btmp[\\/]", "a temporary-directory path"),
        (r"(?i)\ballowed_modes\b", "an allowed_modes field"),
    ]
    for pattern, description in patterns:
        if re.search(pattern, text):
            problems.append(description)
    home = os.path.expanduser("~")
    if home and home.lower() in text.lower():
        problems.append("the generating account's home directory")
    for var in ("USERNAME", "USER", "LOGNAME"):
        value = os.environ.get(var) or ""
        if len(value) > 2 and re.search(rf"\b{re.escape(value)}\b", text):
            problems.append("the generating account name")

    # Credentials, via the ONE redaction definition the export tool already owns
    # (URL userinfo, bearer/basic tokens, `password=`/`token:`/`--api-key`). If
    # redaction would CHANGE the bytes, something secret is in them.
    #
    # Note what is deliberately NOT rejected: an absolute SYSTEM path such as
    # /usr/src/tensorrt/bin/trtexec. Spec 3.2 shows `export_engine_command`
    # verbatim, 3.5 makes it a required non-empty field and 8.4 fixes the recipe
    # at that exact path — a blanket absolute-path rule would make every
    # conforming manifest unpublishable.
    if ex.redact(text) != text:
        problems.append("a credential or secret in a recorded value")
    return sorted(set(problems))


# The ONE absolute path the manifest is allowed to carry, EXACTLY. Spec 8.4
# fixes the engine recipe at this executable and 3.2/3.5 require the command
# verbatim in the manifest, so it cannot be rejected — but nothing else may ride
# along, and an approved PREFIX would be a hole: `/usr/src/tensorrt/../../srv/x`
# starts with it. Compared after normalisation, against the full path.
APPROVED_ABSOLUTE_PATHS = frozenset({"/usr/src/tensorrt/bin/trtexec"})


def scan_for_absolute_paths(manifest: dict) -> list[str]:
    """Machine-specific absolute paths, over the manifest's own string values.

    `/srv/builds/alice/...` and `/mnt/workstation/...` are every bit as
    machine-specific as a home directory while matching none of the home/temp
    patterns. Tokenised over VALUES rather than the serialized text so JSON
    punctuation cannot hide or manufacture a match.
    """
    problems: list[str] = []

    def inspect(token: str) -> None:
        # Shell redirection glues the operator to its target, and a file
        # DESCRIPTOR glues to the operator: `2>/srv/x.log` starts with a digit,
        # so stripping only `<`/`>` leaves it looking like nothing at all.
        # Covers `>`, `>>`, `<`, `2>`, `2>>`, `0<`, `&>`, `>&`.
        token = token.strip("\"'(),;")
        token = re.sub(r"^&?\d*(?:>>|>|<<|<)&?", "", token)
        if not token:
            return
        if token.startswith("\\\\") or token.startswith("//"):
            problems.append(f"a UNC network path ({token})")
            return
        if re.match(r"^[A-Za-z]:[\\/]", token):
            problems.append(f"a Windows absolute path ({token})")
            return
        if not token.startswith("/"):
            return
        # Normalise so `..` cannot walk out of an approved location.
        normalised = posixpath.normpath(token)
        if normalised not in APPROVED_ABSOLUTE_PATHS:
            problems.append(f"an unapproved absolute path ({normalised})")

    def walk(node):
        if isinstance(node, dict):
            for v in node.values():
                walk(v)
        elif isinstance(node, list):
            for v in node:
                walk(v)
        elif isinstance(node, str):
            for raw in node.split():
                # Recorded commands look like `--onnx=/abs/path`; consider the
                # value side of a flag too, not just bare tokens.
                inspect(raw)
                if "=" in raw:
                    inspect(raw.split("=", 1)[1])

    walk(manifest)
    return sorted(set(problems))


# --------------------------------------------------------------------------- #
# Driver
# --------------------------------------------------------------------------- #

def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Emit a schema-2 manifest.json from measured artifacts and "
                    "recorded provenance evidence.")
    parser.add_argument("--generation", action="append", required=True,
                        metavar="DESCRIPTOR.json",
                        help="repeatable, ORDERED: a generation descriptor")
    parser.add_argument("--output", required=True,
                        help="destination manifest.json, outside the repository")
    parser.add_argument("--installed-utc",
                        help="explicit YYYY-MM-DDTHH:MM:SSZ stamp for generations "
                             "that do not carry their own (this tool never reads "
                             "the clock)")
    parser.add_argument("--report", help="optional: write a generation report here")
    parser.add_argument("--overwrite", action="store_true",
                        help="replace an existing output file (still atomic, and "
                             "only after the new manifest validates)")
    parser.add_argument("--skip-structural-check", action="store_true",
                        help="skip onnx.checker.check_model on referenced ONNX "
                             "files; metadata and shape rules still run")
    return parser


def main(argv=None) -> int:
    args = build_parser().parse_args(argv)
    repo_root = Path(__file__).resolve().parent.parent

    output = Path(args.output)
    report_path_arg = Path(args.report) if args.report else None

    # Both destinations are settled BEFORE any work: a run that is going to fail
    # must fail while nothing has been published.
    sc.reject_aliased_outputs(output, report_path_arg)
    sc.preflight_output(output, args.overwrite, repo_root, "output")
    if report_path_arg is not None:
        sc.preflight_output(report_path_arg, args.overwrite, repo_root, "report")

    if args.installed_utc and not TIMESTAMP_RE.match(args.installed_utc):
        raise ManifestError(
            f"--installed-utc must be YYYY-MM-DDTHH:MM:SSZ, found "
            f"{args.installed_utc!r}")

    generations = []
    audits = []
    for raw in args.generation:
        path = Path(raw)
        try:
            descriptor = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, ValueError) as exc:
            raise ManifestError(f"descriptor {path.name} is unreadable: {exc}")
        if not isinstance(descriptor, dict):
            raise ManifestError(f"descriptor {path.name} is not a JSON object")
        generation, audit = build_generation(descriptor, path.name,
                                             args.installed_utc,
                                             args.skip_structural_check)
        generations.append(generation)
        audits.append(audit)

    manifest = {"schema": SCHEMA, "generations": generations}
    validate_manifest_object(manifest)

    text = serialize(manifest)
    leaks = scan_for_leaks(text) + scan_for_absolute_paths(manifest)
    if leaks:
        raise ManifestError(
            "refusing to publish a manifest containing " + ", ".join(leaks))
    # The bytes must survive a round trip unchanged before anything is published.
    if json.loads(text) != manifest:
        raise ManifestError("internal: serialized manifest does not round-trip")

    result = {
        "tool": "gen_model_manifest.py",
        "schema": SCHEMA,
        "manifest": output.name,
        # From the bytes about to be written, so every fallible step stays ahead
        # of publication.
        "manifest_sha256": hashlib.sha256(text.encode("utf-8")).hexdigest(),
        "manifest_bytes": len(text.encode("utf-8")),
        "generation_count": len(generations),
        "generations": audits,
        "leak_scan": "clean",
    }
    # Stage EVERYTHING first, then publish, with the manifest LAST — see the
    # matching comment in gen_names_sidecar.main.
    to_stage = []
    if report_path_arg is not None:
        to_stage.append((report_path_arg,
                         json.dumps(result, indent=2, sort_keys=True) + "\n"))
    to_stage.append((output, text))                             # accepted LAST
    try:
        sc.publish_all(sc.stage_all(to_stage), args.overwrite)
    except OSError as exc:
        raise ManifestError(f"cannot write the requested outputs: {exc}")

    print(json.dumps(result, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (ManifestError, sc.SidecarError, ex.ExportError) as error:
        print(f"gen_model_manifest: {ex.redact(str(error))}", file=sys.stderr)
        sys.exit(1)
