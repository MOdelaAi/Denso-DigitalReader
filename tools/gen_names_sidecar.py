#!/usr/bin/env python3
"""Generate a `<stem>.names.json` class-name sidecar from an artifact's own metadata.

Release A, Slice 4 of the model/operating-mode compatibility work. Like
`export_float_onnx.py` this tool lives OUTSIDE the CMake graph: it touches no
production source, builds nothing, and writes only into an isolated directory
outside the repository and outside every application-visible path.

WHY THIS IS A TOOL AND NOT A TYPED FILE
--------------------------------------
A sidecar looks trivial — `["Small"]` is eleven bytes — which is exactly why it
is dangerous to hand-write. `TrtEngine` reads class names from the sidecar
because a TensorRT plan carries none, so the sidecar IS the engine's class
vocabulary. AGENTS.md records the failure mode: a stale or wrong sidecar keeps
an old class order and makes readings silently *wrong* rather than failing.
Nothing at runtime can detect it.

So the class names here are never typed. They are read from the ONNX `names`
metadata — the same metadata the exporter validated in Slice 2 — and then
cross-checked against the TensorRT build evidence from Slice 3:

    ONNX metadata names  ->  the sidecar's content and order
    ONNX output shape    ->  [1, nc + 4, anchors]        (classic YOLO head)
    Slice-3 engine report->  the SAME output shape, measured by trtexec
    decoded class count  ->  output_channels - 4 == len(names)

Any disagreement between those four is a refusal, not a warning.

CALLER-SUPPLIED EXPECTATIONS, NOT BUILT-IN POLICY
-------------------------------------------------
`--expect-class-count` and `--expect-class-name` exist so an invocation can
assert what it believes it is generating. They are deliberately NOT defaults:
baking "exactly one class called Small" into the tool would make it a Float
policy file rather than a generator, and the same tool must be able to describe
a ten-class digit model. The *call site* states the expectation; the tool proves
the artifact matches it.

DECODE BRANCH
-------------
Only the classic YOLOv8 detection head `[1, nc+4, anchors]` is generated for.
The end-to-end / NMS-free layout `[1, N, 6]` is refused: its channel count says
nothing about the class count, so the count agreement this tool exists to prove
cannot be established from it. (`digitv3` is such an export — output
`[1,300,6]` — and its sidecar already exists, is approved in
`packaging/models.approved`, and is not regenerated here.)

Typical use:

    python tools/gen_names_sidecar.py \\
        --onnx <artifacts>/float-small.onnx \\
        --engine-build-report <artifacts>/slice3-engine-build.json \\
        --engine <artifacts>/float-small.engine \\
        --expect-class-count 1 --expect-class-name Small \\
        --output <isolated-out>/float-small.names.json
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import sys
import tempfile
from pathlib import Path

# The Slice-2 exporter is the existing home of ONNX inspection, output-path
# containment and redaction. Importing it keeps ONE definition of each of those
# rules; re-implementing them here is how two tools start disagreeing about what
# a valid artifact is.
# Importing a sibling tool would drop a `tools/__pycache__/` directory into the
# working tree. That is not cosmetic here: `tools/build_package.sh` REFUSES to
# package a dirty tree, so merely running this generator from the repo would
# block the next release build. Bytecode caching is disabled before the import.
sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
import export_float_onnx as ex  # noqa: E402

ONNX_DTYPE_FLOAT32 = ex.ONNX_DTYPE_FLOAT32
BOX_CHANNELS = 4  # cx, cy, w, h — the non-class channels of a classic YOLO head


class SidecarError(RuntimeError):
    """Any fail-closed refusal. Reported to stderr; exit code 1."""


# --------------------------------------------------------------------------- #
# Slice-3 engine build report
# --------------------------------------------------------------------------- #

def load_engine_report(path: Path) -> list[dict]:
    """Read the Slice-3 build report and return its engine entries."""
    try:
        raw = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as exc:
        raise SidecarError(f"engine build report {path.name} is unreadable: {exc}")
    if not isinstance(raw, dict) or not isinstance(raw.get("engines"), list):
        raise SidecarError(
            f"engine build report {path.name} is malformed: expected an object "
            "with an `engines` array")
    if not raw["engines"]:
        raise SidecarError(f"engine build report {path.name} lists no engines")
    for entry in raw["engines"]:
        if not isinstance(entry, dict):
            raise SidecarError(
                f"engine build report {path.name} has a non-object engine entry")
    return raw["engines"]


def select_engine_entry(entries: list[dict], onnx_sha256: str,
                        onnx_name: str) -> dict:
    """Find the build entry for THIS ONNX, keyed on its hash.

    Keyed on the hash rather than the filename because the filename is the weaker
    identifier: two different exports can share a name, and Slice 3's whole point
    was that only one of four same-named ONNX files carried the approved bytes.
    """
    matches = [e for e in entries if e.get("source_onnx_sha256") == onnx_sha256]
    if not matches:
        raise SidecarError(
            f"engine build report contains no entry whose source_onnx_sha256 "
            f"matches {onnx_name} ({onnx_sha256}); the report describes a "
            "different artifact chain")
    if len(matches) > 1:
        raise SidecarError(
            f"engine build report contains {len(matches)} entries for "
            f"{onnx_sha256}; it is ambiguous")
    entry = matches[0]
    recorded_name = str(entry.get("source_onnx_filename") or "")
    if recorded_name != onnx_name:
        raise SidecarError(
            f"engine build report entry for this hash records source ONNX "
            f"{recorded_name!r}, but the supplied file is {onnx_name!r}")
    return entry


def is_exact_int(value) -> bool:
    """A JSON integer, and NOT a bool.

    `bool` is a subclass of `int` in Python, so `False == 0` and
    `[True, 5, 8400] == [1, 5, 8400]` are both true. Without this, JSON `false`
    passes for a successful build and `true` passes for a batch dimension —
    malformed evidence backing a published sidecar, which is the one thing this
    tool exists to prevent.
    """
    return isinstance(value, int) and not isinstance(value, bool)


def check_engine_entry(entry: dict, class_count: int, onnx_facts: dict) -> None:
    """Every TensorRT-side fact the sidecar's class count depends on.

    Every field is TYPE-checked before it is compared. An absent or wrong-typed
    field must produce a refusal naming it, never an IndexError from an unguarded
    `.split()[0]` or a silent bool/int equality.
    """
    # The engine filename is what binds this sidecar to a plan, so it is checked
    # before anything derived from it. Absent, it yields an empty stem — which
    # would then "match" an output literally named `.names.json`.
    filename = entry.get("engine_filename")
    if (not isinstance(filename, str) or not filename.endswith(".engine") or
            not filename[: -len(".engine")] or "/" in filename or
            "\\" in filename or ".." in filename):
        raise SidecarError(
            f"engine build report entry records engine_filename {filename!r}; a "
            "non-empty `<stem>.engine` basename is required to bind the sidecar "
            "to its engine")
    name = filename

    exit_code = entry.get("build_exit_code")
    if not is_exact_int(exit_code) or exit_code != 0:
        raise SidecarError(
            f"engine build report entry {name} records build_exit_code "
            f"{exit_code!r}; only an integer 0 (a successful build) may back a "
            "sidecar")

    engine_sha = str(entry.get("engine_sha256") or "")
    if not ex_is_lower_hex64(engine_sha):
        raise SidecarError(
            f"engine build report entry {name} has an invalid engine_sha256")

    shape = entry.get("output_shape")
    if (not isinstance(shape, list) or len(shape) != 3 or
            not all(is_exact_int(d) and d > 0 for d in shape)):
        raise SidecarError(
            f"engine build report entry {name} records output shape {shape!r}; "
            "a three-element list of positive integers is required")

    expected_shape = [1, class_count + BOX_CHANNELS, onnx_facts["output_shape"][2]]
    if shape != expected_shape:
        raise SidecarError(
            f"engine build report entry {name} records output shape {shape}, "
            f"but the ONNX class metadata implies {expected_shape}")

    dtype = entry.get("output_dtype")
    if not isinstance(dtype, str) or not dtype.split():
        raise SidecarError(
            f"engine build report entry {name} records output dtype {dtype!r}; "
            "a non-empty string is required")
    if dtype.split()[0].upper() != "FP32":
        raise SidecarError(
            f"engine build report entry {name} records output dtype {dtype!r}; "
            "FP32 bindings are required")

    if entry.get("output_name") != onnx_facts["output_name"]:
        raise SidecarError(
            f"engine build report entry {name} records output name "
            f"{entry.get('output_name')!r}, but the ONNX emits "
            f"{onnx_facts['output_name']!r}")

    decoded = int(shape[1]) - BOX_CHANNELS
    if decoded != class_count:
        raise SidecarError(
            f"engine decoded class count ({shape[1]} - {BOX_CHANNELS} = "
            f"{decoded}) does not equal the ONNX class count ({class_count})")


def ex_is_lower_hex64(value: str) -> bool:
    """Mirror of the C++ `is_lower_hex64` rule the manifest validator applies."""
    return len(value) == 64 and all(c in "0123456789abcdef" for c in value)


def stem_of_engine(value: str) -> str:
    """`float-small.engine` -> `float-small`; mirrors the C++ `stem_of`."""
    return value.rsplit(".", 1)[0] if "." in value else value


# --------------------------------------------------------------------------- #
# ONNX side
# --------------------------------------------------------------------------- #

def check_onnx_class_metadata(facts: dict, onnx_name: str) -> list[str]:
    """Return the ordered class names, refusing anything a sidecar must not carry."""
    names = facts.get("class_names")
    if names is None:
        raise SidecarError(
            f"{onnx_name}: ONNX metadata carries no `names` entry, so the class "
            "vocabulary cannot be derived from the artifact")
    if not names:
        raise SidecarError(f"{onnx_name}: ONNX metadata `names` is empty")
    for index, value in enumerate(names):
        if not isinstance(value, str):
            raise SidecarError(
                f"{onnx_name}: class {index} is {type(value).__name__}, not a "
                "string")
        if not value.strip():
            raise SidecarError(f"{onnx_name}: class {index} is blank")

    ids = facts.get("class_ids")
    if ids is None:
        raise SidecarError(f"{onnx_name}: ONNX metadata `names` carries no class ids")
    if ids != list(range(len(names))):
        raise SidecarError(
            f"{onnx_name}: class ids must be contiguous from 0, found {ids}")
    return list(names)


def check_onnx_shape(facts: dict, class_count: int, onnx_name: str) -> None:
    """The classic-head contract: [1, nc+4, anchors], FP32, static."""
    if facts.get("output_dynamic"):
        raise SidecarError(
            f"{onnx_name}: output shape has a dynamic dimension "
            f"({facts.get('output_shape')})")
    shape = facts.get("output_shape")
    if not isinstance(shape, list) or len(shape) != 3:
        raise SidecarError(f"{onnx_name}: unexpected output rank: {shape}")
    if shape[2] == 6:
        raise SidecarError(
            f"{onnx_name}: output {shape} is an END-TO-END (NMS-free) layout. "
            "Its channel count does not encode the class count, so the "
            "count agreement this tool proves cannot be established. Refusing.")
    expected = [1, class_count + BOX_CHANNELS, shape[2]]
    if shape != expected:
        raise SidecarError(
            f"{onnx_name}: output shape {shape} does not match the class "
            f"metadata; expected {expected} for {class_count} classes")
    if facts.get("output_dtype_enum") != ONNX_DTYPE_FLOAT32:
        raise SidecarError(f"{onnx_name}: output dtype must be FP32")
    if facts.get("input_dtype_enum") != ONNX_DTYPE_FLOAT32:
        raise SidecarError(f"{onnx_name}: input dtype must be FP32")
    if facts.get("input_dynamic"):
        raise SidecarError(
            f"{onnx_name}: input shape has a dynamic dimension "
            f"({facts.get('input_shape')})")


# --------------------------------------------------------------------------- #
# Output
# --------------------------------------------------------------------------- #

def serialize_sidecar(names: list[str]) -> str:
    """The repository's existing format: a BARE JSON array of strings.

    Deliberately compact and wrapper-free. `read_names_sidecar` accepts any JSON
    array of non-empty strings, so a wrapper object would be rejected outright,
    and extra keys are impossible to add without inventing a second format.

    NO trailing newline, and no space after the separators: this reproduces the
    committed `models/digitv3.names.json` BYTE FOR BYTE (41 bytes, sha256
    47538d10…9026, the value approved in `packaging/models.approved`). That is a
    hard requirement, not tidiness — the sidecar hash is an approval input, so a
    generator that could not reproduce the already-approved artifact would be
    describing a different file than the one in production.
    """
    return json.dumps(names, ensure_ascii=False, separators=(",", ":"))


def write_atomically(path: Path, text: str, overwrite: bool) -> None:
    """Publish only after the content is complete and fsync'd.

    A truncated sidecar is the worst outcome available here: a half-written array
    either fails to parse (loud, fine) or parses to a SHORTER list (silent, and
    every class id above the truncation point now means something else). So the
    file is never written in place.

    Two hazards a naive `exists()` + `replace()` leaves open, both closed here:

    * a FIXED temporary name (`<name>.part`) is shared by every concurrent run,
      so two invocations truncate each other's buffer and one deletes the other's
      file. The temporary is created exclusively, with a unique name, in the
      destination directory (same filesystem, so publication stays a rename).
    * `os.replace` overwrites unconditionally, so a destination created AFTER the
      existence check is clobbered even without `--overwrite`. When overwrite was
      not granted, publication goes through `os.link`, which fails atomically if
      the destination exists.
    """
    if path.exists() and not overwrite:
        raise SidecarError(
            f"{path} already exists; refusing to overwrite without --overwrite")
    publish_staged(stage_atomically(path, text), path, overwrite)


def stage_atomically(path: Path, text: str) -> Path:
    """Write the COMPLETE, fsync'd bytes to a unique temporary beside `path`.

    Separated from publication so that every requested output can be fully
    staged before ANY of them becomes visible. Once staging has succeeded, the
    only work left is renames — so a run cannot publish its primary artifact and
    then fail while producing something else.
    """
    handle_fd, tmp_name = tempfile.mkstemp(dir=str(path.parent),
                                           prefix=path.name + ".", suffix=".part")
    tmp = Path(tmp_name)
    try:
        with os.fdopen(handle_fd, "w", encoding="utf-8", newline="\n") as handle:
            handle.write(text)
            handle.flush()
            os.fsync(handle.fileno())
    except BaseException:
        tmp.unlink(missing_ok=True)
        raise
    return tmp


def publish_staged(tmp: Path, path: Path, overwrite: bool) -> None:
    """Rename a staged temporary onto its final name.

    Without `--overwrite` this uses `os.link`, which is an ATOMIC
    create-if-absent: a destination that appeared after the preflight check
    fails here instead of being clobbered.

    If the filesystem cannot do that, the run FAILS rather than degrading. The
    two tempting fallbacks are both worse than refusing: re-checking `exists()`
    and then calling `os.replace` reopens the very window the check closes, and
    reserving the name with `O_CREAT|O_EXCL` publishes a visible ZERO-BYTE file
    at the destination that survives an interrupted process.
    """
    try:
        if overwrite:
            try:
                os.replace(tmp, path)
            except OSError as exc:
                raise SidecarError(f"cannot publish {path}: {exc}")
            return
        try:
            os.link(tmp, path)
        except FileExistsError:
            raise SidecarError(
                f"{path} appeared while it was being written; refusing to "
                "overwrite without --overwrite")
        except (OSError, AttributeError, NotImplementedError) as exc:
            raise SidecarError(
                f"cannot publish {path} safely: this filesystem does not "
                f"support the atomic create-if-absent link used to guarantee "
                f"nothing is clobbered ({exc}). Re-run with --overwrite if "
                "replacing whatever is there is genuinely intended.")
    finally:
        # Publication IS the commit point. Failing to remove the staging link
        # afterwards must not turn a committed, complete artifact into a reported
        # failure.
        try:
            if tmp.exists():
                tmp.unlink(missing_ok=True)
        except OSError:
            pass


def stage_all(items: list) -> list:
    """Stage every (destination, bytes) pair, or none of them.

    One cleanup owner for the whole sequence: if the SECOND staging fails, the
    first is already on disk as a `.part` and nothing downstream would ever see
    it, because publication is never reached.
    """
    staged: list = []
    try:
        for destination, text in items:
            staged.append((stage_atomically(destination, text), destination))
    except BaseException:
        for tmp, _ in staged:
            try:
                tmp.unlink(missing_ok=True)
            except OSError:
                pass
        raise
    return staged


def publish_all(staged: list, overwrite: bool) -> None:
    """Publish staged (temp, destination) pairs in order, in ONE transaction.

    The LAST pair is the accepted artifact — every auxiliary output is published
    before it — so a failure anywhere leaves no accepted artifact behind. Any
    temporary that never lands is removed rather than left in the output
    directory for someone to mistake for a result.
    """
    remaining = list(staged)
    try:
        while remaining:
            tmp, destination = remaining[0]
            publish_staged(tmp, destination, overwrite)
            remaining.pop(0)
    finally:
        for tmp, _ in remaining:
            tmp.unlink(missing_ok=True)


def preflight_output(path: Path, overwrite: bool, repo_root: Path,
                     label: str) -> None:
    """Refuse a destination BEFORE any artifact is published.

    Every deterministic reason a write would fail is checked up front, so a run
    that is going to exit 1 does so while the working tree is still untouched.
    Without this the primary artifact is published and a later report failure
    leaves an accepted file behind an exit-1 invocation.
    """
    ex.ensure_safe_output_dir(path.parent, repo_root)
    # The destination DIRECTORY too: a missing one is a deterministic failure
    # that was otherwise only discovered by mkstemp, after the primary artifact
    # had already been published and as a raw FileNotFoundError.
    if not path.parent.is_dir():
        raise SidecarError(
            f"{label} directory {path.parent} does not exist (or is not a "
            "directory); create it before generating")
    if path.exists() and not overwrite:
        raise SidecarError(
            f"{label} {path} already exists; refusing to overwrite without "
            "--overwrite")


def reject_aliased_outputs(primary: Path, report: Path | None) -> None:
    """`--output X --report X` would publish the artifact and then replace it.

    Resolved, not string-compared: `out/m.json` and `out/./m.json` are the same
    file, and the second write would silently destroy the first with exit 0.
    """
    if report is None:
        return
    if primary.resolve() == report.resolve():
        raise SidecarError(
            "--output and --report resolve to the same path "
            f"({primary}); the report would overwrite the artifact")


# --------------------------------------------------------------------------- #
# Driver
# --------------------------------------------------------------------------- #

def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Generate a <stem>.names.json sidecar from an ONNX "
                    "artifact's own class metadata, cross-checked against the "
                    "TensorRT build evidence.")
    parser.add_argument("--onnx", required=True,
                        help="the exact .onnx whose `names` metadata is the "
                             "class-name authority")
    parser.add_argument("--engine-build-report", required=True,
                        help="the Slice-3 engine build report (JSON)")
    parser.add_argument("--output", required=True,
                        help="destination <stem>.names.json, outside the repository")
    parser.add_argument("--engine",
                        help="optional: the built .engine, hashed and compared "
                             "against the build report entry")
    parser.add_argument("--expected-onnx-sha256",
                        help="optional: refuse unless the ONNX hashes to this")
    parser.add_argument("--expect-class-count", type=int,
                        help="optional: refuse unless the artifact declares "
                             "exactly this many classes")
    parser.add_argument("--expect-class-name", action="append", default=[],
                        metavar="NAME",
                        help="optional, repeatable and ORDERED: refuse unless "
                             "the artifact's class names are exactly these")
    parser.add_argument("--report",
                        help="optional: write a generation report here")
    parser.add_argument("--overwrite", action="store_true",
                        help="replace an existing output file (still atomic)")
    parser.add_argument("--skip-structural-check", action="store_true",
                        help="skip onnx.checker.check_model (it is slow on "
                             "large graphs); shape and metadata rules still run")
    return parser


def main(argv=None) -> int:
    args = build_parser().parse_args(argv)
    repo_root = Path(__file__).resolve().parent.parent

    onnx_path = Path(args.onnx)
    output = Path(args.output)
    report_path_arg = Path(args.report) if args.report else None
    if not onnx_path.is_file():
        raise SidecarError(f"{onnx_path} does not exist")

    # Containment and destination checks FIRST: never produce an artifact where
    # warm-up can find it, and never start work that is already going to fail.
    reject_aliased_outputs(output, report_path_arg)
    preflight_output(output, args.overwrite, repo_root, "output")
    if report_path_arg is not None:
        preflight_output(report_path_arg, args.overwrite, repo_root, "report")

    # 1. Hash the ONNX and, if an expectation was supplied, hold it to that.
    onnx_sha = ex.sha256_file(onnx_path)
    if args.expected_onnx_sha256 and args.expected_onnx_sha256 != onnx_sha:
        raise SidecarError(
            f"{onnx_path.name}: SHA-256 mismatch — expected "
            f"{args.expected_onnx_sha256}, measured {onnx_sha}")

    # 2. Read the artifact's own metadata.
    if not args.skip_structural_check:
        try:
            ex.check_graph_validity(onnx_path)
        except ex.ExportError as exc:
            raise SidecarError(str(exc))
    # A malformed `names`/`args` literal reaches inspect_onnx as a raw ValueError
    # or TypeError; converted here so the CLI prints a redacted refusal rather
    # than a traceback.
    try:
        facts = ex.inspect_onnx(onnx_path)
    except ex.ExportError:
        raise
    except Exception as exc:
        raise SidecarError(
            f"{onnx_path.name}: ONNX metadata is unreadable: "
            f"{type(exc).__name__}: {exc}")
    names = check_onnx_class_metadata(facts, onnx_path.name)
    check_onnx_shape(facts, len(names), onnx_path.name)

    # 3. Caller-stated expectations (never defaults — see the module docstring).
    if args.expect_class_count is not None and args.expect_class_count != len(names):
        raise SidecarError(
            f"{onnx_path.name}: expected {args.expect_class_count} classes, "
            f"artifact declares {len(names)} ({names})")
    if args.expect_class_name and args.expect_class_name != names:
        raise SidecarError(
            f"{onnx_path.name}: expected class names {args.expect_class_name}, "
            f"artifact declares {names}")

    # 4. The TensorRT half of the evidence.
    report_path = Path(args.engine_build_report)
    entries = load_engine_report(report_path)
    entry = select_engine_entry(entries, onnx_sha, onnx_path.name)
    check_engine_entry(entry, len(names), facts)

    engine_sha = None
    if args.engine:
        engine_path = Path(args.engine)
        if not engine_path.is_file():
            raise SidecarError(f"{engine_path} does not exist")
        engine_sha = ex.sha256_file(engine_path)
        if engine_sha != entry.get("engine_sha256"):
            raise SidecarError(
                f"{engine_path.name}: SHA-256 {engine_sha} does not match the "
                f"build report's {entry.get('engine_sha256')}; the report "
                "describes a different engine")

    # 4b. The sidecar must be NAMED for the engine it describes.
    #
    # TrtEngine resolves `<engine-stem>.names.json`, so the filename is what
    # binds this class vocabulary to a plan. Publishing float-small's names as
    # `float-big.names.json` keeps the class COUNT valid — every count check
    # downstream still passes — while the class MEANING is wrong, which AGENTS.md
    # records as making readings silently wrong rather than failing. Nothing at
    # runtime can detect it, so it is refused here.
    engine_stem = stem_of_engine(str(entry.get("engine_filename") or ""))
    if not output.name.endswith(".names.json"):
        raise SidecarError(
            f"output {output.name} must be named <stem>.names.json")
    output_stem = output.name[: -len(".names.json")]
    if output_stem != engine_stem:
        raise SidecarError(
            f"output stem {output_stem!r} does not match the engine this "
            f"sidecar describes ({entry.get('engine_filename')!r}, stem "
            f"{engine_stem!r}). A sidecar is bound to its engine by filename; "
            "a mismatched pair keeps a valid class count while silently "
            "changing what the classes mean.")

    # 5. Serialize, then prove the bytes re-parse to the same ordered list
    #    BEFORE anything is published.
    text = serialize_sidecar(names)
    reparsed = json.loads(text)
    if reparsed != names:
        raise SidecarError("internal: serialized sidecar does not round-trip")
    if not isinstance(reparsed, list) or not all(isinstance(n, str) for n in reparsed):
        raise SidecarError("internal: serialized sidecar is not an array of strings")

    # Hashed from the BYTES about to be written, not by re-reading the published
    # file: that keeps every fallible step ahead of publication.
    sidecar_sha = hashlib.sha256(text.encode("utf-8")).hexdigest()

    result = {
        "tool": "gen_names_sidecar.py",
        "onnx": onnx_path.name,
        "onnx_sha256": onnx_sha,
        "onnx_opset": facts.get("actual_onnx_opset"),
        "onnx_output_name": facts.get("output_name"),
        "onnx_output_shape": facts.get("output_shape"),
        "class_names": names,
        "class_count": len(names),
        "class_ids": facts.get("class_ids"),
        "class_name_authority": "onnx_metadata_names",
        "engine_filename": entry.get("engine_filename"),
        "engine_sha256_from_report": entry.get("engine_sha256"),
        "engine_sha256_measured": engine_sha,
        "engine_output_shape_from_report": entry.get("output_shape"),
        "engine_decoded_class_count": int(entry["output_shape"][1]) - BOX_CHANNELS,
        "engine_build_report": report_path.name,
        "sidecar": output.name,
        "sidecar_sha256": sidecar_sha,
        "sidecar_bytes": len(text.encode("utf-8")),
        "sidecar_content": text.rstrip("\n"),
    }
    # Stage EVERYTHING first, then publish, with the sidecar LAST. The accepted
    # artifact is therefore the final thing to appear: no fallible step remains
    # once it exists, so an invocation that exits 1 never leaves one behind.
    to_stage = []
    if report_path_arg is not None:
        to_stage.append((report_path_arg,
                         json.dumps(result, indent=2, sort_keys=True) + "\n"))
    to_stage.append((output, text))                          # accepted LAST
    try:
        publish_all(stage_all(to_stage), args.overwrite)
    except OSError as exc:
        raise SidecarError(f"cannot write the requested outputs: {exc}")

    print(json.dumps(result, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (SidecarError, ex.ExportError) as error:
        print(f"gen_names_sidecar: {ex.redact(str(error))}", file=sys.stderr)
        sys.exit(1)
