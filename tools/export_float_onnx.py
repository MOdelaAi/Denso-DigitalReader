#!/usr/bin/env python3
"""Pinned-version `.pt` -> `.onnx` exporter for the Floating Ball models.

Release A, Slice 2 of the model/operating-mode compatibility work. This tool is
deliberately OUTSIDE the CMake graph: it touches no production source, builds no
engine, and installs nothing anywhere the application can see. It exports into an
isolated output directory and writes a machine-readable provenance report beside
each artifact.

It is FAIL-CLOSED at every step. It refuses to export unless the source
checkpoint's SHA-256 matches the expected value, refuses to accept an emitted
ONNX that does not satisfy every `TrtEngine` precondition, and refuses to record
an exporter-version deviation that the caller has not explicitly declared.

Why the emitted model is validated rather than the exporter's exit code: an
Ultralytics export can succeed and still produce a graph the appliance cannot
run. The end-to-end (NMS-free) variant emits `output0 [1, N, 6]`, which
`TrtEngine` routes to `decode_yolo_end2end`; the Floating Ball models are classic
YOLOv8 detection heads and MUST emit `[1, 4 + nc, anchors]` = `[1, 5, 8400]` so
the `decode_yolo` branch is selected instead. Nothing but reading the produced
graph can tell those apart.

Typical use (paths are relative to the repository root):

    python tools/export_float_onnx.py \\
        --model models/float-small.pt \\
        --output-dir <isolated-output-dir> \\
        --expected-class Small \\
        --expected-training-version 8.4.33

Validating an already-exported artifact, without exporting:

    python tools/export_float_onnx.py \\
        --validate-onnx <dir>/float-small.onnx --expected-class Small
"""

from __future__ import annotations

import argparse
import ast
import datetime as _dt
import hashlib
import json
import os
import re
import shutil
import sys
import tempfile
from pathlib import Path

# --- The export recipe, fixed to the repository's existing digitv3 process. ---
# `train_model/train.py:86` invokes exactly these arguments. digitv3 was exported
# this way and Ultralytics settled it on opset 12 despite opset=13 being
# requested, which is why the ACTUAL emitted opset is read back and recorded
# rather than asserted.
EXPORT_FORMAT = "onnx"
EXPORT_IMGSZ = 640
EXPORT_SIMPLIFY = True
REQUESTED_OPSET = 13

# --- Required properties of every emitted ONNX (spec 8.2/8.3). ---
EXPECTED_TASK = "detect"
EXPECTED_INPUT_NAME = "images"
EXPECTED_OUTPUT_NAME = "output0"
EXPECTED_INPUT_SHAPE = [1, 3, 640, 640]
EXPECTED_ANCHORS = 8400
EXPECTED_CLASS_COUNT = 1
EXPECTED_BATCH = 1
ONNX_DTYPE_FLOAT32 = 1  # onnx.TensorProto.FLOAT

# --- Known source checkpoints (spec 8.1, read from the checkpoints themselves).
# Supplying an expected hash is MANDATORY: a stem that is not listed here must be
# given one explicitly with --expected-sha256, or the export is refused. A tool
# that exports whatever it is handed is not a provenance tool.
KNOWN_SOURCES = {
    "float-small": "e9a2294757cc13c1041f643f83d2651616434d016d3f4537d81517ea10d7330f",
    "float-big": "3cf0a655af70bcaa960cf96e174cffedf1c8ca41d70293fd19a88fe07727c3b1",
}


class ExportError(RuntimeError):
    """Any fail-closed refusal. Reported to stderr; exit code 1."""


# --------------------------------------------------------------------------- #
# Redaction
# --------------------------------------------------------------------------- #

