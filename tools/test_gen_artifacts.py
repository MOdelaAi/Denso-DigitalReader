#!/usr/bin/env python3
"""Tests for `tools/gen_names_sidecar.py` and `tools/gen_model_manifest.py`.

Outside the CMake graph, exactly like the tools they cover and exactly like
`test_export_float_onnx.py`: Slice 4 adds no CTest registration, so the C++ suite
total is unchanged. Run them with an interpreter that has `onnx` installed:

    python tools/test_gen_artifacts.py            # from the repo root

Every case is SELF-CONTAINED. The fixtures are synthetic ONNX graphs, synthetic
`.engine` blobs and a synthetic engine build report, all built in a temporary
directory — so the suite runs on any machine, with no Jetson, no GPU, and no
access to the real Float artifact chain. That is the point: the real chain lives
in an isolated non-repository workspace that another checkout will not have, and
a test that cannot run is not a test.

The synthetic-ONNX builder is imported from `test_export_float_onnx.py` rather
than copied. One definition of "what a fixture graph looks like" — two would
drift, and a fixture that drifts silently stops exercising the rule it was
written for.
"""

from __future__ import annotations

import importlib.util
import io
import json
import os
import sys
import tempfile
import unittest
from contextlib import redirect_stdout, redirect_stderr
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
TOOLS = REPO_ROOT / "tools"

# No __pycache__: `tools/` is a tracked source dir and a stray bytecode directory
# leaves the tree dirty, which tools/build_package.sh refuses to package.
sys.dont_write_bytecode = True


def load(module_name: str):
    spec = importlib.util.spec_from_file_location(
        module_name, TOOLS / f"{module_name}.py")
    module = importlib.util.module_from_spec(spec)
    sys.modules[module_name] = module
    spec.loader.exec_module(module)
    return module


sidecar_tool = load("gen_names_sidecar")
manifest_tool = load("gen_model_manifest")
export_tests = load("test_export_float_onnx")
make_onnx = export_tests.make_onnx

SHA_ZERO = "0" * 64


# The three exception types the CLI wrappers actually catch. Anything else
# escapes `main()` as a traceback, so it is NOT a fail-closed refusal even though
# it does exit non-zero.
CONTROLLED = (sidecar_tool.SidecarError, manifest_tool.ManifestError,
              sidecar_tool.ex.ExportError)

UNCONTROLLED = "UNCONTROLLED "


def run(tool, argv) -> tuple[int, str, str]:
    """Invoke a tool's main() in-process, capturing its streams.

    Refusals raise rather than return, so they are converted to the exit code the
    CLI would produce — the tests assert on the same two outcomes an operator
    sees.

    A RAW exception is reported distinctly. Catching every `Exception` here and
    calling the result "refused" is how an IndexError from an unguarded
    `.split()[0]` passes for a considered refusal: both exit 1, but only one of
    them prints a redacted diagnostic instead of a traceback, and only one of
    them proves the guard clause that was written for the case is reachable.
    """
    out, err = io.StringIO(), io.StringIO()
    try:
        with redirect_stdout(out), redirect_stderr(err):
            code = tool.main([str(a) for a in argv])
    except CONTROLLED as exc:                      # the fail-closed refusal path
        return 1, out.getvalue(), f"{err.getvalue()}{exc}"
    except Exception as exc:                       # a traceback, not a refusal
        return 1, out.getvalue(), (f"{err.getvalue()}{UNCONTROLLED}"
                                   f"{type(exc).__name__}: {exc}")
    return code, out.getvalue(), err.getvalue()


def sha256_text(text: str) -> str:
    import hashlib
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


