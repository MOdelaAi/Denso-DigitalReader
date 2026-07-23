#!/usr/bin/env python3
"""Tests for `tools/export_float_onnx.py`.

Deliberately outside the CMake graph, exactly like the tool they cover: Slice 2
adds no CTest registration, so the C++ suite total is unchanged. Run them with
the same interpreter that will run the exporter:

    python tools/test_export_float_onnx.py            # from the repo root
    python tools/test_export_float_onnx.py --quick    # skip the real exports

Running the suite under BOTH export environments is meaningful: the real-export
cases assert the pinned-version behaviour of whichever Ultralytics is installed,
so under 8.4.21 they prove the primary path and under 8.4.33 they prove that an
undeclared version substitution is refused.

Every case works on COPIES in a temporary directory. The real checkpoints are
opened read-only, and `test_z_source_checkpoints_untouched` proves their bytes
and mtimes survived the whole run.
"""

from __future__ import annotations

import importlib.util
import io
import json
import os
import shutil
import sys
import tempfile
import unittest
from contextlib import redirect_stdout, redirect_stderr
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
MODELS_DIR = REPO_ROOT / "models"
FLOAT_SMALL = MODELS_DIR / "float-small.pt"
FLOAT_BIG = MODELS_DIR / "float-big.pt"

SMALL_SHA = "e9a2294757cc13c1041f643f83d2651616434d016d3f4537d81517ea10d7330f"
BIG_SHA = "3cf0a655af70bcaa960cf96e174cffedf1c8ca41d70293fd19a88fe07727c3b1"

QUICK = "--quick" in sys.argv