def redact(text: str) -> str:
    """Strip machine usernames and home-directory paths from recorded text.

    Provenance is committed-adjacent and read by other people. A pip or torch
    traceback is full of absolute site-packages paths carrying the operator's
    account name; none of it is needed to understand what failed.
    """
    if not text:
        return text
    out = text

    # 1. URL userinfo. Matches `scheme://user:pass@host` AND `scheme://tok@host`
    #    — a single-token userinfo is just as much a credential.
    out = re.sub(r"(?i)\b([a-z][a-z0-9+.\-]*://)[^\s/@\"']+@",
                 r"\1<redacted>@", out)

    # 2. `Authorization: Bearer <token>` — MUST run before the keyword rule
    #    below, which would otherwise consume the word "Bearer" as the value and
    #    leave the actual token in the clear.
    out = re.sub(r"(?i)\b(Bearer|Basic|Digest)(\s+)([A-Za-z0-9._\-+/=]{8,})",
                 r"\1\2<redacted>", out)

    # 3/4. `password=...`, `token: ...`, `--api-key ...`.
    #
    # An explicit separator says the token is a VALUE, but not that the value is
    # a secret: "--token is required" and "token: missing from metadata" are
    # ordinary diagnostics, and redacting their next word corrupts evidence the
    # spec requires be captured verbatim. So a short stop-list of status words is
    # left alone. Everything else is redacted — the bias is toward redaction.
    secret_word = (r"(?:password|passwd|secret|token|api[_\-]?key|credential|"
                   r"access[_\-]?token|client[_\-]?secret|private[_\-]?key|"
                   r"aws[_\-]secret[_\-]access[_\-]key|authorization)")
    not_a_secret = {
        "is", "are", "was", "were", "not", "no", "none", "null", "missing",
        "required", "unavailable", "unknown", "empty", "invalid", "expired",
        "from", "to", "for", "the", "a", "an", "and", "or", "in", "of", "with",
    }

    def _mask(match: "re.Match") -> str:
        head, sep, value = match.group(1), match.group(2), match.group(3)
        if value.strip("\"'").lower() in not_a_secret:
            return match.group(0)
        return f"{head}{sep}<redacted>"

    out = re.sub(rf"(?i)\b({secret_word})(\s*[:=]\s*)(\"[^\"]*\"|'[^']*'|\S+)",
                 _mask, out)
    out = re.sub(rf"(?i)(--{secret_word})(\s+)(\"[^\"]*\"|'[^']*'|\S+)",
                 _mask, out)

    # 3. Home directories and account names.
    home = os.path.expanduser("~")
    for variant in {home, home.replace("\\", "/"), home.replace("/", "\\")}:
        if variant:
            out = out.replace(variant, "<home>")
    for var in ("USERNAME", "USER", "USERPROFILE", "LOGNAME"):
        value = os.environ.get(var) or ""
        if value and len(value) > 2:
            out = re.sub(rf"\b{re.escape(value)}\b", "<user>", out)
    # Windows, macOS and POSIX user-directory prefixes that survived the above.
    out = re.sub(r"[A-Za-z]:[\\/]Users[\\/][^\\/\s\"']+", "<home>", out)
    out = re.sub(r"(?<![\w])/Users/[^/\s\"']+", "<home>", out)
    out = re.sub(r"(?<![\w])/home/[^/\s\"']+", "<home>", out)
    # UNC profile shares, e.g. \\server\users\name
    out = re.sub(r"(?i)\\\\[^\\\s]+\\users?\\[^\\\s\"']+", "<home>", out)
    return out


# --------------------------------------------------------------------------- #
# Output containment
# --------------------------------------------------------------------------- #

def ensure_safe_output_dir(out_dir: Path, repo_root: Path) -> None:
    """Refuse to write a Float artifact anywhere the application can see it.

    Release A carries no Float artifact at all (spec 8.7/8.7.1): not in the
    `.deb`, not under `/opt/denso`, and not in any application-visible directory.
    `models/` in particular is scanned by `EngineRegistry::warm_up`,
    `sync_models` and `evaluate_integrity`, so an `.onnx` dropped there would be
    loaded at the next boot. The output directory is caller-supplied, so the
    rule is enforced here rather than trusted.
    """
    resolved = out_dir.resolve()
    parts_lower = [p.lower() for p in resolved.parts]

    try:
        resolved.relative_to(repo_root.resolve())
        inside_repo = True
    except ValueError:
        inside_repo = False  # Outside the repo, which is what we want.
    if inside_repo:
        raise ExportError(
            f"refusing to write into the repository working tree ({out_dir}). "
            "Release A carries no Float artifact; export to an isolated "
            "directory outside the repo."
        )

    # Component-wise, not substring: `/tmp/opt/denso-backup/out` is a different
    # directory than `/opt/denso` and must not be refused for merely containing
    # the characters.
    for i in range(len(parts_lower) - 1):
        if parts_lower[i] == "opt" and parts_lower[i + 1] == "denso":
            raise ExportError(
                f"refusing to write under /opt/denso ({out_dir}): that is an "
                "application-visible path."
            )
    if "models" in parts_lower:
        raise ExportError(
            f"refusing to write into a 'models' directory ({out_dir}): such "
            "directories are scanned by warm-up and would load the artifact."
        )