class Base(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        try:
            import onnx  # noqa: F401
        except ImportError:
            raise unittest.SkipTest("onnx is not installed in this interpreter")
        # A directory OUTSIDE the repository: both tools refuse to write into the
        # working tree, so an in-repo temp dir would fail every positive case.
        cls.tmp = Path(tempfile.mkdtemp(prefix="gen-artifact-tests-"))

    @classmethod
    def tearDownClass(cls):
        import shutil
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def work(self, name: str) -> Path:
        d = self.tmp / name
        d.mkdir(parents=True, exist_ok=True)
        return d

    # -- fixture builders --------------------------------------------------- #

    def fixture_onnx(self, d: Path, stem="m", **kwargs) -> Path:
        return make_onnx(d / f"{stem}.onnx", **kwargs)

    def fixture_engine(self, d: Path, stem="m", body=b"ENGINE-BYTES") -> Path:
        path = d / f"{stem}.engine"
        path.write_bytes(body)
        return path

    def fixture_report(self, d: Path, *, onnx_path: Path, engine_path: Path,
                       output_shape=(1, 5, 8400), exit_code=0,
                       engine_sha=None, output_name="output0",
                       output_dtype="FP32 (fp32:CHW)", name="report.json") -> Path:
        import hashlib
        onnx_sha = hashlib.sha256(onnx_path.read_bytes()).hexdigest()
        engine_sha = engine_sha or hashlib.sha256(
            engine_path.read_bytes()).hexdigest()
        doc = {
            "engines": [{
                "source_onnx_filename": onnx_path.name,
                "source_onnx_sha256": onnx_sha,
                "engine_filename": engine_path.name,
                "engine_sha256": engine_sha,
                "build_exit_code": exit_code,
                "output_name": output_name,
                "output_shape": list(output_shape),
                "output_dtype": output_dtype,
                "exact_trtexec_command":
                    f"/usr/src/tensorrt/bin/trtexec --onnx={onnx_path.name} "
                    f"--saveEngine={engine_path.name} --fp16",
                "l4t_version": "R36.5.0 (nvidia-l4t-core 36.5.0)",
                "jetpack_version": "6.2 (inferred from L4T R36.5.0; the "
                                   "nvidia-jetpack meta-package is NOT installed)",
            }],
        }
        path = d / name
        path.write_text(json.dumps(doc, indent=2), encoding="utf-8")
        return path


# =========================================================================== #
# gen_names_sidecar.py
# =========================================================================== #

class SidecarTests(Base):

    def generate(self, d: Path, *extra, onnx=None, report=None, out=None):
        onnx = onnx or self.fixture_onnx(d)
        engine = self.fixture_engine(d)
        report = report or self.fixture_report(d, onnx_path=onnx,
                                               engine_path=engine)
        out = out or (d / "m.names.json")
        code, stdout, stderr = run(sidecar_tool, [
            "--onnx", onnx, "--engine-build-report", report,
            "--output", out, "--skip-structural-check", *extra])
        return code, stdout, stderr, out

    def test_valid_generation_emits_a_bare_array(self):
        d = self.work("valid")
        code, _, err, out = self.generate(d)
        self.assertEqual(code, 0, err)
        self.assertEqual(out.read_text(encoding="utf-8"), '["Small"]')

    def test_class_names_come_from_the_artifact_not_the_caller(self):
        """The tool has no built-in vocabulary: rename the class, the file follows."""
        d = self.work("names-follow-artifact")
        onnx = self.fixture_onnx(d, names={0: "Renamed"})
        code, _, err, out = self.generate(d, onnx=onnx)
        self.assertEqual(code, 0, err)
        self.assertEqual(out.read_text(encoding="utf-8"), '["Renamed"]')

    def test_multi_class_model_is_supported(self):
        """Nothing here is Float-specific: three classes -> a [1,7,N] head."""
        d = self.work("multiclass")
        onnx = self.fixture_onnx(d, names={0: "a", 1: "b", 2: "c"},
                                 output_shape=(1, 7, 8400))
        engine = self.fixture_engine(d)
        report = self.fixture_report(d, onnx_path=onnx, engine_path=engine,
                                     output_shape=(1, 7, 8400))
        code, _, err, out = self.generate(d, onnx=onnx, report=report)
        self.assertEqual(code, 0, err)
        self.assertEqual(out.read_text(encoding="utf-8"), '["a","b","c"]')

    def test_reproduces_the_committed_digitv3_sidecar_byte_for_byte(self):
        """The approved sidecar hash is an input to packaging approval.

        A generator that could not reproduce the already-approved artifact would
        be describing a different file than the one in production.
        """
        committed = REPO_ROOT / "models" / "digitv3.names.json"
        if not committed.is_file():
            self.skipTest("models/digitv3.names.json is not provisioned here")
        names = json.loads(committed.read_text(encoding="utf-8"))
        self.assertEqual(
            sidecar_tool.serialize_sidecar(names).encode("utf-8"),
            committed.read_bytes())

    def test_onnx_hash_mismatch_is_refused(self):
        d = self.work("hash")
        code, _, err, out = self.generate(d, "--expected-onnx-sha256", SHA_ZERO)
        self.assertEqual(code, 1)
        self.assertIn("SHA-256 mismatch", err)
        self.assertFalse(out.exists())

    def test_missing_names_metadata_is_refused(self):
        d = self.work("nonames")
        onnx = self.fixture_onnx(d, omit_names=True)
        code, _, err, out = self.generate(d, onnx=onnx)
        self.assertEqual(code, 1)
        self.assertIn("no `names` entry", err)
        self.assertFalse(out.exists())

    def test_non_contiguous_class_ids_are_refused(self):
        d = self.work("ids")
        onnx = self.fixture_onnx(d, names={0: "a", 2: "c"},
                                 output_shape=(1, 6, 8400))
        code, _, err, _ = self.generate(d, onnx=onnx)
        self.assertEqual(code, 1)
        self.assertIn("contiguous", err)

    def test_blank_class_name_is_refused(self):
        d = self.work("blank")
        onnx = self.fixture_onnx(d, names={0: "   "})
        code, _, err, _ = self.generate(d, onnx=onnx)
        self.assertEqual(code, 1)
        self.assertIn("blank", err)

    def test_engine_class_count_disagreement_is_refused(self):
        """Two classes cannot live behind a [1,5,8400] head."""
        d = self.work("count")
        onnx = self.fixture_onnx(d, names={0: "a", 1: "b"})
        code, _, err, _ = self.generate(d, onnx=onnx)
        self.assertEqual(code, 1)
        self.assertIn("does not match the class metadata", err)

    def test_report_output_shape_disagreement_is_refused(self):
        d = self.work("reportshape")
        onnx = self.fixture_onnx(d)
        engine = self.fixture_engine(d)
        report = self.fixture_report(d, onnx_path=onnx, engine_path=engine,
                                     output_shape=(1, 9, 8400))
        code, _, err, _ = self.generate(d, onnx=onnx, report=report)
        self.assertEqual(code, 1)
        self.assertIn("records output shape", err)

    def test_end_to_end_layout_is_refused(self):
        d = self.work("e2e")
        onnx = self.fixture_onnx(d, output_shape=(1, 300, 6))
        code, _, err, _ = self.generate(d, onnx=onnx)
        self.assertEqual(code, 1)
        self.assertIn("END-TO-END", err)

    def test_failed_build_cannot_back_a_sidecar(self):
        d = self.work("failedbuild")
        onnx = self.fixture_onnx(d)
        engine = self.fixture_engine(d)
        report = self.fixture_report(d, onnx_path=onnx, engine_path=engine,
                                     exit_code=1)
        code, _, err, _ = self.generate(d, onnx=onnx, report=report)
        self.assertEqual(code, 1)
        self.assertIn("build_exit_code", err)

    def test_report_for_a_different_artifact_chain_is_refused(self):
        d = self.work("otherchain")
        onnx = self.fixture_onnx(d)
        other = self.fixture_onnx(d, stem="other", names={0: "Other"})
        engine = self.fixture_engine(d)
        report = self.fixture_report(d, onnx_path=other, engine_path=engine)
        code, _, err, _ = self.generate(d, onnx=onnx, report=report)
        self.assertEqual(code, 1)
        self.assertIn("no entry whose source_onnx_sha256", err)

    def test_engine_hash_disagreeing_with_the_report_is_refused(self):
        d = self.work("enginehash")
        onnx = self.fixture_onnx(d)
        engine = self.fixture_engine(d)
        report = self.fixture_report(d, onnx_path=onnx, engine_path=engine,
                                     engine_sha=SHA_ZERO)
        code, _, err, _ = self.generate(d, onnx=onnx, report=report,
                                        *["--engine", engine])
        self.assertEqual(code, 1)
        self.assertIn("does not match the build report", err)

    def test_malformed_evidence_is_refused(self):
        d = self.work("badreport")
        bad = d / "bad.json"
        bad.write_text('{"engines": "not-a-list"}', encoding="utf-8")
        code, _, err, _ = self.generate(d, report=bad)
        self.assertEqual(code, 1)
        self.assertIn("malformed", err)

    def test_caller_expectations_are_enforced(self):
        d = self.work("expect")
        code, _, err, _ = self.generate(d, "--expect-class-name", "Big")
        self.assertEqual(code, 1)
        self.assertIn("expected class names", err)

        d2 = self.work("expect2")
        code, _, err, _ = self.generate(d2, "--expect-class-count", "3")
        self.assertEqual(code, 1)
        self.assertIn("expected 3 classes", err)

    def test_existing_output_is_not_overwritten_without_the_flag(self):
        d = self.work("exists")
        out = d / "m.names.json"
        out.write_text("PRE-EXISTING", encoding="utf-8")
        code, _, err, _ = self.generate(d, out=out)
        self.assertEqual(code, 1)
        self.assertIn("refusing to overwrite", err)
        self.assertEqual(out.read_text(encoding="utf-8"), "PRE-EXISTING")

    def test_explicit_overwrite_publishes_atomically(self):
        d = self.work("overwrite")
        out = d / "m.names.json"
        out.write_text("PRE-EXISTING", encoding="utf-8")
        code, _, err, _ = self.generate(d, "--overwrite", out=out)
        self.assertEqual(code, 0, err)
        self.assertEqual(out.read_text(encoding="utf-8"), '["Small"]')
        self.assertEqual(list(d.glob("*.part")), [])

    def test_a_refusal_publishes_nothing(self):
        d = self.work("nopartial")
        onnx = self.fixture_onnx(d, names={0: "a", 1: "b"})
        out = d / "m.names.json"
        code, _, _, _ = self.generate(d, onnx=onnx, out=out)
        self.assertEqual(code, 1)
        self.assertFalse(out.exists())
        self.assertEqual(list(d.glob("*.part")), [])

    def test_generation_is_deterministic(self):
        d = self.work("determinism")
        onnx = self.fixture_onnx(d)
        engine = self.fixture_engine(d)
        report = self.fixture_report(d, onnx_path=onnx, engine_path=engine)
        blobs = []
        # Separate DIRECTORIES, identical basename: the sidecar filename is bound
        # to the engine stem, so two runs cannot differ by output name.
        for index in range(2):
            run_dir = d / f"run{index}"
            run_dir.mkdir(exist_ok=True)
            out = run_dir / "m.names.json"
            code, _, err, _ = self.generate(d, onnx=onnx, report=report, out=out)
            self.assertEqual(code, 0, err)
            blobs.append(out.read_bytes())
        self.assertEqual(blobs[0], blobs[1])

    def test_refuses_to_write_inside_the_repository(self):
        d = self.work("containment")
        code, _, err, _ = self.generate(
            d, out=REPO_ROOT / "models" / "should-never-appear.names.json")
        self.assertEqual(code, 1)
        self.assertTrue("repository" in err or "models" in err, err)
        self.assertFalse((REPO_ROOT / "models" /
                          "should-never-appear.names.json").exists())


# =========================================================================== #
# gen_model_manifest.py
# =========================================================================== #

class ManifestTests(Base):

    def chain(self, d: Path, *, names=None, output_shape=(1, 5, 8400)):
        """A complete synthetic artifact chain: onnx + engine + sidecar + report."""
        onnx = make_onnx(d / "m.onnx", names=names or {0: "Small"},
                         output_shape=output_shape)
        engine = self.fixture_engine(d)
        sidecar = d / "m.names.json"
        ordered = [(names or {0: "Small"})[k]
                   for k in sorted(names or {0: "Small"}, key=int)]
        sidecar.write_text(json.dumps(ordered, separators=(",", ":")),
                           encoding="utf-8")
        report = self.fixture_report(d, onnx_path=onnx, engine_path=engine,
                                     output_shape=output_shape)
        return onnx, engine, sidecar, report

    def descriptor(self, d: Path, body: dict, name="desc.json") -> Path:
        path = d / name
        path.write_text(json.dumps(body, indent=2), encoding="utf-8")
        return path

    def base_body(self, d: Path, onnx, engine, sidecar, report) -> dict:
        return {
            "name": "m", "canonical_id": "m", "family": "test_family",
            "task": "detect", "input_size": 640,
            "onnxruntime": {"model_path": str(onnx)},
            "tensorrt": {"engine_path": str(engine), "sidecar_path": str(sidecar),
                         "built_for": {"trt": "10.3", "cuda": "12.6", "sm": "87"}},
            "provenance_from": [str(report)],
            "provenance": {
                "source_pt_sha256": SHA_ZERO,
                "onnx_sha256": None,          # filled by the caller
                "export_ultralytics": "8.4.33",
            },
            "provenance_evidence": {
                "source_pt_sha256": "synthetic fixture",
                "onnx_sha256": "synthetic fixture",
                "export_ultralytics": "synthetic fixture",
            },
        }

    def full_body(self, d: Path) -> tuple[dict, Path]:
        import hashlib
        onnx, engine, sidecar, report = self.chain(d)
        body = self.base_body(d, onnx, engine, sidecar, report)
        body["provenance"]["onnx_sha256"] = hashlib.sha256(
            onnx.read_bytes()).hexdigest()
        return body, d / "manifest.json"

    def generate(self, d: Path, body: dict, out: Path, *extra):
        path = self.descriptor(d, body)
        return run(manifest_tool, [
            "--generation", path, "--installed-utc", "2026-07-23T00:00:00Z",
            "--output", out, "--skip-structural-check", *extra])

    # -- positive ----------------------------------------------------------- #

    def test_both_runtime_blocks(self):
        d = self.work("mf-both")
        body, out = self.full_body(d)
        code, _, err = self.generate(d, body, out)
        self.assertEqual(code, 0, err)
        g = json.loads(out.read_text(encoding="utf-8"))["generations"][0]
        self.assertEqual(set(g["runtime"]), {"onnxruntime", "tensorrt"})
        self.assertEqual(g["class_names"], ["Small"])
        self.assertEqual(g["class_count"], 1)
        self.assertEqual(g["runtime"]["onnxruntime"]["class_metadata_source"],
                         "onnx_metadata_names")
        self.assertEqual(g["runtime"]["tensorrt"]["class_metadata_source"],
                         "names_sidecar")

    def test_onnx_only(self):
        d = self.work("mf-ort")
        body, out = self.full_body(d)
        del body["tensorrt"]
        code, _, err = self.generate(d, body, out)
        self.assertEqual(code, 0, err)
        g = json.loads(out.read_text(encoding="utf-8"))["generations"][0]
        self.assertEqual(set(g["runtime"]), {"onnxruntime"})

    def test_tensorrt_only(self):
        d = self.work("mf-trt")
        body, out = self.full_body(d)
        del body["onnxruntime"]
        code, _, err = self.generate(d, body, out)
        self.assertEqual(code, 0, err)
        g = json.loads(out.read_text(encoding="utf-8"))["generations"][0]
        self.assertEqual(set(g["runtime"]), {"tensorrt"})

    def test_built_for_is_tensorrt_local(self):
        d = self.work("mf-bf")
        body, out = self.full_body(d)
        code, _, err = self.generate(d, body, out)
        self.assertEqual(code, 0, err)
        g = json.loads(out.read_text(encoding="utf-8"))["generations"][0]
        self.assertNotIn("built_for", g)
        self.assertIn("built_for", g["runtime"]["tensorrt"])

    def test_hashes_are_measured_not_copied(self):
        """A stated hash is checked; the EMITTED hash always comes from the file."""
        import hashlib
        d = self.work("mf-measured")
        body, out = self.full_body(d)
        engine_path = Path(body["tensorrt"]["engine_path"])
        code, _, err = self.generate(d, body, out)
        self.assertEqual(code, 0, err)
        g = json.loads(out.read_text(encoding="utf-8"))["generations"][0]
        self.assertEqual(g["runtime"]["tensorrt"]["engine_sha256"],
                         hashlib.sha256(engine_path.read_bytes()).hexdigest())

    # -- negative ----------------------------------------------------------- #

    def test_no_runtime_block_is_refused(self):
        d = self.work("mf-none")
        body, out = self.full_body(d)
        del body["onnxruntime"]
        del body["tensorrt"]
        code, _, err = self.generate(d, body, out)
        self.assertEqual(code, 1)
        self.assertIn("at least one runtime block", err)
        self.assertFalse(out.exists())

    def test_unknown_descriptor_key_is_refused(self):
        d = self.work("mf-unknown")
        body, out = self.full_body(d)
        body["openvino"] = {"model_path": "x"}
        code, _, err = self.generate(d, body, out)
        self.assertEqual(code, 1)
        self.assertIn("unknown descriptor key", err)

    def test_root_built_for_cannot_be_requested(self):
        d = self.work("mf-rootbf")
        body, out = self.full_body(d)
        body["built_for"] = {"trt": "10.3", "cuda": "12.6", "sm": "87"}
        code, _, err = self.generate(d, body, out)
        self.assertEqual(code, 1)
        self.assertIn("unknown descriptor key", err)

    def test_allowed_modes_cannot_be_smuggled_in(self):
        d = self.work("mf-am")
        body, out = self.full_body(d)
        body["allowed_modes"] = ["ball_leveler"]
        code, _, err = self.generate(d, body, out)
        self.assertEqual(code, 1)
        self.assertIn("unknown descriptor key", err)

    def test_artifact_hash_mismatch_is_refused(self):
        d = self.work("mf-sha")
        body, out = self.full_body(d)
        body["onnxruntime"]["expected_sha256"] = SHA_ZERO
        code, _, err = self.generate(d, body, out)
        self.assertEqual(code, 1)
        self.assertIn("SHA-256 mismatch", err)
        self.assertFalse(out.exists())

    def test_onnx_sidecar_class_disagreement_is_refused(self):
        d = self.work("mf-classes")
        body, out = self.full_body(d)
        Path(body["tensorrt"]["sidecar_path"]).write_text(
            '["Small","Extra"]', encoding="utf-8")
        code, _, err = self.generate(d, body, out)
        self.assertEqual(code, 1)
        self.assertIn("one of them is stale", err)

    def test_duplicate_canonical_id_is_refused(self):
        d = self.work("mf-dup")
        body, out = self.full_body(d)
        first = self.descriptor(d, body, "a.json")
        second = self.descriptor(d, body, "b.json")
        code, _, err = run(manifest_tool, [
            "--generation", first, "--generation", second,
            "--installed-utc", "2026-07-23T00:00:00Z", "--output", out,
            "--skip-structural-check"])
        self.assertEqual(code, 1)
        self.assertIn("duplicate", err)
        self.assertFalse(out.exists())

    def test_uncited_provenance_is_refused(self):
        d = self.work("mf-cite")
        body, out = self.full_body(d)
        body["provenance"]["precision"] = "int8"
        code, _, err = self.generate(d, body, out)
        self.assertEqual(code, 1)
        self.assertIn("provenance_evidence citation", err)

    def test_provenance_disagreeing_with_the_artifact_is_refused(self):
        d = self.work("mf-provsha")
        body, out = self.full_body(d)
        body["provenance"]["onnx_sha256"] = "1" * 64
        code, _, err = self.generate(d, body, out)
        self.assertEqual(code, 1)
        self.assertIn("describes a different artifact", err)

    def test_non_schema_provenance_key_is_refused(self):
        d = self.work("mf-provkey")
        body, out = self.full_body(d)
        body["provenance"]["trained_by"] = "someone"
        body["provenance_evidence"]["trained_by"] = "cited, but not a schema field"
        code, _, err = self.generate(d, body, out)
        self.assertEqual(code, 1)
        self.assertIn("not a schema-2 provenance field", err)

    def test_missing_required_provenance_is_refused(self):
        d = self.work("mf-provmissing")
        body, out = self.full_body(d)
        del body["provenance"]["export_ultralytics"]
        del body["provenance_evidence"]["export_ultralytics"]
        code, _, err = self.generate(d, body, out)
        self.assertEqual(code, 1)
        self.assertIn("missing required field", err)

    def test_input_size_disagreeing_with_the_onnx_is_refused(self):
        d = self.work("mf-size")
        body, out = self.full_body(d)
        body["input_size"] = 512
        code, _, err = self.generate(d, body, out)
        self.assertEqual(code, 1)
        self.assertIn("does not match the ONNX input shape", err)

    def test_engine_sidecar_stem_mismatch_is_refused(self):
        d = self.work("mf-stem")
        body, out = self.full_body(d)
        other = Path(body["tensorrt"]["sidecar_path"]).parent / "other.names.json"
        other.write_text('["Small"]', encoding="utf-8")
        body["tensorrt"]["sidecar_path"] = str(other)
        code, _, err = self.generate(d, body, out)
        self.assertEqual(code, 1)
        self.assertIn("stem mismatch", err)

    def test_incomplete_built_for_is_refused(self):
        d = self.work("mf-bfpartial")
        body, out = self.full_body(d)
        body["tensorrt"]["built_for"] = {"trt": "10.3", "cuda": "12.6"}
        code, _, err = self.generate(d, body, out)
        self.assertEqual(code, 1)
        self.assertIn("built_for", err)

    def test_bad_state_is_refused(self):
        d = self.work("mf-state")
        body, out = self.full_body(d)
        body["state"] = "staged"
        code, _, err = self.generate(d, body, out)
        self.assertEqual(code, 1)
        self.assertIn("state must be", err)

    # -- output discipline -------------------------------------------------- #

    def test_existing_output_is_not_overwritten_without_the_flag(self):
        d = self.work("mf-exists")
        body, out = self.full_body(d)
        out.write_text("PRE-EXISTING", encoding="utf-8")
        code, _, err = self.generate(d, body, out)
        self.assertEqual(code, 1)
        self.assertEqual(out.read_text(encoding="utf-8"), "PRE-EXISTING")

    def test_explicit_overwrite_publishes_atomically(self):
        d = self.work("mf-overwrite")
        body, out = self.full_body(d)
        out.write_text("PRE-EXISTING", encoding="utf-8")
        code, _, err = self.generate(d, body, out, "--overwrite")
        self.assertEqual(code, 0, err)
        self.assertEqual(json.loads(out.read_text(encoding="utf-8"))["schema"], 2)
        self.assertEqual(list(d.glob("*.part")), [])

    def test_generation_is_deterministic(self):
        d = self.work("mf-det")
        body, _ = self.full_body(d)
        blobs = []
        for index in range(2):
            out = d / f"run{index}.json"
            code, _, err = self.generate(d, body, out)
            self.assertEqual(code, 0, err)
            blobs.append(out.read_bytes())
        self.assertEqual(blobs[0], blobs[1])

    def test_the_clock_is_never_read(self):
        d = self.work("mf-noclock")
        body, out = self.full_body(d)
        path = self.descriptor(d, body)
        code, _, err = run(manifest_tool, [
            "--generation", path, "--output", out, "--skip-structural-check"])
        self.assertEqual(code, 1)
        self.assertIn("never reads the clock", err)

    def test_no_path_or_account_leaks_into_the_output(self):
        d = self.work("mf-leak")
        body, out = self.full_body(d)
        code, _, err = self.generate(d, body, out)
        self.assertEqual(code, 0, err)
        text = out.read_text(encoding="utf-8")
        self.assertNotIn(str(d), text)
        self.assertNotIn("allowed_modes", text)
        self.assertNotIn(os.path.expanduser("~"), text)
        for var in ("USERNAME", "USER", "LOGNAME"):
            value = os.environ.get(var) or ""
            if len(value) > 2:
                self.assertNotIn(value, text)

    def test_refuses_to_write_inside_the_repository(self):
        d = self.work("mf-containment")
        body, _ = self.full_body(d)
        target = REPO_ROOT / "models" / "should-never-appear.json"
        code, _, err = self.generate(d, body, target)
        self.assertEqual(code, 1)
        self.assertTrue("repository" in err or "models" in err, err)
        self.assertFalse(target.exists())


# =========================================================================== #
# Review regressions
#
# One case per defect found in the Slice-4 review. Each asserts a CONTROLLED
# refusal, not merely a non-zero exit: the whole class of defect here was code
# that failed by traceback where a considered refusal had been written and was
# unreachable.
# =========================================================================== #

class ReviewRegressions(Base):

    def refused(self, code: int, err: str, needle: str):
        """Exit 1, a redacted diagnostic, and NO traceback."""
        self.assertEqual(code, 1, err)
        self.assertNotIn(UNCONTROLLED, err)
        self.assertIn(needle, err)

    # -- fixture chain ------------------------------------------------------ #

    def chain(self, d: Path, *, names=None, output_shape=(1, 5, 8400),
              stem="m", **report_kwargs):
        names = names or {0: "Small"}
        onnx = make_onnx(d / f"{stem}.onnx", names=names, output_shape=output_shape)
        engine = self.fixture_engine(d, stem=stem)
        sidecar = d / f"{stem}.names.json"
        ordered = [names[k] for k in sorted(names, key=int)]
        sidecar.write_text(json.dumps(ordered, separators=(",", ":")),
                           encoding="utf-8")
        report = self.fixture_report(d, onnx_path=onnx, engine_path=engine,
                                     output_shape=output_shape, **report_kwargs)
        return onnx, engine, sidecar, report

    def body(self, d: Path, onnx, engine, sidecar, report) -> dict:
        import hashlib
        return {
            "name": "m", "canonical_id": "m", "family": "test_family",
            "task": "detect", "input_size": 640,
            "onnxruntime": {"model_path": str(onnx)},
            "tensorrt": {"engine_path": str(engine), "sidecar_path": str(sidecar),
                         "built_for": {"trt": "10.3", "cuda": "12.6", "sm": "87"}},
            "provenance_from": [str(report)],
            "provenance": {
                "source_pt_sha256": SHA_ZERO,
                "onnx_sha256": hashlib.sha256(onnx.read_bytes()).hexdigest(),
                "export_ultralytics": "8.4.33",
            },
            "provenance_evidence": {
                "source_pt_sha256": "synthetic fixture",
                "onnx_sha256": "synthetic fixture",
                "export_ultralytics": "synthetic fixture",
            },
        }

    def gen(self, d: Path, body: dict, out: Path, *extra, name="desc.json"):
        path = d / name
        path.write_text(json.dumps(body, indent=2), encoding="utf-8")
        return run(manifest_tool, [
            "--generation", path, "--installed-utc", "2026-07-23T00:00:00Z",
            "--output", out, "--skip-structural-check", *extra])

    def sidecar_run(self, d, onnx, report, out, *extra):
        return run(sidecar_tool, ["--onnx", onnx, "--engine-build-report", report,
                                  "--output", out, "--skip-structural-check", *extra])

    # -- F1: rules the C++ enforces that the Python mirror must too ---------- #

    def test_duplicate_class_names_are_refused(self):
        """C++ validate_common rejects them; publishing one is a global blocker.

        A duplicate-name manifest parses fine and then fails `validate_manifest`,
        which spec 3.3 makes ManifestCorrupt — a GLOBAL blocker, not a degraded
        state. The generator must never be able to produce one.
        """
        d = self.work("rg-dupclass")
        onnx, engine, sidecar, report = self.chain(
            d, names={0: "ball", 1: "ball"}, output_shape=(1, 6, 8400))
        body = self.body(d, onnx, engine, sidecar, report)
        out = d / "manifest.json"
        code, _, err = self.gen(d, body, out)
        self.refused(code, err, "duplicate class name")
        self.assertFalse(out.exists())

    def test_non_string_provenance_value_is_refused(self):
        """`"precision": {}` is non-empty to `str()` but empty to C++ `opt_str`."""
        d = self.work("rg-provtype")
        onnx, engine, sidecar, report = self.chain(d)
        body = self.body(d, onnx, engine, sidecar, report)
        body["provenance"]["precision"] = {}
        body["provenance_evidence"]["precision"] = "cited but not a string"
        out = d / "manifest.json"
        code, _, err = self.gen(d, body, out)
        self.refused(code, err, "precision")
        self.assertFalse(out.exists())

    def test_nan_provenance_cannot_produce_non_rfc_json(self):
        """`json.dumps` emits bare NaN by default; Qt's parser rejects the file."""
        d = self.work("rg-nan")
        onnx, engine, sidecar, report = self.chain(d)
        body = self.body(d, onnx, engine, sidecar, report)
        out = d / "manifest.json"
        path = d / "desc.json"
        # NaN cannot be written by json.dumps with allow_nan=False, so the
        # descriptor is authored literally — json.loads accepts it by default,
        # which is exactly how it would reach the generator in the field.
        raw = json.dumps(body, indent=2)
        raw = raw.replace('"input_size": 640', '"input_size": 640,\n  "_pad": 0')
        body["provenance"]["batch"] = "__NAN__"
        body["provenance_evidence"]["batch"] = "synthetic"
        raw = json.dumps(body, indent=2).replace('"__NAN__"', "NaN")
        path.write_text(raw, encoding="utf-8")
        code, _, err = run(manifest_tool, [
            "--generation", path, "--installed-utc", "2026-07-23T00:00:00Z",
            "--output", out, "--skip-structural-check"])
        self.assertEqual(code, 1, err)
        self.assertNotIn(UNCONTROLLED, err)
        self.assertFalse(out.exists())

    def test_input_size_beyond_the_cpp_int_range_is_refused(self):
        """C++ `whole()` uses QJsonValue::toInt; an out-of-range value fails parse.

        Deliberately TensorRT-ONLY: with an ONNX present the value is already
        caught by the input-shape cross-check, which would let this pass without
        the range rule ever existing.
        """
        d = self.work("rg-intrange")
        onnx, engine, sidecar, report = self.chain(d)
        body = self.body(d, onnx, engine, sidecar, report)
        del body["onnxruntime"]
        body["input_size"] = 2 ** 31
        out = d / "manifest.json"
        code, _, err = self.gen(d, body, out)
        self.refused(code, err, "input_size")
        self.assertFalse(out.exists())

    # -- F2: the evidence chain --------------------------------------------- #

    def test_engine_bytes_not_described_by_the_report_are_refused(self):
        """The manifest must not describe engine A with engine B's provenance."""
        d = self.work("rg-enginebytes")
        onnx, engine, sidecar, report = self.chain(d)
        body = self.body(d, onnx, engine, sidecar, report)
        # Same filename, DIFFERENT bytes than the report recorded.
        engine.write_bytes(b"DIFFERENT-ENGINE-BYTES")
        out = d / "manifest.json"
        code, _, err = self.gen(d, body, out)
        self.refused(code, err, "engine")
        self.assertFalse(out.exists())

    def test_failed_build_evidence_cannot_back_a_manifest(self):
        d = self.work("rg-failedbuild")
        onnx, engine, sidecar, report = self.chain(d, exit_code=1)
        body = self.body(d, onnx, engine, sidecar, report)
        out = d / "manifest.json"
        code, _, err = self.gen(d, body, out)
        self.refused(code, err, "build_exit_code")
        self.assertFalse(out.exists())

    def test_serializer_refuses_non_rfc_json(self):
        """`json.dumps` emits bare `NaN`/`Infinity` unless told not to.

        Python round-trips those happily; Qt's parser rejects them, so the
        appliance would see ManifestCorrupt. Asserted directly on the serializer
        because the provenance type rules already stop the only descriptor route
        to one — this is the backstop, and a backstop needs its own test.
        """
        for bad in (float("nan"), float("inf"), float("-inf")):
            with self.assertRaises(ValueError):
                manifest_tool.serialize(
                    {"schema": 2, "generations": [{"provenance": {"batch": bad}}]})

    def test_slice2_record_for_a_different_onnx_is_refused(self):
        """An ONNX IS declared, so the record must prove it describes THAT one."""
        d = self.work("rg-slice2link")
        onnx, engine, sidecar, report = self.chain(d)
        body = self.body(d, onnx, engine, sidecar, report)
        alien = d / "alien.provenance.json"
        alien.write_text(json.dumps({
            "source_pt": "other.pt", "source_pt_sha256": "b" * 64,
            "onnx": "other.onnx", "onnx_sha256": "c" * 64,
            "actual_onnx_opset": 13, "export_ultralytics": "8.4.33",
        }), encoding="utf-8")
        body["provenance_from"] = [str(report), str(alien)]
        out = d / "manifest.json"
        code, _, err = self.gen(d, body, out)
        self.refused(code, err, "different artifact")
        self.assertFalse(out.exists())

    def test_two_evidence_files_disagreeing_are_refused(self):
        """Neither record silently wins; the disagreement itself is the fault."""
        d = self.work("rg-evconflict")
        onnx, engine, sidecar, report = self.chain(d)
        body = self.body(d, onnx, engine, sidecar, report)
        import hashlib
        onnx_sha = hashlib.sha256(onnx.read_bytes()).hexdigest()
        # Same ONNX, so it is legitimately linked — but it disagrees about the
        # exporter version with a second record for the same artifact.
        first = d / "first.provenance.json"
        first.write_text(json.dumps({
            "onnx": "m.onnx", "onnx_sha256": onnx_sha,
            "export_ultralytics": "8.4.33", "source_pt_sha256": "a" * 64,
            "validation_passed": True, "validation_violations": [],
        }), encoding="utf-8")
        second = d / "second.provenance.json"
        second.write_text(json.dumps({
            "onnx": "m.onnx", "onnx_sha256": onnx_sha,
            "export_ultralytics": "8.4.21", "source_pt_sha256": "a" * 64,
            "validation_passed": True, "validation_violations": [],
        }), encoding="utf-8")
        body["provenance_from"] = [str(report), str(first), str(second)]
        del body["provenance"]["export_ultralytics"]
        del body["provenance_evidence"]["export_ultralytics"]
        out = d / "manifest.json"
        code, _, err = self.gen(d, body, out)
        self.refused(code, err, "evidence conflict")
        self.assertFalse(out.exists())

    def test_trt_only_cannot_absorb_unrelated_slice2_provenance(self):
        """With no ONNX to key on, a Slice-2 record must still prove it belongs."""
        d = self.work("rg-trtprov")
        onnx, engine, sidecar, report = self.chain(d)
        body = self.body(d, onnx, engine, sidecar, report)
        del body["onnxruntime"]
        # A provenance record describing a DIFFERENT artifact chain.
        alien = d / "alien.provenance.json"
        alien.write_text(json.dumps({
            "source_pt": "other.pt", "source_pt_sha256": "b" * 64,
            "onnx": "other.onnx", "onnx_sha256": "c" * 64,
            "actual_onnx_opset": 13, "export_ultralytics": "8.4.33",
        }), encoding="utf-8")
        body["provenance_from"] = [str(report), str(alien)]
        out = d / "manifest.json"
        code, _, err = self.gen(d, body, out)
        self.refused(code, err, "onnx")
        self.assertFalse(out.exists())

    # -- F3: publication ordering and output aliasing ----------------------- #

    def test_report_may_not_alias_the_manifest_output(self):
        d = self.work("rg-alias-mf")
        onnx, engine, sidecar, report = self.chain(d)
        body = self.body(d, onnx, engine, sidecar, report)
        out = d / "manifest.json"
        code, _, err = self.gen(d, body, out, "--report", out, "--overwrite")
        self.refused(code, err, "same path")

    def test_report_may_not_alias_the_sidecar_output(self):
        d = self.work("rg-alias-sc")
        onnx, engine, sidecar, report = self.chain(d)
        out = d / "out.names.json"
        code, _, err = self.sidecar_run(d, onnx, report, out,
                                        "--report", out, "--overwrite")
        self.refused(code, err, "same path")

    def test_an_unwritable_report_does_not_leave_a_published_manifest(self):
        """The primary artifact must not survive an invocation that exited 1."""
        d = self.work("rg-reportfail")
        onnx, engine, sidecar, report = self.chain(d)
        body = self.body(d, onnx, engine, sidecar, report)
        out = d / "manifest.json"
        blocked = d / "report.json"
        blocked.write_text("PRE-EXISTING", encoding="utf-8")  # no --overwrite
        code, _, err = self.gen(d, body, out, "--report", blocked)
        self.assertEqual(code, 1, err)
        self.assertNotIn(UNCONTROLLED, err)
        self.assertFalse(out.exists())
        self.assertEqual(blocked.read_text(encoding="utf-8"), "PRE-EXISTING")

    # -- F4: provenance may be added, never contradicted -------------------- #

    def test_descriptor_may_not_contradict_extracted_evidence(self):
        d = self.work("rg-override")
        onnx, engine, sidecar, report = self.chain(d)
        body = self.body(d, onnx, engine, sidecar, report)
        # The report says fp16; the descriptor claims int8 with a plausible note.
        body["provenance"]["precision"] = "int8"
        body["provenance_evidence"]["precision"] = "operator recollection"
        out = d / "manifest.json"
        code, _, err = self.gen(d, body, out)
        self.refused(code, err, "contradict")
        self.assertFalse(out.exists())

    # -- F5: controlled refusals, not tracebacks ---------------------------- #

    def test_missing_output_dtype_is_a_controlled_refusal(self):
        d = self.work("rg-dtype")
        onnx = self.fixture_onnx(d)
        engine = self.fixture_engine(d)
        report = self.fixture_report(d, onnx_path=onnx, engine_path=engine,
                                     output_dtype="")
        code, _, err = self.sidecar_run(d, onnx, report, d / "m.names.json")
        self.refused(code, err, "dtype")

    def test_inferred_jetpack_without_an_l4t_version_is_a_controlled_refusal(self):
        """The guard clause written for this case must be reachable."""
        d = self.work("rg-jetpack")
        onnx, engine, sidecar, report = self.chain(d)
        doc = json.loads(report.read_text(encoding="utf-8"))
        doc["engines"][0]["l4t_version"] = ""
        report.write_text(json.dumps(doc), encoding="utf-8")
        body = self.body(d, onnx, engine, sidecar, report)
        out = d / "manifest.json"
        code, _, err = self.gen(d, body, out)
        self.refused(code, err, "L4T")
        self.assertFalse(out.exists())

    def test_missing_sidecar_path_is_a_controlled_refusal(self):
        d = self.work("rg-nosidecarpath")
        onnx, engine, sidecar, report = self.chain(d)
        body = self.body(d, onnx, engine, sidecar, report)
        del body["tensorrt"]["sidecar_path"]
        out = d / "manifest.json"
        code, _, err = self.gen(d, body, out)
        self.refused(code, err, "sidecar_path")

    def test_an_empty_runtime_block_is_refused_not_silently_dropped(self):
        """A declared-but-empty block must not vanish from the manifest.

        Spec 3.2.1 rule 5: a generation with no block for the running backend is
        UNAVAILABLE there. Silently dropping a block the operator wrote makes the
        model unavailable on that platform with exit 0 and no diagnostic.
        """
        d = self.work("rg-emptyblock")
        onnx, engine, sidecar, report = self.chain(d)
        body = self.body(d, onnx, engine, sidecar, report)
        body["onnxruntime"] = {}
        out = d / "manifest.json"
        code, _, err = self.gen(d, body, out)
        self.refused(code, err, "onnxruntime")
        self.assertFalse(out.exists())

    def test_non_object_engine_entry_is_a_controlled_refusal(self):
        d = self.work("rg-badentry")
        onnx, engine, sidecar, report = self.chain(d)
        body = self.body(d, onnx, engine, sidecar, report)
        del body["onnxruntime"]                       # key on the engine hash
        report.write_text(json.dumps({"engines": ["not-an-object"]}),
                          encoding="utf-8")
        out = d / "manifest.json"
        code, _, err = self.gen(d, body, out)
        self.assertEqual(code, 1, err)
        self.assertNotIn(UNCONTROLLED, err)

    # -- F6: bool/int type confusion ---------------------------------------- #

    def test_boolean_build_exit_code_is_refused(self):
        """`False == 0` in Python: a bool must not pass for a successful build."""
        d = self.work("rg-boolexit")
        onnx = self.fixture_onnx(d)
        engine = self.fixture_engine(d)
        report = self.fixture_report(d, onnx_path=onnx, engine_path=engine,
                                     exit_code=False)
        code, _, err = self.sidecar_run(d, onnx, report, d / "m.names.json")
        self.refused(code, err, "build_exit_code")

    def test_boolean_output_shape_dimension_is_refused(self):
        """`[True,5,8400] == [1,5,8400]` in Python."""
        d = self.work("rg-boolshape")
        onnx = self.fixture_onnx(d)
        engine = self.fixture_engine(d)
        report = self.fixture_report(d, onnx_path=onnx, engine_path=engine,
                                     output_shape=(True, 5, 8400))
        code, _, err = self.sidecar_run(d, onnx, report, d / "m.names.json")
        self.refused(code, err, "output shape")

    # -- F7: the sidecar must be named for the engine it describes ---------- #

    def test_sidecar_output_stem_must_match_the_engine(self):
        """AGENTS.md's exact failure mode: a wrong sidecar reads silently wrong.

        Small's names published as `big.names.json` keeps the class COUNT valid,
        so every downstream count check still passes while the class meaning is
        wrong. Nothing at runtime can detect it.
        """
        d = self.work("rg-stem")
        onnx = self.fixture_onnx(d)                       # m.onnx -> m.engine
        engine = self.fixture_engine(d)
        report = self.fixture_report(d, onnx_path=onnx, engine_path=engine)
        wrong = d / "other.names.json"
        code, _, err = self.sidecar_run(d, onnx, report, wrong)
        self.refused(code, err, "stem")
        self.assertFalse(wrong.exists())

    def test_matching_stem_is_accepted(self):
        d = self.work("rg-stem-ok")
        onnx = self.fixture_onnx(d)
        engine = self.fixture_engine(d)
        report = self.fixture_report(d, onnx_path=onnx, engine_path=engine)
        out = d / "m.names.json"
        code, _, err = self.sidecar_run(d, onnx, report, out)
        self.assertEqual(code, 0, err)
        self.assertEqual(out.read_text(encoding="utf-8"), '["Small"]')

    # -- F8: credential detection (system paths stay: spec requires verbatim) - #

    def test_a_credential_in_a_recorded_command_is_refused(self):
        d = self.work("rg-cred")
        onnx, engine, sidecar, report = self.chain(d)
        doc = json.loads(report.read_text(encoding="utf-8"))
        doc["engines"][0]["exact_trtexec_command"] = (
            "/usr/src/tensorrt/bin/trtexec --onnx=https://bob:hunter2@example."
            "test/m.onnx --saveEngine=m.engine --fp16")
        report.write_text(json.dumps(doc), encoding="utf-8")
        body = self.body(d, onnx, engine, sidecar, report)
        out = d / "manifest.json"
        code, _, err = self.gen(d, body, out)
        self.refused(code, err, "credential")
        self.assertFalse(out.exists())

    def test_a_spec_mandated_system_path_is_still_publishable(self):
        """§3.2/§3.5/§8.4 require export_engine_command VERBATIM.

        `/usr/src/tensorrt/bin/trtexec` is the approved recipe. A leak rule that
        refused it would make every conforming manifest unpublishable.
        """
        d = self.work("rg-syspath")
        onnx, engine, sidecar, report = self.chain(d)
        body = self.body(d, onnx, engine, sidecar, report)
        out = d / "manifest.json"
        code, _, err = self.gen(d, body, out)
        self.assertEqual(code, 0, err)
        self.assertIn("/usr/src/tensorrt/bin/trtexec",
                      out.read_text(encoding="utf-8"))

    # -- F9: atomicity under fault injection and concurrency ---------------- #

    def test_no_clobber_is_not_a_check_then_replace_race(self):
        """A destination appearing after the check must not be overwritten."""
        d = self.work("rg-race")
        target = d / "racy.json"

        # fsync runs AFTER the existence check and BEFORE publication, so it is
        # the seam where a competing writer would land. Patching it simulates the
        # destination appearing inside that window — independent of whether the
        # implementation publishes with link() or replace().
        real_fsync = os.fsync

        def another_writer_wins(fd):
            if not target.exists():
                target.write_text("WINNER", encoding="utf-8")
            return real_fsync(fd)

        os.fsync = another_writer_wins
        try:
            with self.assertRaises(sidecar_tool.SidecarError):
                sidecar_tool.write_atomically(target, "LOSER", False)
        finally:
            os.fsync = real_fsync
        self.assertEqual(target.read_text(encoding="utf-8"), "WINNER")
        self.assertEqual(list(d.glob("*.part*")), [])

    def test_a_write_failure_leaves_no_temporary_and_no_output(self):
        """Fault injection: the publish step fails after the bytes are written."""
        d = self.work("rg-faultinject")
        target = d / "out.json"
        real_replace, real_link = os.replace, os.link

        def boom(src, dst):
            raise OSError("injected publication failure")

        # Both publication primitives: no-overwrite publishes via link(),
        # overwrite via replace(). Patching only one would leave the injected
        # fault unreached and the test vacuous.
        os.replace, os.link = boom, boom
        try:
            # A CONTROLLED refusal, not a raw OSError: publication failures are
            # wrapped so the CLI prints a redacted diagnostic.
            with self.assertRaises(sidecar_tool.SidecarError):
                sidecar_tool.write_atomically(target, '["Small"]', False)
        finally:
            os.replace, os.link = real_replace, real_link
        self.assertFalse(target.exists())
        self.assertEqual(list(d.glob("*.part")), [])
        self.assertEqual(list(d.glob("*.tmp*")), [])

    def test_concurrent_writers_do_not_share_a_temporary_file(self):
        d = self.work("rg-tmpunique")
        seen = []
        real_replace = os.replace

        def capture(src, dst):
            seen.append(Path(src).name)
            return real_replace(src, dst)

        os.replace = capture
        try:
            sidecar_tool.write_atomically(d / "a.json", "A", False)
            sidecar_tool.write_atomically(d / "b.json", "B", False)
            sidecar_tool.write_atomically(d / "a.json", "A2", True)
        finally:
            os.replace = real_replace
        self.assertEqual(len(seen), len(set(seen)),
                         f"temporary names must be unique, got {seen}")


# =========================================================================== #
# Second review round
# =========================================================================== #

class ReviewRegressions2(ReviewRegressions):

    # -- publication ordering ----------------------------------------------- #

    def test_a_missing_report_directory_does_not_leave_a_published_manifest(self):
        """Preflight must settle the report's PARENT, not just its existence.

        A nonexistent report directory is a deterministic failure, but it was
        only discovered by `mkstemp` — after the manifest had been published, and
        as a raw FileNotFoundError.
        """
        d = self.work("rg2-reportdir")
        onnx, engine, sidecar, report = self.chain(d)
        body = self.body(d, onnx, engine, sidecar, report)
        out = d / "manifest.json"
        code, _, err = self.gen(d, body, out,
                                "--report", d / "no-such-dir" / "report.json")
        # The PREFLIGHT must be what names it. Staging would also fail safely,
        # but only after work had begun and with a generic errno message.
        self.refused(code, err, "does not exist (or is not a directory)")
        self.assertFalse(out.exists())

    def test_a_missing_sidecar_report_directory_publishes_nothing(self):
        d = self.work("rg2-screportdir")
        onnx = self.fixture_onnx(d)
        engine = self.fixture_engine(d)
        rep = self.fixture_report(d, onnx_path=onnx, engine_path=engine)
        out = d / "m.names.json"
        code, _, err = self.sidecar_run(d, onnx, rep, out,
                                        "--report", d / "nope" / "r.json")
        self.refused(code, err, "does not exist (or is not a directory)")
        self.assertFalse(out.exists())

    # -- evidence completeness ---------------------------------------------- #

    def _slice2_record(self, d: Path, onnx: Path, **extra) -> Path:
        """A Slice-2 record. `extra` overrides, so a case can omit a field."""
        import hashlib
        rec = d / "rec.provenance.json"
        rec.write_text(json.dumps({
            "onnx": "m.onnx",
            "onnx_sha256": hashlib.sha256(onnx.read_bytes()).hexdigest(),
            "export_ultralytics": "8.4.33", "source_pt_sha256": "a" * 64,
            **extra,
        }), encoding="utf-8")
        return rec

    def test_slice2_record_that_failed_its_own_validation_is_refused(self):
        """`validation_passed: false` ALONE — no violations list to mask it.

        Both signals are checked independently; a test that trips both proves
        only that one of them works.
        """
        d = self.work("rg2-slice2invalid")
        onnx, engine, sidecar, report = self.chain(d)
        body = self.body(d, onnx, engine, sidecar, report)
        rec = self._slice2_record(d, onnx, validation_passed=False)
        body["provenance_from"] = [str(report), str(rec)]
        out = d / "manifest.json"
        code, _, err = self.gen(d, body, out)
        self.refused(code, err, "validation_passed")
        self.assertFalse(out.exists())

    def test_slice2_record_with_violations_is_refused(self):
        """Violations recorded ALONE, with validation_passed still true."""
        d = self.work("rg2-slice2violations")
        onnx, engine, sidecar, report = self.chain(d)
        body = self.body(d, onnx, engine, sidecar, report)
        rec = self._slice2_record(d, onnx, validation_passed=True,
                                  validation_violations=["output shape mismatch"])
        body["provenance_from"] = [str(report), str(rec)]
        out = d / "manifest.json"
        code, _, err = self.gen(d, body, out)
        self.refused(code, err, "validation_violations")
        self.assertFalse(out.exists())

    def test_manifest_checks_slice3_output_evidence_not_just_hashes(self):
        """A correct exit code and engine hash do not make the plan usable."""
        d = self.work("rg2-slice3shape")
        onnx, engine, sidecar, report = self.chain(d)
        doc = json.loads(report.read_text(encoding="utf-8"))
        doc["engines"][0]["output_shape"] = [1, 99, 8400]   # 95 classes, not 1
        report.write_text(json.dumps(doc), encoding="utf-8")
        body = self.body(d, onnx, engine, sidecar, report)
        out = d / "manifest.json"
        code, _, err = self.gen(d, body, out)
        self.refused(code, err, "shape")
        self.assertFalse(out.exists())

    def test_manifest_checks_slice3_output_dtype(self):
        d = self.work("rg2-slice3dtype")
        onnx, engine, sidecar, report = self.chain(d)
        doc = json.loads(report.read_text(encoding="utf-8"))
        doc["engines"][0]["output_dtype"] = "FP16 (fp16:CHW)"
        report.write_text(json.dumps(doc), encoding="utf-8")
        body = self.body(d, onnx, engine, sidecar, report)
        out = d / "manifest.json"
        code, _, err = self.gen(d, body, out)
        self.refused(code, err, "FP32")
        self.assertFalse(out.exists())

    # -- the mirror must be a real backstop --------------------------------- #

    def test_validate_manifest_object_is_a_complete_backstop(self):
        """Called directly: it must reject on its own, not lean on its callers.

        It is documented as a mirror of the C++ validator and is the last gate
        before bytes are published, so every rule must live IN it.
        """
        def good():
            return {"schema": 2, "generations": [{
                "name": "m", "canonical_id": "m", "family": "fam",
                "task": "detect", "input_size": 640,
                "class_names": ["Small"], "class_count": 1,
                "runtime": {"tensorrt": {
                    "engine": "m.engine", "engine_sha256": "a" * 64,
                    "sidecar": "m.names.json", "sidecar_sha256": "b" * 64,
                    "class_metadata_source": "names_sidecar",
                    "built_for": {"trt": "10.3", "cuda": "12.6", "sm": "87"}}},
                "provenance": {"source_pt_sha256": "c" * 64,
                               "onnx_sha256": "d" * 64,
                               "export_ultralytics": "8.4.33",
                               "precision": "fp16",
                               "export_engine_command": "trtexec --fp16"},
                "installed_utc": "2026-07-23T00:00:00Z", "state": "installed"}]}

        manifest_tool.validate_manifest_object(good())      # the control

        def mutate(**changes):
            m = good()
            m["generations"][0].update(changes)
            return m

        cases = {
            "state": mutate(state="staged"),
            "name": mutate(name=""),
            "installed_utc": mutate(installed_utc=""),
            "canonical_id": mutate(canonical_id="../evil"),
            "family": mutate(family=""),
            "task": mutate(task=""),
            "input_size": mutate(input_size=0),
            "blank class": mutate(class_names=["  "], class_count=1),
        }
        for label, manifest in cases.items():
            with self.subTest(rule=label):
                with self.assertRaises(manifest_tool.ManifestError):
                    manifest_tool.validate_manifest_object(manifest)

        # TensorRT-block rules, applied inside the block.
        for label, trt in {
            "engine basename": {"engine": "../m.engine"},
            "stem mismatch": {"sidecar": "other.names.json"},
            "built_for empty": {"built_for": {"trt": "", "cuda": "12.6", "sm": "87"}},
        }.items():
            with self.subTest(rule=label):
                m = good()
                m["generations"][0]["runtime"]["tensorrt"].update(trt)
                with self.assertRaises(manifest_tool.ManifestError):
                    manifest_tool.validate_manifest_object(m)

        # ONNX-block rules.
        for label, ort in {
            "not .onnx": {"model": "m.bin"},
            "bad basename": {"model": "../m.onnx"},
        }.items():
            with self.subTest(rule=label):
                m = good()
                m["generations"][0]["runtime"]["onnxruntime"] = {
                    "model": "m.onnx", "model_sha256": "e" * 64,
                    "class_metadata_source": "onnx_metadata_names"}
                m["generations"][0]["runtime"]["onnxruntime"].update(ort)
                with self.assertRaises(manifest_tool.ManifestError):
                    manifest_tool.validate_manifest_object(m)

        # Required provenance, enforced in the backstop itself.
        for field in ("source_pt_sha256", "onnx_sha256", "export_ultralytics",
                      "precision", "export_engine_command"):
            with self.subTest(rule=f"provenance.{field}"):
                m = good()
                m["generations"][0]["provenance"][field] = ""
                with self.assertRaises(manifest_tool.ManifestError):
                    manifest_tool.validate_manifest_object(m)

    # -- malformed descriptor containers ------------------------------------ #

    def test_built_for_as_a_list_is_a_controlled_refusal(self):
        """`set(["trt","cuda","sm"])` equals the required key set."""
        d = self.work("rg2-bflist")
        onnx, engine, sidecar, report = self.chain(d)
        body = self.body(d, onnx, engine, sidecar, report)
        body["tensorrt"]["built_for"] = ["trt", "cuda", "sm"]
        out = d / "manifest.json"
        code, _, err = self.gen(d, body, out)
        self.refused(code, err, "built_for")

    def test_provenance_as_a_list_is_a_controlled_refusal(self):
        d = self.work("rg2-provlist")
        onnx, engine, sidecar, report = self.chain(d)
        body = self.body(d, onnx, engine, sidecar, report)
        body["provenance"] = ["precision"]
        out = d / "manifest.json"
        code, _, err = self.gen(d, body, out)
        self.refused(code, err, "provenance")

    def test_provenance_from_as_a_string_is_a_controlled_refusal(self):
        d = self.work("rg2-provfromstr")
        onnx, engine, sidecar, report = self.chain(d)
        body = self.body(d, onnx, engine, sidecar, report)
        body["provenance_from"] = str(report)          # a string, not a list
        out = d / "manifest.json"
        code, _, err = self.gen(d, body, out)
        self.refused(code, err, "provenance_from")

    def test_manifest_wraps_onnx_inspection_failures(self):
        d = self.work("rg2-badnames")
        onnx = make_onnx(d / "m.onnx", names={0: "Small"})
        # A non-numeric class id makes inspect_onnx's `sorted(..., key=int)` raise.
        import onnx as onnx_mod
        model = onnx_mod.load(str(onnx))
        for entry in model.metadata_props:
            if entry.key == "names":
                entry.value = "{'zero': 'Small'}"
        onnx_mod.save(model, str(onnx))
        engine = self.fixture_engine(d)
        sidecar = d / "m.names.json"
        sidecar.write_text('["Small"]', encoding="utf-8")
        report = self.fixture_report(d, onnx_path=onnx, engine_path=engine)
        body = self.body(d, onnx, engine, sidecar, report)
        out = d / "manifest.json"
        code, _, err = self.gen(d, body, out)
        self.assertEqual(code, 1, err)
        self.assertNotIn(UNCONTROLLED, err)

    # -- publication primitive ---------------------------------------------- #

    def test_no_overwrite_never_falls_back_to_a_racy_replace(self):
        """With hardlinks unavailable, publication must stay atomic.

        The old fallback re-checked `exists()` and then called `os.replace`,
        which reopens exactly the window the check was there to close.
        """
        d = self.work("rg2-nofallback")
        target = d / "x.json"
        real_link, real_fsync = os.link, os.fsync

        def no_hardlinks(src, dst):
            raise OSError("hardlinks unsupported on this filesystem")

        def competitor_wins(fd):
            if not target.exists():
                target.write_text("WINNER", encoding="utf-8")
            return real_fsync(fd)

        os.link, os.fsync = no_hardlinks, competitor_wins
        try:
            with self.assertRaises(sidecar_tool.SidecarError):
                sidecar_tool.write_atomically(target, "LOSER", False)
        finally:
            os.link, os.fsync = real_link, real_fsync
        self.assertEqual(target.read_text(encoding="utf-8"), "WINNER")

    def test_without_hardlinks_no_overwrite_fails_closed(self):
        """No degraded fallback, and above all NO empty destination.

        The two tempting fallbacks are both worse than refusing: `exists()` then
        `replace()` reopens the race, and an `O_CREAT|O_EXCL` reservation
        publishes a visible ZERO-BYTE file that survives an interrupted process.
        Refusing is the only outcome that cannot produce a wrong artifact.
        """
        d = self.work("rg2-nofallbackatall")
        target = d / "y.json"
        real_link = os.link

        def no_hardlinks(src, dst):
            raise OSError("hardlinks unsupported on this filesystem")

        os.link = no_hardlinks
        try:
            with self.assertRaises(sidecar_tool.SidecarError):
                sidecar_tool.write_atomically(target, '["Small"]', False)
        finally:
            os.link = real_link
        self.assertFalse(target.exists(), "no empty reservation may be left")
        self.assertEqual(list(d.glob("*.part*")), [])

    def test_overwrite_still_works_without_hardlinks(self):
        """--overwrite uses replace(), so it is unaffected by the link rule."""
        d = self.work("rg2-overwritenolink")
        target = d / "z.json"
        target.write_text("OLD", encoding="utf-8")
        real_link = os.link

        def no_hardlinks(src, dst):
            raise OSError("hardlinks unsupported")

        os.link = no_hardlinks
        try:
            sidecar_tool.write_atomically(target, '["Small"]', True)
        finally:
            os.link = real_link
        self.assertEqual(target.read_text(encoding="utf-8"), '["Small"]')

    # -- absolute-path leakage ---------------------------------------------- #

    def test_an_unapproved_absolute_path_is_refused(self):
        d = self.work("rg2-abspath")
        onnx, engine, sidecar, report = self.chain(d)
        doc = json.loads(report.read_text(encoding="utf-8"))
        doc["engines"][0]["exact_trtexec_command"] = (
            "/srv/builds/alice/private/trtexec --onnx=m.onnx --fp16")
        report.write_text(json.dumps(doc), encoding="utf-8")
        body = self.body(d, onnx, engine, sidecar, report)
        out = d / "manifest.json"
        code, _, err = self.gen(d, body, out)
        self.refused(code, err, "absolute path")
        self.assertFalse(out.exists())

    def test_the_approved_tensorrt_path_remains_publishable(self):
        """§8.4 fixes the recipe at /usr/src/tensorrt/bin/trtexec."""
        d = self.work("rg2-approvedpath")
        onnx, engine, sidecar, report = self.chain(d)
        body = self.body(d, onnx, engine, sidecar, report)
        out = d / "manifest.json"
        code, _, err = self.gen(d, body, out)
        self.assertEqual(code, 0, err)
        self.assertIn("/usr/src/tensorrt/bin/trtexec",
                      out.read_text(encoding="utf-8"))

    # -- engine filename binding -------------------------------------------- #

    # -- third round -------------------------------------------------------- #

    def test_slice2_record_without_a_validation_field_is_refused(self):
        """Absence is not a pass — an authoritative record always emits it."""
        d = self.work("rg3-noval")
        onnx, engine, sidecar, report = self.chain(d)
        body = self.body(d, onnx, engine, sidecar, report)
        rec = self._slice2_record(d, onnx)          # no validation_passed at all
        body["provenance_from"] = [str(report), str(rec)]
        out = d / "manifest.json"
        code, _, err = self.gen(d, body, out)
        self.refused(code, err, "validation_passed")
        self.assertFalse(out.exists())

    def test_manifest_checks_slice3_output_name(self):
        d = self.work("rg3-outname")
        onnx, engine, sidecar, report = self.chain(d)
        doc = json.loads(report.read_text(encoding="utf-8"))
        doc["engines"][0]["output_name"] = "detections"
        report.write_text(json.dumps(doc), encoding="utf-8")
        body = self.body(d, onnx, engine, sidecar, report)
        out = d / "manifest.json"
        code, _, err = self.gen(d, body, out)
        self.refused(code, err, "output name")
        self.assertFalse(out.exists())

    def test_end_to_end_engine_evidence_is_not_misread_as_a_classic_head(self):
        """digitv3 is `[1,300,6]` with TEN classes — `channels - 4` would say 296.

        The end-to-end layout carries no class count in its channel dimension, so
        deriving one there would reject the very model Release A exists to
        declare. The sidecar stays the class authority (§8.5).
        """
        d = self.work("rg3-e2e")
        names = {i: str(i) for i in range(10)}
        onnx = make_onnx(d / "m.onnx", names=names, output_shape=(1, 300, 6))
        engine = self.fixture_engine(d)
        sidecar = d / "m.names.json"
        sidecar.write_text(json.dumps([str(i) for i in range(10)],
                                      separators=(",", ":")), encoding="utf-8")
        report = self.fixture_report(d, onnx_path=onnx, engine_path=engine,
                                     output_shape=(1, 300, 6))
        body = self.body(d, onnx, engine, sidecar, report)
        out = d / "manifest.json"
        code, _, err = self.gen(d, body, out)
        self.assertEqual(code, 0, err)
        g = json.loads(out.read_text(encoding="utf-8"))["generations"][0]
        self.assertEqual(g["class_count"], 10)

    def test_prefix_traversal_out_of_the_approved_path_is_refused(self):
        d = self.work("rg3-traversal")
        onnx, engine, sidecar, report = self.chain(d)
        doc = json.loads(report.read_text(encoding="utf-8"))
        doc["engines"][0]["exact_trtexec_command"] = (
            "/usr/src/tensorrt/../../srv/builds/alice/trtexec --fp16")
        report.write_text(json.dumps(doc), encoding="utf-8")
        body = self.body(d, onnx, engine, sidecar, report)
        out = d / "manifest.json"
        code, _, err = self.gen(d, body, out)
        self.refused(code, err, "absolute path")
        self.assertFalse(out.exists())

    def test_an_unrelated_file_under_the_approved_tree_is_refused(self):
        """The allowlist is the exact executable, not the whole subtree."""
        d = self.work("rg3-subtree")
        onnx, engine, sidecar, report = self.chain(d)
        doc = json.loads(report.read_text(encoding="utf-8"))
        doc["engines"][0]["exact_trtexec_command"] = (
            "/usr/src/tensorrt/bin/trtexec --onnx=/usr/src/tensorrt/secret.onnx "
            "--fp16")
        report.write_text(json.dumps(doc), encoding="utf-8")
        body = self.body(d, onnx, engine, sidecar, report)
        out = d / "manifest.json"
        code, _, err = self.gen(d, body, out)
        self.refused(code, err, "absolute path")

    def test_an_equivalent_spelling_of_the_approved_path_is_accepted(self):
        """Normalisation makes the allowlist a comparison of PATHS, not strings.

        `/usr/src/tensorrt/bin/../bin/trtexec` is the approved executable spelled
        differently. Without normalisation the allowlist would be a string match
        that a legitimate command could miss.
        """
        d = self.work("rg3-normequiv")
        onnx, engine, sidecar, report = self.chain(d)
        doc = json.loads(report.read_text(encoding="utf-8"))
        doc["engines"][0]["exact_trtexec_command"] = (
            "/usr/src/tensorrt/bin/../bin/trtexec --onnx=m.onnx --fp16")
        report.write_text(json.dumps(doc), encoding="utf-8")
        body = self.body(d, onnx, engine, sidecar, report)
        out = d / "manifest.json"
        code, _, err = self.gen(d, body, out)
        self.assertEqual(code, 0, err)

    def test_unc_and_redirection_targets_are_refused(self):
        for label, command in {
            "unc": "trtexec --onnx=m.onnx --saveEngine=\\\\server\\share\\m.engine",
            "redirection": "trtexec --onnx=m.onnx --fp16 >/srv/private/build.log",
            # A file-descriptor prefix glues to the operator, so the token starts
            # with a DIGIT and never looked like a path.
            "fd-stderr": "trtexec --onnx=m.onnx 2>/srv/private/build.log",
            "fd-append": "trtexec --onnx=m.onnx 2>>/srv/private/build.log",
            "fd-stdin": "trtexec --onnx=m.onnx 0</srv/private/input",
            "fd-both": "trtexec --onnx=m.onnx &>/srv/private/build.log",
        }.items():
            with self.subTest(form=label):
                d = self.work(f"rg3-{label}")
                onnx, engine, sidecar, report = self.chain(d)
                doc = json.loads(report.read_text(encoding="utf-8"))
                doc["engines"][0]["exact_trtexec_command"] = command
                report.write_text(json.dumps(doc), encoding="utf-8")
                body = self.body(d, onnx, engine, sidecar, report)
                out = d / "manifest.json"
                code, _, err = self.gen(d, body, out)
                self.assertEqual(code, 1, err)
                self.assertNotIn(UNCONTROLLED, err)
                self.assertFalse(out.exists())

    def test_a_report_failure_after_preflight_publishes_no_primary(self):
        """Fault injected at PUBLICATION, after every preflight has passed.

        Everything is staged before anything is published and the primary is
        published last, so a late failure cannot leave an accepted artifact.
        """
        for tool_label in ("manifest", "sidecar"):
            with self.subTest(tool=tool_label):
                d = self.work(f"rg3-latefail-{tool_label}")
                onnx, engine, sidecar, report = self.chain(d)
                # A separate publication directory: `chain()` already put an
                # `m.names.json` fixture in `d`, and the sidecar's output name is
                # pinned to the engine stem.
                pub = d / "pub"
                pub.mkdir(exist_ok=True)
                out = pub / ("manifest.json" if tool_label == "manifest"
                             else "m.names.json")
                rep = pub / "r.json"
                real_link, real_replace = os.link, os.replace

                def fail_the_report_publish(src, dst):
                    # Keyed on the DESTINATION, not on call order: keying on
                    # "the first call" would still fire if the primary were
                    # published first, so the test would pass under exactly the
                    # ordering it exists to forbid.
                    if Path(dst) == rep:
                        raise OSError("injected publication failure")
                    return real_link(src, dst)

                os.link = fail_the_report_publish
                try:
                    if tool_label == "manifest":
                        body = self.body(d, onnx, engine, sidecar, report)
                        code, _, err = self.gen(d, body, out, "--report", rep)
                    else:
                        code, _, err = self.sidecar_run(d, onnx, report, out,
                                                        "--report", rep)
                finally:
                    os.link, os.replace = real_link, real_replace
                self.assertEqual(code, 1, err)
                self.assertNotIn(UNCONTROLLED, err)
                self.assertFalse(out.exists(),
                                 "the primary must not survive a failed run")
                self.assertEqual(list(pub.glob("*.part*")), [],
                                 "no staged temporary may be left behind")

    # -- fourth round ------------------------------------------------------- #

    def test_a_failure_staging_the_primary_cleans_up_the_staged_report(self):
        """Staging is ONE transaction: a later failure removes what came before."""
        d = self.work("rg4-stagefail")
        pub = d / "pub"
        pub.mkdir(exist_ok=True)
        onnx, engine, sidecar, report = self.chain(d)
        out = pub / "m.names.json"
        rep = pub / "r.json"
        real_stage = sidecar_tool.stage_atomically
        calls = {"n": 0}

        def fail_second_stage(path, text):
            calls["n"] += 1
            if calls["n"] == 2:                 # the primary, staged after the report
                raise OSError("injected staging failure (disk full)")
            return real_stage(path, text)

        sidecar_tool.stage_atomically = fail_second_stage
        try:
            code, _, err = self.sidecar_run(d, onnx, report, out, "--report", rep)
        finally:
            sidecar_tool.stage_atomically = real_stage
        self.assertEqual(code, 1, err)
        self.assertNotIn(UNCONTROLLED, err)
        self.assertFalse(out.exists())
        self.assertFalse(rep.exists())
        self.assertEqual(list(pub.glob("*.part*")), [],
                         "the already-staged report must not be left behind")

    def test_an_overwrite_publication_failure_is_controlled(self):
        """`--overwrite` publishes with replace(); its errors need wrapping too."""
        d = self.work("rg4-replacefail")
        target = d / "t.json"
        target.write_text("OLD", encoding="utf-8")
        real_replace = os.replace

        def boom(src, dst):
            raise OSError("injected replace failure")

        os.replace = boom
        try:
            with self.assertRaises(sidecar_tool.SidecarError):
                sidecar_tool.write_atomically(target, '["Small"]', True)
        finally:
            os.replace = real_replace
        self.assertEqual(list(d.glob("*.part*")), [])

    def test_malformed_validation_violations_is_refused(self):
        """Truthiness is not a type check: `{}` and `""` are both falsy."""
        for label, value in {"object": {}, "string": "", "number": 0}.items():
            with self.subTest(kind=label):
                d = self.work(f"rg4-violations-{label}")
                onnx, engine, sidecar, report = self.chain(d)
                body = self.body(d, onnx, engine, sidecar, report)
                rec = self._slice2_record(d, onnx, validation_passed=True,
                                          validation_violations=value)
                body["provenance_from"] = [str(report), str(rec)]
                out = d / "manifest.json"
                code, _, err = self.gen(d, body, out)
                self.refused(code, err, "validation_violations")
                self.assertFalse(out.exists())

    def test_a_build_entry_without_an_engine_filename_is_refused(self):
        """An empty stem would match an output literally named `.names.json`."""
        d = self.work("rg2-noenginename")
        onnx = self.fixture_onnx(d)
        engine = self.fixture_engine(d)
        report = self.fixture_report(d, onnx_path=onnx, engine_path=engine)
        doc = json.loads(report.read_text(encoding="utf-8"))
        del doc["engines"][0]["engine_filename"]
        report.write_text(json.dumps(doc), encoding="utf-8")
        code, _, err = self.sidecar_run(d, onnx, report, d / ".names.json")
        self.refused(code, err, "engine_filename")


if __name__ == "__main__":
    unittest.main(verbosity=2)