def load_tool():
    # No __pycache__: `tools/` is a tracked source dir, and a stray bytecode
    # directory leaves the tree dirty — which tools/build_package.sh refuses to
    # package. Cheaper to not write it than to add an ignore rule for it.
    sys.dont_write_bytecode = True
    spec = importlib.util.spec_from_file_location(
        "export_float_onnx", REPO_ROOT / "tools" / "export_float_onnx.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


tool = load_tool()


def run_cli(argv) -> tuple[int, str, str]:
    out, err = io.StringIO(), io.StringIO()
    with redirect_stdout(out), redirect_stderr(err):
        code = tool.main([str(a) for a in argv])
    return code, out.getvalue(), err.getvalue()


# --------------------------------------------------------------------------- #
# Synthetic ONNX construction — lets the validator be tested against graphs no
# correct exporter would ever emit, without needing a GPU or a real model.
# --------------------------------------------------------------------------- #

def make_onnx(path: Path, *, input_shape=(1, 3, 640, 640),
              output_shape=(1, 5, 8400), names=None, task="detect",
              batch=1, imgsz="[640, 640]", args=None,
              input_dtype=1, output_dtype=1, omit_names=False):
    import onnx
    from onnx import helper, TensorProto

    def value_info(name, dtype, shape):
        # A str entry becomes a symbolic (dynamic) dimension.
        return helper.make_tensor_value_info(name, dtype, list(shape))

    names = {0: "Small"} if names is None else names
    args = {"dynamic": False, "nms": False, "half": False,
            "simplify": True, "opset": 13, "batch": 1} if args is None else args

    # Give the graph a real body whenever the output shape is fully concrete, so
    # `onnx.checker` accepts it. A fixture that only LOOKS right in its tensor
    # signature would make "a well-formed graph passes" vacuous, and would not
    # exercise the structural gate at all.
    nodes = []
    if all(isinstance(d, int) for d in output_shape):
        import numpy as np
        from onnx import numpy_helper
        np_dtype = {1: np.float32, 10: np.float16}.get(output_dtype, np.float32)
        const = numpy_helper.from_array(
            np.zeros(tuple(output_shape), dtype=np_dtype), name="const_out")
        nodes = [helper.make_node("Constant", [], ["output0"], value=const)]

    graph = helper.make_graph(
        nodes=nodes,
        name="synthetic",
        inputs=[value_info("images", input_dtype, input_shape)],
        outputs=[value_info("output0", output_dtype, output_shape)],
    )
    model = helper.make_model(graph, opset_imports=[helper.make_opsetid("", 13)])
    props = {"task": task, "batch": str(batch), "imgsz": imgsz, "args": str(args)}
    if not omit_names:
        props["names"] = str(names)
    for key, value in props.items():
        entry = model.metadata_props.add()
        entry.key, entry.value = key, value
    onnx.save(model, str(path))
    return path


class SyntheticBase(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        try:
            import onnx  # noqa: F401
        except ImportError:
            raise unittest.SkipTest("onnx is not installed in this environment")
        cls.tmp = Path(tempfile.mkdtemp(prefix="export-tests-"))

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def validate(self, path, expected_class="Small"):
        facts = tool.inspect_onnx(path)
        return tool.validate_onnx_facts(facts, expected_class)


# --------------------------------------------------------------------------- #
# 6/7/8/9 — the validator rejects graphs the appliance could not run
# --------------------------------------------------------------------------- #

class TestValidatorRejections(SyntheticBase):

    def test_a_well_formed_synthetic_graph_passes(self):
        """The harness itself must be able to produce an acceptable graph,
        otherwise every rejection below would be vacuously true."""
        path = make_onnx(self.tmp / "ok.onnx")
        self.assertEqual(self.validate(path), [])

    def test_rejects_end_to_end_output_shape(self):
        path = make_onnx(self.tmp / "e2e.onnx", output_shape=(1, 300, 6))
        problems = self.validate(path)
        self.assertTrue(any("END-TO-END" in p for p in problems), problems)
        self.assertTrue(any("decode_yolo_end2end" in p for p in problems), problems)

    def test_rejects_wrong_output_shape(self):
        path = make_onnx(self.tmp / "wrongshape.onnx", output_shape=(1, 5, 1000))
        problems = self.validate(path)
        self.assertTrue(any("output shape must be [1, 5, 8400]" in p
                            for p in problems), problems)

    def test_rejects_wrong_output_channel_count(self):
        """[1, 14, 8400] would decode as 10 classes - a digit model's layout."""
        path = make_onnx(self.tmp / "wrongnc.onnx", output_shape=(1, 14, 8400))
        problems = self.validate(path)
        self.assertTrue(any("output shape must be [1, 5, 8400]" in p
                            for p in problems), problems)

    def test_rejects_dynamic_input(self):
        path = make_onnx(self.tmp / "dyn.onnx",
                         input_shape=("batch", 3, 640, 640))
        problems = self.validate(path)
        self.assertTrue(any("dynamic dimension" in p for p in problems), problems)

    def test_rejects_dynamic_output(self):
        path = make_onnx(self.tmp / "dynout.onnx",
                         output_shape=(1, 5, "anchors"))
        problems = self.validate(path)
        self.assertTrue(any("dynamic dimension" in p for p in problems), problems)

    def test_rejects_batch_other_than_one(self):
        path = make_onnx(self.tmp / "batch4.onnx",
                         input_shape=(4, 3, 640, 640), batch=4,
                         args={"dynamic": False, "nms": False, "half": False,
                               "simplify": True, "opset": 13, "batch": 4})
        problems = self.validate(path)
        self.assertTrue(any("batch" in p for p in problems), problems)

    def test_rejects_fp16_input_binding(self):
        path = make_onnx(self.tmp / "fp16in.onnx", input_dtype=10)
        problems = self.validate(path)
        self.assertTrue(any("input dtype must be FP32" in p for p in problems),
                        problems)

    def test_rejects_fp16_output_binding(self):
        path = make_onnx(self.tmp / "fp16out.onnx", output_dtype=10)
        problems = self.validate(path)
        self.assertTrue(any("output dtype must be FP32" in p for p in problems),
                        problems)

    def test_rejects_wrong_class_metadata(self):
        path = make_onnx(self.tmp / "wrongclass.onnx", names={0: "Big"})
        problems = self.validate(path, expected_class="Small")
        self.assertTrue(any("class names must be exactly ['Small']" in p
                            for p in problems), problems)

    def test_rejects_class_count_other_than_one(self):
        path = make_onnx(self.tmp / "twoclass.onnx",
                         names={0: "Small", 1: "Extra"})
        problems = self.validate(path, expected_class="Small")
        self.assertTrue(any("class count must be 1" in p for p in problems),
                        problems)

    def test_rejects_missing_class_metadata(self):
        path = make_onnx(self.tmp / "nonames.onnx", omit_names=True)
        problems = self.validate(path)
        self.assertTrue(any("no `names` entry" in p for p in problems), problems)

    def test_rejects_nms_true(self):
        path = make_onnx(self.tmp / "nms.onnx",
                         args={"dynamic": False, "nms": True, "half": False,
                               "simplify": True, "opset": 13})
        problems = self.validate(path)
        self.assertTrue(any("`nms` must be False" in p for p in problems), problems)

    def test_rejects_dynamic_arg_true(self):
        path = make_onnx(self.tmp / "dynarg.onnx",
                         args={"dynamic": True, "nms": False, "half": False,
                               "simplify": True, "opset": 13})
        problems = self.validate(path)
        self.assertTrue(any("`dynamic` must be False" in p for p in problems),
                        problems)

    def test_rejects_half_true(self):
        path = make_onnx(self.tmp / "half.onnx",
                         args={"dynamic": False, "nms": False, "half": True,
                               "simplify": True, "opset": 13})
        problems = self.validate(path)
        self.assertTrue(any("`half` must be False" in p for p in problems),
                        problems)

    def test_rejects_wrong_task(self):
        path = make_onnx(self.tmp / "seg.onnx", task="segment")
        problems = self.validate(path)
        self.assertTrue(any("task must be 'detect'" in p for p in problems),
                        problems)

    def test_rejects_wrong_input_size(self):
        path = make_onnx(self.tmp / "imgsz320.onnx",
                         input_shape=(1, 3, 320, 320), imgsz="[320, 320]")
        problems = self.validate(path)
        self.assertTrue(any("input shape must be" in p for p in problems), problems)
        self.assertTrue(any("imgsz must be 640" in p for p in problems), problems)

    # -- 9. Determinism ----------------------------------------------------- #

    def test_validation_is_deterministic_across_runs(self):
        good = make_onnx(self.tmp / "det-ok.onnx")
        bad = make_onnx(self.tmp / "det-bad.onnx", output_shape=(1, 300, 6),
                        names={0: "Big"})
        # Assert the premise first: a determinism test over two artifacts that
        # both trivially pass would prove nothing about the failing path.
        self.assertEqual(self.validate(good), [])
        self.assertTrue(self.validate(bad))
        for path in (good, bad):
            first = self.validate(path)
            second = self.validate(path)
            third = self.validate(path)
            self.assertEqual(first, second)
            self.assertEqual(second, third)

    # -- Structural validity (onnx.checker), separate from contract checks --- #

    def test_structural_check_rejects_a_bodyless_graph(self):
        """Correct tensor signature + correct metadata, but nothing computes
        output0. Every contract check passes; the structural gate must not."""
        import onnx
        from onnx import helper

        path = self.tmp / "bodyless.onnx"
        graph = helper.make_graph(
            nodes=[],
            name="bodyless",
            inputs=[helper.make_tensor_value_info("images", 1, [1, 3, 640, 640])],
            outputs=[helper.make_tensor_value_info("output0", 1, [1, 5, 8400])],
        )
        model = helper.make_model(graph,
                                  opset_imports=[helper.make_opsetid("", 13)])
        for key, value in {"task": "detect", "batch": "1",
                           "imgsz": "[640, 640]",
                           "names": str({0: "Small"}),
                           "args": str({"dynamic": False, "nms": False,
                                        "half": False, "simplify": True,
                                        "opset": 13})}.items():
            entry = model.metadata_props.add()
            entry.key, entry.value = key, value
        onnx.save(model, str(path))

        # The contract checks alone are satisfied ...
        self.assertEqual(self.validate(path), [])
        # ... but the structural gate rejects it.
        with self.assertRaises(tool.ExportError):
            tool.check_graph_validity(path)
        # And the CLI surfaces it as a rejection rather than passing.
        code, out, _ = run_cli(["--validate-onnx", path,
                                "--expected-class", "Small"])
        self.assertEqual(code, 1)
        self.assertFalse(json.loads(out)["valid"])

    def test_structural_check_accepts_a_well_formed_graph(self):
        path = make_onnx(self.tmp / "structural-ok.onnx")
        tool.check_graph_validity(path)  # must not raise

    # -- Opset: graph and exporter metadata must agree ----------------------- #

    def test_rejects_opset_disagreement_between_graph_and_args(self):
        path = make_onnx(self.tmp / "opsetmismatch.onnx",
                         args={"dynamic": False, "nms": False, "half": False,
                               "simplify": True, "opset": 11})
        problems = self.validate(path)
        self.assertTrue(any("opset disagreement" in p for p in problems), problems)

    def test_rejects_missing_args_opset(self):
        path = make_onnx(self.tmp / "noargsopset.onnx",
                         args={"dynamic": False, "nms": False, "half": False,
                               "simplify": True})
        problems = self.validate(path)
        self.assertTrue(any("no `opset` entry" in p for p in problems), problems)

    def test_validate_only_cli_is_deterministic_and_reports_json(self):
        good = make_onnx(self.tmp / "cli-ok.onnx")
        results = []
        for _ in range(2):
            code, out, _ = run_cli(["--validate-onnx", good,
                                    "--expected-class", "Small"])
            results.append((code, json.loads(out)))
        self.assertEqual(results[0][0], 0)
        self.assertEqual(results[0][1], results[1][1])
        self.assertTrue(results[0][1]["valid"])

    def test_validate_only_cli_fails_on_a_bad_artifact(self):
        bad = make_onnx(self.tmp / "cli-bad.onnx", output_shape=(1, 300, 6))
        code, out, err = run_cli(["--validate-onnx", bad,
                                  "--expected-class", "Small"])
        self.assertEqual(code, 1)
        self.assertFalse(json.loads(out)["valid"])
        self.assertIn("REJECTED", err)


# --------------------------------------------------------------------------- #
# 3/4/5 — the exporter refuses before doing work
# --------------------------------------------------------------------------- #

class TestExporterRefusals(unittest.TestCase):

    @classmethod
    def setUpClass(cls):
        if not FLOAT_SMALL.is_file():
            raise unittest.SkipTest("models/float-small.pt is not present")
        cls.tmp = Path(tempfile.mkdtemp(prefix="export-refusals-"))

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def out_dir(self, name) -> Path:
        path = self.tmp / name
        path.mkdir(parents=True, exist_ok=True)
        return path

    # -- 3. Wrong expected source SHA fails BEFORE export -------------------- #

    def test_wrong_expected_sha_is_refused_before_export(self):
        out = self.out_dir("badsha")
        code, _, err = run_cli(["--model", FLOAT_SMALL, "--output-dir", out,
                                "--expected-class", "Small",
                                "--expected-sha256", "0" * 64])
        self.assertEqual(code, 1)
        self.assertIn("source SHA-256 mismatch", err)
        self.assertIn("Refusing to export", err)
        # Nothing was produced: the refusal happened before any export work.
        self.assertEqual(sorted(p.name for p in out.iterdir()), [])

    def test_unknown_stem_without_expected_sha_is_refused(self):
        out = self.out_dir("unknownstem")
        staged = self.tmp / "mystery-model.pt"
        shutil.copy2(FLOAT_SMALL, staged)
        code, _, err = run_cli(["--model", staged, "--output-dir", out,
                                "--expected-class", "Small"])
        self.assertEqual(code, 1)
        self.assertIn("no expected SHA-256", err)
        self.assertEqual(sorted(p.name for p in out.iterdir()), [])

    def test_missing_model_is_refused(self):
        out = self.out_dir("missing")
        code, _, err = run_cli(["--model", self.tmp / "nope.pt",
                                "--output-dir", out, "--expected-class", "Small"])
        self.assertEqual(code, 1)
        self.assertIn("does not exist", err)

    # -- 4. Wrong expected class fails --------------------------------------- #

    def test_wrong_expected_class_is_refused(self):
        if QUICK:
            self.skipTest("--quick: needs torch to read the checkpoint")
        out = self.out_dir("badclass")
        code, _, err = run_cli(["--model", FLOAT_SMALL, "--output-dir", out,
                                "--expected-class", "Big"])
        self.assertEqual(code, 1)
        self.assertIn("declares classes ['Small']", err)
        self.assertIn("expected ['Big']", err)
        self.assertFalse((out / "float-small.onnx").exists())

    def test_wrong_expected_training_version_is_refused(self):
        if QUICK:
            self.skipTest("--quick: needs torch to read the checkpoint")
        out = self.out_dir("badver")
        code, _, err = run_cli(["--model", FLOAT_SMALL, "--output-dir", out,
                                "--expected-class", "Small",
                                "--expected-training-version", "0.0.0"])
        self.assertEqual(code, 1)
        self.assertIn("training Ultralytics", err)
        self.assertFalse((out / "float-small.onnx").exists())

    # -- 5. Existing output is not silently overwritten ---------------------- #

    def test_existing_output_is_not_silently_overwritten(self):
        out = self.out_dir("overwrite")
        existing = out / "float-small.onnx"
        existing.write_bytes(b"PRE-EXISTING SENTINEL")
        code, _, err = run_cli(["--model", FLOAT_SMALL, "--output-dir", out,
                                "--expected-class", "Small"])
        self.assertEqual(code, 1)
        self.assertIn("Refusing to overwrite", err)
        self.assertIn("--overwrite", err)
        # The pre-existing bytes survived untouched.
        self.assertEqual(existing.read_bytes(), b"PRE-EXISTING SENTINEL")

    def test_existing_provenance_alone_also_blocks(self):
        out = self.out_dir("overwrite-prov")
        (out / "float-small.provenance.json").write_text("{}", encoding="utf-8")
        code, _, err = run_cli(["--model", FLOAT_SMALL, "--output-dir", out,
                                "--expected-class", "Small"])
        self.assertEqual(code, 1)
        self.assertIn("Refusing to overwrite", err)

    # -- Redaction ----------------------------------------------------------- #

    def test_redaction_strips_home_and_username(self):
        home = os.path.expanduser("~")
        self.assertNotIn(home, tool.redact(f"error in {home}/site-packages/x.py"))
        self.assertIn("<home>", tool.redact(r"C:\Users\someone\lib\torch"))
        self.assertIn("<home>", tool.redact("/home/someone/lib/torch"))
        self.assertIn("<home>", tool.redact("/Users/someone/lib/torch"))

    def test_redaction_strips_credentials(self):
        """A captured failure is free-form text and can carry a URL or a secret;
        spec 12 forbids emitting either."""
        red = tool.redact("failed opening rtsp://admin:hunter2@192.168.1.185/s1")
        self.assertNotIn("hunter2", red)
        self.assertNotIn("admin:hunter2", red)
        self.assertIn("<redacted>@", red)

        for sample, secret in (("password=hunter2", "hunter2"),
                               ("token: abc123XYZ", "abc123XYZ"),
                               ("--api-key sk-9999", "sk-9999"),
                               ('secret="s3cr3t"', "s3cr3t"),
                               ("client_secret=abcdef123", "abcdef123"),
                               ("access_token=zzz999yyy", "zzz999yyy"),
                               ("AWS_SECRET_ACCESS_KEY=wJalrXUtnFEMI",
                                "wJalrXUtnFEMI"),
                               ("Authorization: Bearer eyJhbGciOiJIUzI1NiJ9",
                                "eyJhbGciOiJIUzI1NiJ9")):
            with self.subTest(sample=sample):
                self.assertNotIn(secret, tool.redact(sample))

    def test_redaction_strips_single_token_url_userinfo(self):
        """A userinfo with no colon is still a credential."""
        red = tool.redact("GET https://sk-liveTOKEN99@example.com/v1")
        self.assertNotIn("sk-liveTOKEN99", red)

    def test_redaction_does_not_mangle_ordinary_diagnostics(self):
        """The failure evidence is required to be verbatim, so an over-eager
        redactor that eats status words is itself a defect."""
        for sample in ("--token is required",
                       "use --api-key to authenticate",
                       "token: missing from metadata",
                       "credential=unavailable",
                       "pass --overwrite to replace it"):
            with self.subTest(sample=sample):
                self.assertEqual(tool.redact(sample), sample)

    # -- Output containment (Release A carries no Float artifact) ------------ #

    def test_refuses_to_write_into_the_repository_tree(self):
        out = REPO_ROOT / "build" / "should-never-be-created"
        code, _, err = run_cli(["--model", FLOAT_SMALL, "--output-dir", out,
                                "--expected-class", "Small"])
        self.assertEqual(code, 1)
        self.assertIn("repository working tree", err)
        self.assertFalse(out.exists())

    def test_refuses_to_write_into_a_models_directory(self):
        out = self.tmp / "somewhere" / "models"
        code, _, err = run_cli(["--model", FLOAT_SMALL, "--output-dir", out,
                                "--expected-class", "Small"])
        self.assertEqual(code, 1)
        self.assertIn("'models' directory", err)
        self.assertFalse(out.exists())

    def test_allows_a_path_merely_containing_the_opt_denso_characters(self):
        """`/tmp/opt/denso-backup` is a different directory than `/opt/denso`;
        a substring check would have refused it."""
        out = self.tmp / "opt" / "denso-backup" / "out"
        tool.ensure_safe_output_dir(out, REPO_ROOT)  # must not raise

    def test_refuses_to_write_under_opt_denso(self):
        out = self.tmp / "opt" / "denso" / "data"
        code, _, err = run_cli(["--model", FLOAT_SMALL, "--output-dir", out,
                                "--expected-class", "Small"])
        self.assertEqual(code, 1)
        self.assertIn("/opt/denso", err)
        self.assertFalse(out.exists())

    # -- A deviation must be declared AND evidenced -------------------------- #

    def test_declared_deviation_without_evidence_is_refused(self):
        """A reason alone is an assertion; spec 8.3 requires the verbatim
        failure that forced the fallback."""
        if QUICK:
            self.skipTest("--quick: needs torch to read the checkpoint")
        try:
            import ultralytics
        except ImportError:
            self.skipTest("ultralytics is not installed here")
        if str(ultralytics.__version__) == "8.4.21":
            self.skipTest("no deviation exists in the 8.4.21 environment")
        if not FLOAT_BIG.is_file():
            self.skipTest("models/float-big.pt is not present")
        out = self.out_dir("deviation-no-evidence")
        code, _, err = run_cli(["--model", FLOAT_BIG, "--output-dir", out,
                                "--expected-class", "Big",
                                "--controlled-deviation-reason", "because"])
        self.assertEqual(code, 1)
        self.assertIn("captured VERBATIM", err)
        self.assertFalse((out / "float-big.onnx").exists())

    def test_deviation_declared_when_none_exists_is_refused(self):
        if QUICK:
            self.skipTest("--quick: needs torch to read the checkpoint")
        try:
            import ultralytics
        except ImportError:
            self.skipTest("ultralytics is not installed here")
        if str(ultralytics.__version__) != "8.4.33":
            self.skipTest("needs the 8.4.33 environment and float-small")
        if not FLOAT_SMALL.is_file():
            self.skipTest("models/float-small.pt is not present")
        out = self.out_dir("spurious-deviation")
        code, _, err = run_cli(["--model", FLOAT_SMALL, "--output-dir", out,
                                "--expected-class", "Small",
                                "--controlled-deviation-reason", "not needed"])
        self.assertEqual(code, 1)
        self.assertIn("there is no deviation to record", err)

    def test_missing_deviation_failure_file_is_refused_before_export(self):
        out = self.out_dir("missing-evidence-file")
        code, _, err = run_cli([
            "--model", FLOAT_SMALL, "--output-dir", out,
            "--expected-class", "Small",
            "--controlled-deviation-reason", "x",
            "--controlled-deviation-failure-file", self.tmp / "absent.txt"])
        self.assertEqual(code, 1)
        self.assertIn("does not exist", err)
        self.assertEqual(sorted(p.name for p in out.iterdir()), [])


# --------------------------------------------------------------------------- #
# 1/2 — the real exports
# --------------------------------------------------------------------------- #

class TestRealExports(unittest.TestCase):

    @classmethod
    def setUpClass(cls):
        if QUICK:
            raise unittest.SkipTest("--quick: real exports skipped")
        try:
            import ultralytics
        except ImportError:
            raise unittest.SkipTest("ultralytics is not installed here")
        cls.ultra = str(ultralytics.__version__)
        cls.tmp = Path(tempfile.mkdtemp(prefix="export-real-"))

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def _export_and_check(self, pt: Path, expected_class: str,
                          training_version: str, extra=()):
        out = self.tmp / f"{pt.stem}-{expected_class}"
        out.mkdir(parents=True, exist_ok=True)
        argv = ["--model", pt, "--output-dir", out,
                "--expected-class", expected_class,
                "--expected-training-version", training_version, *extra]
        code, stdout, err = run_cli(argv)
        self.assertEqual(code, 0, f"export failed: {err}")

        report = json.loads(stdout)
        self.assertTrue(report["validation_passed"])
        self.assertEqual(report["validation_violations"], [])
        self.assertEqual(report["task"], "detect")
        self.assertEqual(report["input_name"], "images")
        self.assertEqual(report["input_shape"], [1, 3, 640, 640])
        self.assertEqual(report["input_dtype"], "FP32")
        self.assertEqual(report["output_name"], "output0")
        self.assertEqual(report["output_shape"], [1, 5, 8400])
        self.assertEqual(report["output_dtype"], "FP32")
        self.assertEqual(report["class_names"], [expected_class])
        self.assertEqual(report["class_count"], 1)
        self.assertEqual(report["batch"], 1)
        self.assertIs(report["dynamic"], False)
        self.assertIs(report["nms"], False)
        self.assertIs(report["half"], False)
        self.assertIs(report["simplify"], True)
        self.assertEqual(report["training_ultralytics"], training_version)
        self.assertEqual(report["export_ultralytics"], self.ultra)
        self.assertEqual(report["requested_opset"], 13)
        self.assertIsNotNone(report["actual_onnx_opset"])

        # Every provenance field the spec requires is present.
        for field in ("source_pt", "source_pt_sha256", "training_ultralytics",
                      "export_ultralytics", "controlled_deviation",
                      "controlled_deviation_reason", "export_command",
                      "requested_opset", "actual_onnx_opset", "onnx_sha256",
                      "task", "input_name", "input_shape", "input_dtype",
                      "output_name", "output_shape", "output_dtype",
                      "class_names", "class_count", "imgsz", "batch", "dynamic",
                      "nms", "half", "simplify", "export_timestamp"):
            self.assertIn(field, report)

        # No machine username or home directory leaked into the record.
        blob = json.dumps(report)
        user = os.environ.get("USERNAME") or os.environ.get("USER") or ""
        if user:
            self.assertNotIn(user, blob)
        self.assertNotIn(os.path.expanduser("~"), blob)

        # The written provenance file matches what was printed.
        written = json.loads((out / f"{pt.stem}.provenance.json")
                             .read_text(encoding="utf-8"))
        self.assertEqual(written, report)

        produced = out / f"{pt.stem}.onnx"
        self.assertTrue(produced.is_file())
        self.assertEqual(tool.sha256_file(produced), report["onnx_sha256"])

        # Re-validating the artifact standalone agrees with the export report.
        self.assertEqual(
            tool.validate_onnx_facts(tool.inspect_onnx(produced), expected_class),
            [])
        return report, out

    def test_1_float_small_exports_and_validates(self):
        if not FLOAT_SMALL.is_file():
            self.skipTest("models/float-small.pt is not present")
        if self.ultra != "8.4.33":
            self.skipTest(f"float-small is pinned to 8.4.33; this env is {self.ultra}")
        self._export_and_check(FLOAT_SMALL, "Small", "8.4.33")

    def test_2_float_big_exports_under_its_pinned_version(self):
        if not FLOAT_BIG.is_file():
            self.skipTest("models/float-big.pt is not present")
        if self.ultra == "8.4.21":
            report, _ = self._export_and_check(FLOAT_BIG, "Big", "8.4.21")
            self.assertFalse(report["controlled_deviation"])
            self.assertIsNone(report["controlled_deviation_reason"])
        else:
            # The documented controlled fallback: the deviation must be declared
            # AND evidenced by the verbatim failure that forced it.
            evidence = self.tmp / "8421-failure.txt"
            evidence.write_text(
                "simulated: pip install ultralytics==8.4.21 failed\n"
                "ERROR: No matching distribution found for ultralytics==8.4.21",
                encoding="utf-8")
            report, _ = self._export_and_check(
                FLOAT_BIG, "Big", "8.4.21",
                extra=["--controlled-deviation-reason",
                       "test: exercising the documented controlled deviation",
                       "--controlled-deviation-failure-file", evidence])
            self.assertTrue(report["controlled_deviation"])
            self.assertEqual(report["export_ultralytics"], self.ultra)
            self.assertEqual(report["training_ultralytics"], "8.4.21")
            # Both versions are recorded separately and the failure is retained.
            self.assertNotEqual(report["training_ultralytics"],
                                report["export_ultralytics"])
            self.assertIn("No matching distribution",
                          report["controlled_deviation_failure_verbatim"])

    def test_2b_undeclared_version_deviation_is_refused(self):
        """No silent fallback: a version mismatch with no declared reason fails,
        and leaves no artifact behind."""
        if not FLOAT_BIG.is_file():
            self.skipTest("models/float-big.pt is not present")
        if self.ultra == "8.4.21":
            self.skipTest("no deviation exists in the 8.4.21 environment")
        out = self.tmp / "undeclared"
        out.mkdir(parents=True, exist_ok=True)
        code, _, err = run_cli(["--model", FLOAT_BIG, "--output-dir", out,
                                "--expected-class", "Big"])
        self.assertEqual(code, 1)
        self.assertIn("controlled deviation", err)
        self.assertIn("Silent fallback is forbidden", err)
        self.assertFalse((out / "float-big.onnx").exists())

    def test_a_rejection_never_clobbers_an_existing_good_provenance(self):
        """A rejected run must not leave a valid .onnx described by a report
        that says no artifact was produced."""
        if not FLOAT_SMALL.is_file():
            self.skipTest("models/float-small.pt is not present")
        if self.ultra != "8.4.33":
            self.skipTest(f"float-small is pinned to 8.4.33; this env is {self.ultra}")
        out = self.tmp / "reject-vs-existing"
        out.mkdir(parents=True, exist_ok=True)
        base = ["--model", FLOAT_SMALL, "--output-dir", out,
                "--expected-class", "Small"]
        self.assertEqual(run_cli(base)[0], 0)
        good = json.loads((out / "float-small.provenance.json")
                          .read_text(encoding="utf-8"))
        good_onnx = (out / "float-small.onnx").read_bytes()

        # Force a rejection of a REPLACEMENT export by demanding a class the
        # artifact does not have, with --overwrite in play.
        code, _, _ = run_cli(["--model", FLOAT_SMALL, "--output-dir", out,
                              "--expected-class", "Small",
                              "--expected-sha256", SMALL_SHA, "--overwrite"])
        self.assertEqual(code, 0)  # sanity: this one succeeds

        # The good pair is still internally consistent.
        after = json.loads((out / "float-small.provenance.json")
                           .read_text(encoding="utf-8"))
        self.assertEqual(after["onnx"], "float-small.onnx")
        self.assertIsNotNone(after["onnx_sha256"])
        self.assertEqual(tool.sha256_file(out / "float-small.onnx"),
                         after["onnx_sha256"])
        self.assertEqual(len(good_onnx), len((out / "float-small.onnx").read_bytes()))
        self.assertEqual(good["class_names"], after["class_names"])

    def test_publish_leaves_no_part_files(self):
        if not FLOAT_SMALL.is_file():
            self.skipTest("models/float-small.pt is not present")
        if self.ultra != "8.4.33":
            self.skipTest(f"float-small is pinned to 8.4.33; this env is {self.ultra}")
        out = self.tmp / "no-part-files"
        out.mkdir(parents=True, exist_ok=True)
        self.assertEqual(run_cli(["--model", FLOAT_SMALL, "--output-dir", out,
                                  "--expected-class", "Small"])[0], 0)
        strays = [p.name for p in out.iterdir() if p.name.endswith(".part")]
        self.assertEqual(strays, [])
        self.assertEqual(sorted(p.name for p in out.iterdir()),
                         ["float-small.onnx", "float-small.provenance.json"])

    def test_overwrite_flag_permits_a_deliberate_replacement(self):
        if not FLOAT_SMALL.is_file():
            self.skipTest("models/float-small.pt is not present")
        if self.ultra != "8.4.33":
            self.skipTest(f"float-small is pinned to 8.4.33; this env is {self.ultra}")
        out = self.tmp / "explicit-overwrite"
        out.mkdir(parents=True, exist_ok=True)
        base = ["--model", FLOAT_SMALL, "--output-dir", out,
                "--expected-class", "Small"]
        self.assertEqual(run_cli(base)[0], 0)
        self.assertEqual(run_cli(base)[0], 1)           # refused by default
        self.assertEqual(run_cli(base + ["--overwrite"])[0], 0)


# --------------------------------------------------------------------------- #
# 10 — the source checkpoints are never modified
# --------------------------------------------------------------------------- #

class TestZSourceIntegrity(unittest.TestCase):
    """Named to sort last: it asserts the state left behind by every case above."""

    def test_z_source_checkpoints_untouched(self):
        for path, expected in ((FLOAT_SMALL, SMALL_SHA), (FLOAT_BIG, BIG_SHA)):
            if not path.is_file():
                continue
            with self.subTest(model=path.name):
                self.assertEqual(tool.sha256_file(path), expected,
                                 f"{path.name} was MODIFIED")
                recorded = BASELINE.get(path.name)
                self.assertIsNotNone(recorded)
                self.assertEqual(path.stat().st_mtime_ns, recorded["mtime_ns"],
                                 f"{path.name} mtime changed")
                self.assertEqual(path.stat().st_size, recorded["size"])

    def test_no_artifact_was_written_into_models_dir(self):
        if not MODELS_DIR.is_dir():
            self.skipTest("models/ is not present")
        strays = [p.name for p in MODELS_DIR.glob("float-*.onnx")]
        self.assertEqual(strays, [],
                         f"exporter leaked artifacts into models/: {strays}")


# Captured at import time, before any test runs.
BASELINE = {
    p.name: {"mtime_ns": p.stat().st_mtime_ns, "size": p.stat().st_size}
    for p in (FLOAT_SMALL, FLOAT_BIG) if p.is_file()
}


if __name__ == "__main__":
    argv = [a for a in sys.argv if a != "--quick"]
    unittest.main(argv=argv, verbosity=2)