def relative_path(path: Path) -> str:
    """Record a repo-relative path when possible, a bare filename otherwise."""
    resolved = Path(path).resolve()
    for base in (Path.cwd().resolve(), Path(__file__).resolve().parent.parent):
        try:
            return resolved.relative_to(base).as_posix()
        except ValueError:
            continue
    return resolved.name


# --------------------------------------------------------------------------- #
# Hashing
# --------------------------------------------------------------------------- #

def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


# --------------------------------------------------------------------------- #
# Checkpoint inspection
# --------------------------------------------------------------------------- #

def read_checkpoint_metadata(pt_path: Path) -> dict:
    """Read the checkpoint's own recorded metadata BEFORE exporting anything.

    The Ultralytics version that wrote the checkpoint is the `version` key; it is
    the authority for `training_ultralytics` and must never be conflated with the
    version doing the export.
    """
    import torch

    ckpt = torch.load(str(pt_path), map_location="cpu", weights_only=False)
    if not isinstance(ckpt, dict):
        raise ExportError(f"{pt_path.name}: checkpoint is not a dict")

    model = ckpt.get("model")
    names = getattr(model, "names", None) or {}
    if isinstance(names, dict):
        ordered = [names[k] for k in sorted(names, key=int)]
    else:
        ordered = list(names)

    train_args = ckpt.get("train_args") or {}
    return {
        "training_ultralytics": str(ckpt.get("version") or "").strip(),
        "trained_date": str(ckpt.get("date") or "").strip(),
        "task": str(getattr(model, "task", "") or train_args.get("task", "")),
        "class_names": ordered,
        "class_count": len(ordered),
        "train_imgsz": train_args.get("imgsz"),
    }


# --------------------------------------------------------------------------- #
# ONNX inspection + validation
# --------------------------------------------------------------------------- #

def _parse_metadata_literal(raw: str, field: str):
    """Ultralytics stores `names` and `args` as `str(dict)`, not as JSON."""
    try:
        return ast.literal_eval(raw)
    except (ValueError, SyntaxError) as exc:
        raise ExportError(f"ONNX metadata `{field}` is not a Python literal: {exc}")


def _tensor_shape(value_info) -> tuple[list, bool]:
    """Return (shape, has_dynamic_dim). A symbolic dim is recorded by name."""
    shape: list = []
    dynamic = False
    for dim in value_info.type.tensor_type.shape.dim:
        if dim.HasField("dim_value"):
            shape.append(int(dim.dim_value))
        else:
            shape.append(dim.dim_param or "?")
            dynamic = True
    return shape, dynamic


def check_graph_validity(onnx_path: Path) -> None:
    """Structural validation, separate from the contract checks below.

    Shape and metadata agreement says nothing about whether the graph can
    actually run: a model with the right tensor signature and no working body
    would satisfy every contract rule here. `onnx.checker` is the cheap
    structural gate; `trtexec` on `.15` (Slice 3) is the expensive one.
    """
    import onnx

    try:
        onnx.checker.check_model(str(onnx_path), full_check=True)
    except Exception as exc:  # onnx raises several distinct types
        raise ExportError(f"ONNX structural check failed: {exc}")


def inspect_onnx(onnx_path: Path) -> dict:
    """Read every property the validation rules below depend on."""
    import onnx

    model = onnx.load(str(onnx_path))

    opsets = {imp.domain or "": int(imp.version) for imp in model.opset_import}
    meta = {p.key: p.value for p in model.metadata_props}

    initializers = {init.name for init in model.graph.initializer}
    inputs = [i for i in model.graph.input if i.name not in initializers]
    outputs = list(model.graph.output)

    facts: dict = {
        "actual_onnx_opset": opsets.get("", None),
        "opset_imports": opsets,
        "input_count": len(inputs),
        "output_count": len(outputs),
        "raw_metadata_keys": sorted(meta),
    }

    if len(inputs) == 1:
        shape, dynamic = _tensor_shape(inputs[0])
        facts["input_name"] = inputs[0].name
        facts["input_shape"] = shape
        facts["input_dtype_enum"] = int(inputs[0].type.tensor_type.elem_type)
        facts["input_dynamic"] = dynamic
    if len(outputs) == 1:
        shape, dynamic = _tensor_shape(outputs[0])
        facts["output_name"] = outputs[0].name
        facts["output_shape"] = shape
        facts["output_dtype_enum"] = int(outputs[0].type.tensor_type.elem_type)
        facts["output_dynamic"] = dynamic

    facts["task"] = meta.get("task", "")
    facts["metadata_batch"] = meta.get("batch")
    facts["metadata_imgsz"] = meta.get("imgsz")

    names_raw = meta.get("names")
    if names_raw is None:
        facts["class_names"] = None
        facts["class_count"] = None
    else:
        names = _parse_metadata_literal(names_raw, "names")
        if not isinstance(names, dict):
            raise ExportError("ONNX metadata `names` is not a dict")
        facts["class_names"] = [names[k] for k in sorted(names, key=int)]
        facts["class_count"] = len(names)
        facts["class_ids"] = sorted(int(k) for k in names)

    args_raw = meta.get("args")
    args = _parse_metadata_literal(args_raw, "args") if args_raw else {}
    if not isinstance(args, dict):
        raise ExportError("ONNX metadata `args` is not a dict")
    facts["export_args"] = args
    for key in ("dynamic", "nms", "half", "simplify"):
        facts[key] = args.get(key)
    facts["args_opset"] = args.get("opset")
    facts["args_batch"] = args.get("batch")

    return facts


def _dtype_name(enum_value) -> str:
    return {1: "FP32", 10: "FP16", 11: "FP64"}.get(enum_value, f"enum:{enum_value}")


def validate_onnx_facts(facts: dict, expected_class: str) -> list[str]:
    """Return a list of violations. Empty means the artifact is acceptable.

    Deterministic and side-effect free: it reads `facts` only, so running it
    twice over the same artifact yields the same list in the same order.
    """
    problems: list[str] = []

    def bad(msg: str) -> None:
        problems.append(msg)

    # -- Graph arity -------------------------------------------------------- #
    if facts.get("input_count") != 1:
        bad(f"expected exactly 1 input, found {facts.get('input_count')}")
    if facts.get("output_count") != 1:
        bad(f"expected exactly 1 output, found {facts.get('output_count')}")

    # -- Task --------------------------------------------------------------- #
    if facts.get("task") != EXPECTED_TASK:
        bad(f"task must be {EXPECTED_TASK!r}, found {facts.get('task')!r}")

    # -- Input -------------------------------------------------------------- #
    if "input_name" in facts:
        if facts["input_name"] != EXPECTED_INPUT_NAME:
            bad(f"input name must be {EXPECTED_INPUT_NAME!r}, "
                f"found {facts['input_name']!r}")
        if facts.get("input_dynamic"):
            bad(f"input shape has a dynamic dimension: {facts['input_shape']}")
        elif facts["input_shape"] != EXPECTED_INPUT_SHAPE:
            bad(f"input shape must be {EXPECTED_INPUT_SHAPE}, "
                f"found {facts['input_shape']}")
        if facts.get("input_dtype_enum") != ONNX_DTYPE_FLOAT32:
            bad("input dtype must be FP32, found "
                f"{_dtype_name(facts.get('input_dtype_enum'))}")

    # -- Output: the decode-branch contract --------------------------------- #
    # `[1, 5, 8400]` selects decode_yolo. `[1, N, 6]` is the end-to-end (NMS-free)
    # layout, which selects decode_yolo_end2end and is NOT what these models are.
    if "output_name" in facts:
        expected_output_shape = [1, 4 + EXPECTED_CLASS_COUNT, EXPECTED_ANCHORS]
        if facts["output_name"] != EXPECTED_OUTPUT_NAME:
            bad(f"output name must be {EXPECTED_OUTPUT_NAME!r}, "
                f"found {facts['output_name']!r}")
        if facts.get("output_dynamic"):
            bad(f"output shape has a dynamic dimension: {facts['output_shape']}")
        else:
            shape = facts["output_shape"]
            if len(shape) == 3 and shape[2] == 6:
                bad(f"output shape {shape} is an END-TO-END (NMS-free) export; "
                    "this model must keep the classic YOLOv8 detection head so "
                    "TrtEngine selects decode_yolo, not decode_yolo_end2end")
            elif shape != expected_output_shape:
                bad(f"output shape must be {expected_output_shape}, found {shape}")
        if facts.get("output_dtype_enum") != ONNX_DTYPE_FLOAT32:
            bad("output dtype must be FP32, found "
                f"{_dtype_name(facts.get('output_dtype_enum'))}")

    # -- Export arguments --------------------------------------------------- #
    if facts.get("dynamic") is not False:
        bad(f"export arg `dynamic` must be False, found {facts.get('dynamic')!r}")
    if facts.get("nms") is not False:
        bad(f"export arg `nms` must be False, found {facts.get('nms')!r}")
    if facts.get("half") is not False:
        bad(f"export arg `half` must be False, found {facts.get('half')!r}")
    if facts.get("simplify") is not True:
        bad(f"export arg `simplify` must be True, found {facts.get('simplify')!r}")

    # -- Batch -------------------------------------------------------------- #
    batch = facts.get("metadata_batch")
    if batch is not None and str(batch) != str(EXPECTED_BATCH):
        bad(f"batch must be {EXPECTED_BATCH}, found {batch!r}")
    if facts.get("args_batch") not in (None, EXPECTED_BATCH):
        bad(f"export arg `batch` must be {EXPECTED_BATCH}, "
            f"found {facts['args_batch']!r}")

    # -- imgsz -------------------------------------------------------------- #
    imgsz = facts.get("metadata_imgsz")
    if imgsz is not None:
        parsed = imgsz
        if isinstance(imgsz, str):
            try:
                parsed = ast.literal_eval(imgsz)
            except (ValueError, SyntaxError):
                parsed = imgsz
        if parsed not in (EXPORT_IMGSZ, [EXPORT_IMGSZ, EXPORT_IMGSZ],
                          (EXPORT_IMGSZ, EXPORT_IMGSZ)):
            bad(f"imgsz must be {EXPORT_IMGSZ}, found {imgsz!r}")

    # -- Class metadata ----------------------------------------------------- #
    names = facts.get("class_names")
    if names is None:
        bad("ONNX metadata carries no `names` entry")
    else:
        if facts.get("class_count") != EXPECTED_CLASS_COUNT:
            bad(f"class count must be {EXPECTED_CLASS_COUNT}, "
                f"found {facts.get('class_count')}")
        if names != [expected_class]:
            bad(f"class names must be exactly {[expected_class]}, found {names}")
        ids = facts.get("class_ids")
        if ids is not None and ids != list(range(len(ids))):
            bad(f"class ids must be contiguous from 0, found {ids}")

    # -- Opset -------------------------------------------------------------- #
    # The REQUESTED opset is not asserted: Ultralytics settled digitv3 on 12 from
    # a requested 13. Both values are recorded; only its presence is required.
    graph_opset = facts.get("actual_onnx_opset")
    if graph_opset is None:
        bad("ONNX declares no default-domain opset import")

    # The spec asks for the emitted `args` metadata to be read back. It is a
    # SECOND witness to the same fact, so a disagreement between the exporter's
    # own record and the graph it emitted is itself a defect worth refusing.
    args_opset = facts.get("args_opset")
    if args_opset is None:
        bad("ONNX metadata `args` carries no `opset` entry")
    elif graph_opset is not None and int(args_opset) != int(graph_opset):
        bad(f"opset disagreement: graph opset_import says {graph_opset}, "
            f"exporter args metadata says {args_opset}")

    return problems


# --------------------------------------------------------------------------- #
# Export
# --------------------------------------------------------------------------- #

def run_export(pt_path: Path, work_dir: Path) -> tuple[Path, str]:
    """Export a COPY of the checkpoint, so the source is provably untouched.

    Ultralytics writes the `.onnx` beside the `.pt` it was given. Handing it the
    real checkpoint would put a generated artifact into `models/` as a side
    effect; copying first keeps every write inside the throwaway working dir.
    """
    from ultralytics import YOLO
    import ultralytics

    staged = work_dir / pt_path.name
    shutil.copy2(pt_path, staged)

    model = YOLO(str(staged))
    produced = model.export(
        format=EXPORT_FORMAT,
        imgsz=EXPORT_IMGSZ,
        simplify=EXPORT_SIMPLIFY,
        opset=REQUESTED_OPSET,
    )
    produced_path = Path(str(produced))
    if not produced_path.is_absolute():
        produced_path = (work_dir / produced_path).resolve()
    if not produced_path.exists():
        raise ExportError(f"exporter reported {produced_path.name}, which does not exist")
    return produced_path, str(ultralytics.__version__)


def export_command_text(pt_name: str) -> str:
    return (
        f'YOLO("{pt_name}").export(format="{EXPORT_FORMAT}", '
        f"imgsz={EXPORT_IMGSZ}, simplify={EXPORT_SIMPLIFY}, opset={REQUESTED_OPSET})"
    )


# --------------------------------------------------------------------------- #
# Driver
# --------------------------------------------------------------------------- #

def do_validate_only(args) -> int:
    onnx_path = Path(args.validate_onnx)
    if not onnx_path.is_file():
        raise ExportError(f"{onnx_path} does not exist")
    structural: list[str] = []
    if not args.skip_structural_check:
        try:
            check_graph_validity(onnx_path)
        except ExportError as exc:
            structural.append(str(exc))
    facts = inspect_onnx(onnx_path)
    problems = structural + validate_onnx_facts(facts, args.expected_class)
    report = {
        "mode": "validate-only",
        "onnx": relative_path(onnx_path),
        "onnx_sha256": sha256_file(onnx_path),
        "actual_onnx_opset": facts.get("actual_onnx_opset"),
        "task": facts.get("task"),
        "input_name": facts.get("input_name"),
        "input_shape": facts.get("input_shape"),
        "input_dtype": _dtype_name(facts.get("input_dtype_enum")),
        "output_name": facts.get("output_name"),
        "output_shape": facts.get("output_shape"),
        "output_dtype": _dtype_name(facts.get("output_dtype_enum")),
        "class_names": facts.get("class_names"),
        "class_count": facts.get("class_count"),
        "dynamic": facts.get("dynamic"),
        "nms": facts.get("nms"),
        "half": facts.get("half"),
        "simplify": facts.get("simplify"),
        "valid": not problems,
        "violations": problems,
    }
    print(json.dumps(report, indent=2, sort_keys=True))
    if problems:
        for problem in problems:
            print(f"REJECTED: {problem}", file=sys.stderr)
        return 1
    return 0


def do_export(args) -> int:
    pt_path = Path(args.model)
    if not pt_path.is_file():
        raise ExportError(f"{args.model} does not exist")

    stem = pt_path.stem

    # -- 1/2. Verify the source hash BEFORE loading or exporting anything. --- #
    expected_sha = args.expected_sha256 or KNOWN_SOURCES.get(stem)
    if not expected_sha:
        raise ExportError(
            f"no expected SHA-256 for {pt_path.name!r}: pass --expected-sha256. "
            "This tool refuses to export a checkpoint it cannot identify."
        )
    expected_sha = expected_sha.strip().lower()
    actual_sha = sha256_file(pt_path)
    if actual_sha != expected_sha:
        raise ExportError(
            f"source SHA-256 mismatch for {pt_path.name}\n"
            f"  expected: {expected_sha}\n"
            f"  actual:   {actual_sha}\n"
            "Refusing to export. Do NOT substitute another checkpoint."
        )

    # -- Output-dir containment + overwrite safety, before any work is done. - #
    out_dir = Path(args.output_dir)
    ensure_safe_output_dir(out_dir, Path(__file__).resolve().parent.parent)
    out_dir.mkdir(parents=True, exist_ok=True)
    onnx_out = out_dir / f"{stem}.onnx"
    provenance_out = out_dir / f"{stem}.provenance.json"
    if not args.overwrite:
        for existing in (onnx_out, provenance_out):
            if existing.exists():
                raise ExportError(
                    f"{existing.name} already exists in the output directory. "
                    "Refusing to overwrite; pass --overwrite to replace it."
                )

    # -- 3/4. Load the checkpoint and record its metadata BEFORE export. ----- #
    ckpt = read_checkpoint_metadata(pt_path)
    training_version = ckpt["training_ultralytics"]
    if not training_version:
        # Without it, the deviation check below has nothing to compare against
        # and would fail OPEN — silently permitting the version substitution the
        # whole mechanism exists to prevent.
        raise ExportError(
            f"{pt_path.name} records no Ultralytics training version; refusing "
            "to export, because the exporter-deviation check cannot be evaluated."
        )
    if args.expected_training_version:
        if training_version != args.expected_training_version:
            raise ExportError(
                f"{pt_path.name} records training Ultralytics "
                f"{training_version!r}, expected "
                f"{args.expected_training_version!r}"
            )
    if ckpt["class_names"] != [args.expected_class]:
        raise ExportError(
            f"{pt_path.name} declares classes {ckpt['class_names']}, "
            f"expected {[args.expected_class]}"
        )

    # -- Read the deviation evidence BEFORE exporting, so a missing file fails
    #    the run before any artifact exists. ------------------------------- #
    deviation_reason = (args.controlled_deviation_reason or "").strip()
    deviation_failure = ""
    if args.controlled_deviation_failure_file:
        failure_path = Path(args.controlled_deviation_failure_file)
        if not failure_path.is_file():
            raise ExportError(f"{failure_path} does not exist")
        deviation_failure = redact(
            failure_path.read_text(encoding="utf-8", errors="replace")).strip()

    # -- 5/6/7. Export, then validate, ENTIRELY inside a throwaway directory.
    # Nothing is published to the output directory until every check has passed.
    # Publishing first and cleaning up afterwards cannot be made airtight: any
    # unhandled failure between the copy and the last check — a malformed graph,
    # an unreadable metadata literal, a failed write — would strand an
    # unvalidated .onnx exactly where a later directory scan would find it. With
    # --overwrite it would additionally have destroyed a known-good artifact to
    # do so.
    with tempfile.TemporaryDirectory(prefix=f"export-{stem}-") as tmp:
        work_dir = Path(tmp)
        produced, export_version = run_export(pt_path, work_dir)

        # -- Exporter-version deviation: explicit, evidenced, never silent. -- #
        deviation = export_version != training_version
        if deviation:
            if not deviation_reason:
                raise ExportError(
                    f"exporter version {export_version} differs from the "
                    f"recorded training version {training_version}, and no "
                    "--controlled-deviation-reason was supplied.\n"
                    "A version substitution must be an explicitly declared, "
                    "recorded controlled deviation (spec 8.3). Silent fallback "
                    "is forbidden."
                )
            if not deviation_failure:
                raise ExportError(
                    f"exporter version {export_version} differs from the "
                    f"recorded training version {training_version}, but no "
                    "--controlled-deviation-failure-file was supplied.\n"
                    "Spec 8.3 requires the failure that forced the fallback to "
                    "be captured VERBATIM. A deviation asserted without its "
                    "evidence is not a controlled deviation."
                )
        elif deviation_reason or deviation_failure:
            raise ExportError(
                f"a controlled deviation was declared, but the exporter "
                f"({export_version}) matches the training version "
                f"({training_version}); there is no deviation to record."
            )

        # Validate the EMITTED artifact, not the exporter's exit code.
        check_graph_validity(produced)
        facts = inspect_onnx(produced)
        problems = validate_onnx_facts(facts, args.expected_class)
        onnx_sha = sha256_file(produced)

        report = build_report(
            pt_path=pt_path, actual_sha=actual_sha,
            training_version=training_version, export_version=export_version,
            deviation=deviation, deviation_reason=deviation_reason,
            deviation_failure=deviation_failure, facts=facts,
            onnx_name=onnx_out.name, onnx_sha=onnx_sha, ckpt=ckpt,
            problems=problems)

        if problems:
            # The artifact was never published — it stayed here and dies with
            # this directory. The rejection record goes to a DISTINCT filename:
            # overwriting <stem>.provenance.json would leave an existing, valid
            # .onnx described by a report that says no artifact was produced.
            report["onnx"] = None
            report["onnx_sha256"] = None
            report["rejected_artifact_sha256"] = onnx_sha
            rejected_out = out_dir / f"{stem}.rejected.provenance.json"
            rejected_out.write_text(
                json.dumps(report, indent=2, sort_keys=True) + "\n",
                encoding="utf-8")
            for problem in problems:
                print(f"REJECTED: {problem}", file=sys.stderr)
            raise ExportError(
                f"{stem}: emitted ONNX failed {len(problems)} validation "
                f"check(s); it was never published. See {rejected_out.name}."
            )

        publish_pair(produced, onnx_out, report, provenance_out)

    print(json.dumps(report, indent=2, sort_keys=True))
    return 0


def build_report(*, pt_path, actual_sha, training_version, export_version,
                 deviation, deviation_reason, deviation_failure, facts,
                 onnx_name, onnx_sha, ckpt, problems) -> dict:
    return {
        "source_pt": relative_path(pt_path),
        "source_pt_sha256": actual_sha,
        "training_ultralytics": training_version,
        "export_ultralytics": export_version,
        "controlled_deviation": deviation,
        "controlled_deviation_reason": deviation_reason or None,
        "controlled_deviation_failure_verbatim": deviation_failure or None,
        "export_command": export_command_text(pt_path.name),
        "requested_opset": REQUESTED_OPSET,
        "actual_onnx_opset": facts.get("actual_onnx_opset"),
        "actual_onnx_opset_from_args_metadata": facts.get("args_opset"),
        "onnx": onnx_name,
        "onnx_sha256": onnx_sha,
        "task": facts.get("task"),
        "input_name": facts.get("input_name"),
        "input_shape": facts.get("input_shape"),
        "input_dtype": _dtype_name(facts.get("input_dtype_enum")),
        "output_name": facts.get("output_name"),
        "output_shape": facts.get("output_shape"),
        "output_dtype": _dtype_name(facts.get("output_dtype_enum")),
        "class_names": facts.get("class_names"),
        "class_count": facts.get("class_count"),
        "imgsz": EXPORT_IMGSZ,
        "batch": EXPECTED_BATCH,
        "dynamic": facts.get("dynamic"),
        "nms": facts.get("nms"),
        "half": facts.get("half"),
        "simplify": facts.get("simplify"),
        "checkpoint_trained_date": ckpt.get("trained_date"),
        "checkpoint_class_names": ckpt.get("class_names"),
        "export_timestamp": _dt.datetime.now(_dt.timezone.utc)
                              .strftime("%Y-%m-%dT%H:%M:%SZ"),
        "validation_passed": not problems,
        "validation_violations": problems,
    }


def publish_pair(produced: Path, onnx_out: Path, report: dict,
                 provenance_out: Path) -> None:
    """Publish the artifact and its provenance as a PAIR.

    Both are staged beside their destinations first, so a failure while copying
    12 MB of ONNX cannot leave a published artifact with no provenance — or,
    under --overwrite, replace a known-good artifact and then fail to describe
    it. Two renames is not a transaction, but it reduces the window to two
    metadata operations instead of a multi-megabyte copy plus a file write.
    """
    onnx_tmp = onnx_out.with_suffix(onnx_out.suffix + ".part")
    prov_tmp = provenance_out.with_suffix(provenance_out.suffix + ".part")
    try:
        shutil.copy2(produced, onnx_tmp)
        prov_tmp.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n",
                            encoding="utf-8")
        os.replace(onnx_tmp, onnx_out)
        os.replace(prov_tmp, provenance_out)
    finally:
        for leftover in (onnx_tmp, prov_tmp):
            if leftover.exists():
                leftover.unlink(missing_ok=True)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Export a Floating Ball .pt checkpoint to ONNX with pinned "
                    "arguments, validate the emitted graph, and record provenance.",
    )
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--model", help="path to the source .pt checkpoint")
    source.add_argument("--validate-onnx",
                        help="validate an existing .onnx and exit; exports nothing")

    parser.add_argument("--output-dir",
                        help="isolated directory for the .onnx and its provenance "
                             "report (required with --model)")
    parser.add_argument("--expected-class", required=True,
                        help="the single class name the model must declare, "
                             'e.g. "Small" or "Big"')
    parser.add_argument("--expected-training-version",
                        help="Ultralytics version the checkpoint must record as "
                             "its training version")
    parser.add_argument("--expected-sha256",
                        help="expected SHA-256 of the source .pt; defaults to the "
                             "recorded hash for a known checkpoint stem")
    parser.add_argument("--controlled-deviation-reason",
                        help="why the exporting Ultralytics version differs from "
                             "the training version. REQUIRED when they differ.")
    parser.add_argument("--controlled-deviation-failure-file",
                        help="file holding the verbatim failure that forced the "
                             "deviation; recorded redacted")
    parser.add_argument("--overwrite", action="store_true",
                        help="replace existing output files (refused by default)")
    parser.add_argument("--skip-structural-check", action="store_true",
                        help="with --validate-onnx only: skip onnx.checker and "
                             "report the contract checks alone. Never available "
                             "on the export path.")
    return parser


def main(argv=None) -> int:
    args = build_parser().parse_args(argv)
    try:
        if args.validate_onnx:
            return do_validate_only(args)
        if not args.output_dir:
            raise ExportError("--output-dir is required with --model")
        return do_export(args)
    except ExportError as exc:
        print(f"ERROR: {redact(str(exc))}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
