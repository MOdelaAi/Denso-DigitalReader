// Slice 1 — manifest schema 2: declared model identity + per-backend runtime
// artifacts + schema-aware accessors.
//
// Scope discipline: this file tests PARSING, VALIDATION and ACCESS only. There is
// deliberately no notion of a mode, a family->mode matrix, or an authorization
// verdict anywhere in it — that is Slice 6's compatibility policy, and Slice 1
// must not grow one by accident.
#include <catch2/catch_test_macros.hpp>

#include "detection/detection.h"
#include "models/compatibility.h"
#include "models/hashing.h"
#include "models/manifest.h"
#include "models/model_identity.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <string>

using denso::models::find_by_engine;
using denso::models::parse_manifest;
using denso::models::validate_manifest;

namespace {

// A 64-char lowercase-hex-looking digest built from one repeated char, so each
// artifact in a fixture gets a visibly distinct hash.
std::string sha(char c) { return std::string(64, c); }

std::string default_provenance() {
    return std::string(R"("provenance":{)") +
           R"("source_pt":"digitv3.pt","source_pt_sha256":")" + sha('d') + R"(",)" +
           R"("onnx":"digitv3.onnx","onnx_sha256":")" + sha('a') + R"(","onnx_opset":12,)" +
           R"("training_ultralytics":"8.4.33","export_ultralytics":"8.4.33",)" +
           R"("batch":1,"dynamic":false,"nms":false,)" +
           R"("precision":"fp16","jetpack":"6.2",)" +
           R"("export_onnx_command":"yolo export","export_engine_command":"trtexec")" +
           R"(})";
}

// Builder for a schema-2 manifest with exactly one generation. Every field a test
// needs to corrupt is a member, so the negative cases differ from the positive one
// by a single assignment.
struct Gen2 {
    std::string schema = "2";
    std::string name = "digitv3";
    std::string canonical_id = "digitv3";
    std::string family = "digit_numeric";
    std::string task = "detect";
    std::string input_size = "640";
    std::string class_names = R"(["0","1"])";
    std::string class_count = "2";

    bool with_ort = true;
    std::string ort_model = "digitv3.onnx";
    std::string ort_sha = sha('a');
    std::string ort_source = "onnx_metadata_names";

    bool with_trt = true;
    std::string trt_engine = "digitv3.engine";
    std::string trt_engine_sha = sha('b');
    std::string trt_sidecar = "digitv3.names.json";
    std::string trt_sidecar_sha = sha('c');
    std::string trt_source = "names_sidecar";
    std::string built_for = R"({"trt":"10.3","cuda":"12.6","sm":"87"})";

    bool with_provenance = true;
    std::string provenance = default_provenance();

    // Raw injection points for structural cases the typed members cannot express.
    std::string runtime_override;  // replaces the whole "runtime" value
    std::string extra_root_keys;   // e.g. R"("allowed_modes":["digit_reader"],)"

    std::string runtime() const {
        if (!runtime_override.empty()) return runtime_override;
        std::string r = "{";
        bool first = true;
        if (with_ort) {
            r += std::string(R"("onnxruntime":{"model":")") + ort_model +
                 R"(","model_sha256":")" + ort_sha +
                 R"(","class_metadata_source":")" + ort_source + R"("})";
            first = false;
        }
        if (with_trt) {
            if (!first) r += ",";
            r += std::string(R"("tensorrt":{"engine":")") + trt_engine +
                 R"(","engine_sha256":")" + trt_engine_sha +
                 R"(","sidecar":")" + trt_sidecar +
                 R"(","sidecar_sha256":")" + trt_sidecar_sha +
                 R"(","class_metadata_source":")" + trt_source +
                 R"(","built_for":)" + built_for + "}";
        }
        return r + "}";
    }

    std::string json() const {
        std::string s = R"({"schema":)" + schema + R"(,"generations":[{)";
        s += extra_root_keys;
        s += R"("name":")" + name + R"(",)";
        s += R"("canonical_id":")" + canonical_id + R"(",)";
        s += R"("family":")" + family + R"(",)";
        s += R"("task":")" + task + R"(",)";
        s += R"("input_size":)" + input_size + ",";
        s += R"("class_names":)" + class_names + ",";
        s += R"("class_count":)" + class_count + ",";
        s += R"("runtime":)" + runtime() + ",";
        if (with_provenance) s += provenance + ",";
        s += R"("installed_utc":"2026-07-22T00:00:00Z","state":"installed"}]})";
        return s;
    }
};

// The schema-1 fixture, byte-for-byte the shape test_manifest.cpp already uses.
std::string schema1_json() {
    return std::string(R"({"schema":1,"generations":[{)") +
           R"("name":"digit-v3.1","engine":"digit-v3.1.engine","engine_sha256":")" + sha('b') + R"(",)" +
           R"("sidecar":"digit-v3.1.names.json","sidecar_sha256":")" + sha('c') + R"(",)" +
           R"("class_names":["0","1"],"built_for":{"trt":"10.3","cuda":"12.6","sm":"87"},)" +
           R"("installed_utc":"2026-07-20T00:00:00Z","state":"installed"}]})";
}

// Parse + validate in one step. True iff the manifest is ACCEPTED. Whether a
// given defect is caught structurally (parse) or semantically (validate) is an
// implementation detail; every caller in the tree treats either as ManifestCorrupt.
bool accepted(const std::string& json) {
    auto r = parse_manifest(json);
    if (!r.manifest.has_value()) return false;
    return !validate_manifest(*r.manifest).has_value();
}

}  // namespace

// ─── 1. schema 1 still parses, and is NOT declared ───────────────────────────

TEST_CASE("schema 1 parses, validates, and is undeclared", "[model_identity]") {
    auto r = parse_manifest(schema1_json());
    REQUIRE(r.error.empty());
    REQUIRE(r.manifest.has_value());
    REQUIRE(r.manifest->schema == 1);
    REQUIRE(r.manifest->generations.size() == 1);
    const auto& g = r.manifest->generations[0];
    REQUIRE_FALSE(g.declared);
    REQUIRE(g.canonical_id.empty());
    REQUIRE(g.family.empty());
    REQUIRE_FALSE(validate_manifest(*r.manifest).has_value());
}

// ─── 2-4. schema 2 accepts both blocks, ORT-only, and TRT-only ───────────────

TEST_CASE("schema 2 with both runtime blocks is accepted and declared",
          "[model_identity]") {
    Gen2 g;
    auto r = parse_manifest(g.json());
    REQUIRE(r.error.empty());
    REQUIRE(r.manifest.has_value());
    REQUIRE(r.manifest->schema == 2);
    const auto& gen = r.manifest->generations.at(0);
    REQUIRE(gen.declared);
    REQUIRE(gen.canonical_id == "digitv3");
    REQUIRE(gen.family == "digit_numeric");
    REQUIRE(gen.task == "detect");
    REQUIRE(gen.input_size == 640);
    REQUIRE(gen.class_count == 2);
    REQUIRE(gen.class_names == std::vector<std::string>{"0", "1"});
    REQUIRE(gen.runtime.onnxruntime.has_value());
    REQUIRE(gen.runtime.tensorrt.has_value());
    REQUIRE(gen.runtime.onnxruntime->model == "digitv3.onnx");
    REQUIRE(gen.runtime.tensorrt->engine == "digitv3.engine");
    REQUIRE(gen.provenance.export_ultralytics == "8.4.33");
    REQUIRE_FALSE(validate_manifest(*r.manifest).has_value());
}

TEST_CASE("schema 2 with only the onnxruntime block is accepted", "[model_identity]") {
    Gen2 g; g.with_trt = false;
    auto r = parse_manifest(g.json());
    REQUIRE(r.manifest.has_value());
    const auto& gen = r.manifest->generations.at(0);
    REQUIRE(gen.runtime.onnxruntime.has_value());
    REQUIRE_FALSE(gen.runtime.tensorrt.has_value());
    REQUIRE_FALSE(validate_manifest(*r.manifest).has_value());
}

TEST_CASE("schema 2 with only the tensorrt block is accepted", "[model_identity]") {
    Gen2 g; g.with_ort = false;
    auto r = parse_manifest(g.json());
    REQUIRE(r.manifest.has_value());
    const auto& gen = r.manifest->generations.at(0);
    REQUIRE_FALSE(gen.runtime.onnxruntime.has_value());
    REQUIRE(gen.runtime.tensorrt.has_value());
    REQUIRE_FALSE(validate_manifest(*r.manifest).has_value());
}

// ─── 5. a runtime block is mandatory ─────────────────────────────────────────

TEST_CASE("schema 2 with neither runtime block is rejected", "[model_identity]") {
    Gen2 g; g.with_ort = false; g.with_trt = false;
    REQUIRE_FALSE(accepted(g.json()));
}

TEST_CASE("schema 2 with an empty runtime object is rejected", "[model_identity]") {
    Gen2 g; g.runtime_override = "{}";
    REQUIRE_FALSE(accepted(g.json()));
}

TEST_CASE("schema 2 with a missing runtime key is rejected", "[model_identity]") {
    Gen2 g;
    std::string j = g.json();
    // Drop the whole "runtime":{...}, member by rebuilding without it.
    const std::string needle = R"("runtime":)" + g.runtime() + ",";
    const auto pos = j.find(needle);
    REQUIRE(pos != std::string::npos);
    j.erase(pos, needle.size());
    REQUIRE_FALSE(accepted(j));
}

TEST_CASE("schema 2 with a non-object runtime is rejected", "[model_identity]") {
    Gen2 g; g.runtime_override = R"("tensorrt")";
    REQUIRE_FALSE(accepted(g.json()));
}

// ─── 6. no silent third backend ──────────────────────────────────────────────

TEST_CASE("schema 2 with an unknown runtime backend key is rejected",
          "[model_identity]") {
    Gen2 g;
    g.runtime_override = std::string(R"({"openvino":{"model":"x.xml"},)") +
                         R"("tensorrt":{"engine":"digitv3.engine","engine_sha256":")" + sha('b') +
                         R"(","sidecar":"digitv3.names.json","sidecar_sha256":")" + sha('c') +
                         R"(","class_metadata_source":"names_sidecar",)" +
                         R"("built_for":{"trt":"10.3","cuda":"12.6","sm":"87"}}})";
    REQUIRE_FALSE(accepted(g.json()));
}

// ─── 7-8. the metadata-source vocabulary is CLOSED, and backend-specific ─────

TEST_CASE("a misspelled class_metadata_source is rejected", "[model_identity]") {
    Gen2 g; g.trt_source = "names_sidcar";   // one transposed letter
    REQUIRE_FALSE(accepted(g.json()));
}

TEST_CASE("an empty class_metadata_source is rejected", "[model_identity]") {
    Gen2 g; g.ort_source = "";
    REQUIRE_FALSE(accepted(g.json()));
}

TEST_CASE("the onnxruntime block may not declare the sidecar metadata source",
          "[model_identity]") {
    Gen2 g; g.ort_source = "names_sidecar";
    REQUIRE_FALSE(accepted(g.json()));
}

TEST_CASE("the tensorrt block may not declare the onnx metadata source",
          "[model_identity]") {
    Gen2 g; g.trt_source = "onnx_metadata_names";
    REQUIRE_FALSE(accepted(g.json()));
}

// ─── 9-10. built_for is TensorRT-local, and complete ─────────────────────────

TEST_CASE("schema 2 with built_for at the generation root is rejected",
          "[model_identity]") {
    // A root copy would resurrect the cross-platform rejection that moving
    // built_for into runtime.tensorrt exists to make structurally impossible.
    Gen2 g;
    g.extra_root_keys = R"("built_for":{"trt":"10.3","cuda":"12.6","sm":"87"},)";
    REQUIRE_FALSE(accepted(g.json()));
}

TEST_CASE("schema 2 rejects a tensorrt block missing built_for trt",
          "[model_identity]") {
    Gen2 g; g.built_for = R"({"cuda":"12.6","sm":"87"})";
    REQUIRE_FALSE(accepted(g.json()));
}

TEST_CASE("schema 2 rejects a tensorrt block missing built_for cuda",
          "[model_identity]") {
    Gen2 g; g.built_for = R"({"trt":"10.3","sm":"87"})";
    REQUIRE_FALSE(accepted(g.json()));
}

TEST_CASE("schema 2 rejects a tensorrt block missing built_for sm",
          "[model_identity]") {
    Gen2 g; g.built_for = R"({"trt":"10.3","cuda":"12.6"})";
    REQUIRE_FALSE(accepted(g.json()));
}

TEST_CASE("schema 2 rejects a blank built_for value", "[model_identity]") {
    Gen2 g; g.built_for = R"({"trt":"","cuda":"12.6","sm":"87"})";
    REQUIRE_FALSE(accepted(g.json()));
}

// ─── 11-12. canonical_id is a safe, unique token ─────────────────────────────

TEST_CASE("schema 2 rejects an empty canonical_id", "[model_identity]") {
    Gen2 g; g.canonical_id = "";
    REQUIRE_FALSE(accepted(g.json()));
}

TEST_CASE("schema 2 rejects a canonical_id with a path separator",
          "[model_identity]") {
    Gen2 g; g.canonical_id = "evil/id";
    REQUIRE_FALSE(accepted(g.json()));
}

TEST_CASE("schema 2 rejects a canonical_id with parent traversal",
          "[model_identity]") {
    Gen2 g; g.canonical_id = "..";
    REQUIRE_FALSE(accepted(g.json()));
}

TEST_CASE("schema 2 rejects a canonical_id with a space", "[model_identity]") {
    Gen2 g; g.canonical_id = "digit v3";
    REQUIRE_FALSE(accepted(g.json()));
}

TEST_CASE("schema 2 rejects duplicate canonical ids", "[model_identity]") {
    Gen2 g;
    auto r = parse_manifest(g.json());
    REQUIRE(r.manifest.has_value());
    auto dup = r.manifest->generations.at(0);
    dup.name = "digitv3-b";                       // distinct name
    dup.runtime.tensorrt->engine = "other.engine";  // distinct engine
    dup.runtime.tensorrt->sidecar = "other.names.json";
    dup.runtime.onnxruntime->model = "other.onnx";
    r.manifest->generations.push_back(dup);       // same canonical_id
    REQUIRE(validate_manifest(*r.manifest).has_value());
}

// ─── 13. class_count is a checksum on class_names ────────────────────────────

TEST_CASE("schema 2 rejects a class_count that disagrees with class_names",
          "[model_identity]") {
    Gen2 g; g.class_count = "3";   // class_names has 2
    REQUIRE_FALSE(accepted(g.json()));
}

TEST_CASE("schema 2 rejects a zero class_count", "[model_identity]") {
    Gen2 g; g.class_names = "[]"; g.class_count = "0";
    REQUIRE_FALSE(accepted(g.json()));
}

TEST_CASE("schema 2 rejects a non-positive input_size", "[model_identity]") {
    Gen2 g; g.input_size = "0";
    REQUIRE_FALSE(accepted(g.json()));
    Gen2 h; h.input_size = "-640";
    REQUIRE_FALSE(accepted(h.json()));
}

TEST_CASE("schema 2 rejects an empty family or task", "[model_identity]") {
    Gen2 g; g.family = "";
    REQUIRE_FALSE(accepted(g.json()));
    Gen2 h; h.task = "";
    REQUIRE_FALSE(accepted(h.json()));
}

TEST_CASE("schema 2 rejects an unsafe family token", "[model_identity]") {
    Gen2 g; g.family = "digit/numeric";
    REQUIRE_FALSE(accepted(g.json()));
}

// ─── 14. every digest is strictly 64 lowercase hex ───────────────────────────

TEST_CASE("schema 2 rejects a short engine sha256", "[model_identity]") {
    Gen2 g; g.trt_engine_sha = "aa";
    REQUIRE_FALSE(accepted(g.json()));
}

TEST_CASE("schema 2 rejects an uppercase-hex sidecar sha256", "[model_identity]") {
    Gen2 g; g.trt_sidecar_sha = std::string(64, 'A');
    REQUIRE_FALSE(accepted(g.json()));
}

TEST_CASE("schema 2 rejects a non-hex onnx model sha256", "[model_identity]") {
    Gen2 g; g.ort_sha = std::string(64, 'z');
    REQUIRE_FALSE(accepted(g.json()));
}

// ─── runtime filenames are safe basenames with matching stems ────────────────

TEST_CASE("schema 2 rejects an unsafe tensorrt engine basename",
          "[model_identity]") {
    Gen2 g; g.trt_engine = "../evil.engine"; g.trt_sidecar = "../evil.names.json";
    REQUIRE_FALSE(accepted(g.json()));
}

TEST_CASE("schema 2 rejects mismatched tensorrt engine and sidecar stems",
          "[model_identity]") {
    Gen2 g; g.trt_sidecar = "other-stem.names.json";
    REQUIRE_FALSE(accepted(g.json()));
}

TEST_CASE("schema 2 rejects an onnxruntime model that is not a dot-onnx file",
          "[model_identity]") {
    Gen2 g; g.ort_model = "digitv3.bin";
    REQUIRE_FALSE(accepted(g.json()));
}

TEST_CASE("schema 2 rejects an unsafe onnxruntime model basename",
          "[model_identity]") {
    Gen2 g; g.ort_model = "../evil.onnx";
    REQUIRE_FALSE(accepted(g.json()));
}

// ─── 15. the schema value itself ─────────────────────────────────────────────

TEST_CASE("parse_manifest rejects schema 0", "[model_identity]") {
    Gen2 g; g.schema = "0";
    REQUIRE_FALSE(parse_manifest(g.json()).manifest.has_value());
}

TEST_CASE("parse_manifest rejects schema 3", "[model_identity]") {
    Gen2 g; g.schema = "3";
    REQUIRE_FALSE(parse_manifest(g.json()).manifest.has_value());
}

TEST_CASE("parse_manifest rejects a string schema", "[model_identity]") {
    Gen2 g; g.schema = R"("2")";
    REQUIRE_FALSE(parse_manifest(g.json()).manifest.has_value());
}

TEST_CASE("parse_manifest rejects a fractional schema", "[model_identity]") {
    Gen2 g; g.schema = "1.5";
    REQUIRE_FALSE(parse_manifest(g.json()).manifest.has_value());
}

// ─── 16. provenance is required, and its key fields are non-empty ────────────

TEST_CASE("schema 2 rejects a missing provenance object", "[model_identity]") {
    Gen2 g; g.with_provenance = false;
    REQUIRE_FALSE(accepted(g.json()));
}

// ENGINE-ONLY ARTIFACT POLICY. The production artifact pair is
// <model-id>.engine + <model-id>.names.json and the approved ENGINE BYTES are
// the provenance authority. A .onnx / .pt is not required, not packaged and
// never consulted, so a manifest must NOT be refused for omitting the source
// chain. These three cases previously asserted the opposite; they are inverted
// deliberately, and the runtime authorization they might be mistaken for is
// pinned separately below.
TEST_CASE("engine-only: a blank provenance source_pt_sha256 is accepted",
          "[model_identity][engine_only]") {
    Gen2 g;
    auto r = parse_manifest(g.json());
    REQUIRE(r.manifest.has_value());
    r.manifest->generations.at(0).provenance.source_pt_sha256.clear();
    REQUIRE_FALSE(validate_manifest(*r.manifest).has_value());
}

TEST_CASE("engine-only: a blank provenance onnx_sha256 is accepted",
          "[model_identity][engine_only]") {
    Gen2 g;
    auto r = parse_manifest(g.json());
    REQUIRE(r.manifest.has_value());
    r.manifest->generations.at(0).provenance.onnx_sha256.clear();
    REQUIRE_FALSE(validate_manifest(*r.manifest).has_value());
}

TEST_CASE("engine-only: a blank provenance export_ultralytics is accepted",
          "[model_identity][engine_only]") {
    Gen2 g;
    auto r = parse_manifest(g.json());
    REQUIRE(r.manifest.has_value());
    r.manifest->generations.at(0).provenance.export_ultralytics.clear();
    REQUIRE_FALSE(validate_manifest(*r.manifest).has_value());
}

TEST_CASE("engine-only: a manifest carrying NO source chain at all validates",
          "[model_identity][engine_only]") {
    // The shape tools/gen_model_manifest.py now emits: engine + sidecar hashes,
    // built_for, and precision. Nothing about a checkpoint, an export or an ONNX.
    Gen2 g;
    auto r = parse_manifest(g.json());
    REQUIRE(r.manifest.has_value());
    auto& p = r.manifest->generations.at(0).provenance;
    p.source_pt.clear();
    p.source_pt_sha256.clear();
    p.onnx.clear();
    p.onnx_sha256.clear();
    p.onnx_opset = 0;
    p.training_ultralytics.clear();
    p.export_ultralytics.clear();
    p.export_onnx_command.clear();
    p.export_engine_command.clear();
    REQUIRE_FALSE(validate_manifest(*r.manifest).has_value());
}

TEST_CASE("engine-only: hash enforcement is NOT weakened by the relaxation",
          "[model_identity][engine_only]") {
    // The whole point of the relaxation is that it touches what a manifest must
    // DECLARE, never what an artifact must PROVE. The engine and sidecar digests
    // remain strict 64-lowercase-hex and a generation is still refused without
    // them — otherwise dropping the source chain would have opened a hole.
    Gen2 g;
    auto r = parse_manifest(g.json());
    REQUIRE(r.manifest.has_value());
    const std::string good = r.manifest->generations.at(0).runtime.tensorrt->engine_sha256;
    REQUIRE(good.size() == 64);

    r.manifest->generations.at(0).runtime.tensorrt->engine_sha256.clear();
    REQUIRE(validate_manifest(*r.manifest).has_value());

    r.manifest->generations.at(0).runtime.tensorrt->engine_sha256 = "NOTHEX";
    REQUIRE(validate_manifest(*r.manifest).has_value());

    r.manifest->generations.at(0).runtime.tensorrt->engine_sha256 = good;
    r.manifest->generations.at(0).runtime.tensorrt->sidecar_sha256.clear();
    REQUIRE(validate_manifest(*r.manifest).has_value());
}

TEST_CASE("schema 2 rejects a blank provenance precision", "[model_identity]") {
    Gen2 g;
    auto r = parse_manifest(g.json());
    REQUIRE(r.manifest.has_value());
    r.manifest->generations.at(0).provenance.precision.clear();
    REQUIRE(validate_manifest(*r.manifest).has_value());
}

TEST_CASE("provenance digests are validated as non-empty, NOT as strict hex",
          "[model_identity]") {
    // DELIBERATE, per spec 3.5, which requires provenance's source_pt_sha256 and
    // onnx_sha256 to be non-empty and says nothing about their shape. They are
    // traceability metadata: no slice ever compares them against a file. The
    // hashes that ARE compared -- runtime.{onnxruntime.model_sha256,
    // tensorrt.engine_sha256, tensorrt.sidecar_sha256} -- are strict 64-char
    // lowercase hex, asserted above.
    //
    // Strictness here would not harden anything, and validate_manifest failure is
    // ManifestCorrupt -> Blocked, so an unspecified extra rule could brick an
    // appliance over a cosmetic field. Pinned so the looseness reads as a decision.
    Gen2 g;
    g.provenance = std::string(R"("provenance":{"source_pt":"digitv3.pt",)") +
                   R"("source_pt_sha256":"NOT-A-HASH","onnx":"digitv3.onnx",)" +
                   R"("onnx_sha256":"ALSO-NOT-A-HASH","onnx_opset":12,)" +
                   R"("training_ultralytics":"8.4.33","export_ultralytics":"8.4.33",)" +
                   R"("batch":1,"dynamic":false,"nms":false,)" +
                   R"("precision":"fp16","jetpack":"6.2",)" +
                   R"("export_onnx_command":"yolo export","export_engine_command":"trtexec"})";
    REQUIRE(accepted(g.json()));
}

TEST_CASE("engine-only: a blank provenance export_engine_command is accepted",
          "[model_identity][engine_only]") {
    // Also inverted by the engine-only policy. The canonical value of this field
    // is a trtexec recipe naming a .onnx, i.e. an export-source reference — which
    // a generated manifest may no longer carry at all. `precision` remains the
    // one required provenance field because it describes the PLAN, not a source.
    Gen2 g;
    auto r = parse_manifest(g.json());
    REQUIRE(r.manifest.has_value());
    r.manifest->generations.at(0).provenance.export_engine_command.clear();
    REQUIRE_FALSE(validate_manifest(*r.manifest).has_value());
}

TEST_CASE("engine-only: provenance.precision is still required",
          "[model_identity][engine_only]") {
    Gen2 g;
    auto r = parse_manifest(g.json());
    REQUIRE(r.manifest.has_value());
    r.manifest->generations.at(0).provenance.precision.clear();
    REQUIRE(validate_manifest(*r.manifest).has_value());
}

// ─── 17-18, 20. the schema-aware accessors ───────────────────────────────────

TEST_CASE("accessors return the ROOT fields for a schema 1 generation",
          "[model_identity]") {
    auto r = parse_manifest(schema1_json());
    REQUIRE(r.manifest.has_value());
    const auto& g = r.manifest->generations.at(0);
    REQUIRE(g.tensorrt_engine() == "digit-v3.1.engine");
    REQUIRE(g.tensorrt_sidecar() == "digit-v3.1.names.json");
    REQUIRE(g.tensorrt_engine_sha256() == sha('b'));
    REQUIRE(g.tensorrt_sidecar_sha256() == sha('c'));
    REQUIRE(g.built_for_trt() == "10.3");
    REQUIRE(g.built_for_cuda() == "12.6");
    REQUIRE(g.built_for_sm() == "87");
}

TEST_CASE("accessors return the NESTED tensorrt fields for a schema 2 generation",
          "[model_identity]") {
    Gen2 g;
    auto r = parse_manifest(g.json());
    REQUIRE(r.manifest.has_value());
    const auto& gen = r.manifest->generations.at(0);
    REQUIRE(gen.tensorrt_engine() == "digitv3.engine");
    REQUIRE(gen.tensorrt_sidecar() == "digitv3.names.json");
    REQUIRE(gen.tensorrt_engine_sha256() == sha('b'));
    REQUIRE(gen.tensorrt_sidecar_sha256() == sha('c'));
    REQUIRE(gen.built_for_trt() == "10.3");
    REQUIRE(gen.built_for_cuda() == "12.6");
    REQUIRE(gen.built_for_sm() == "87");
    // The root fields stay empty for schema 2 — nothing may read them directly.
    REQUIRE(gen.engine.empty());
    REQUIRE(gen.sidecar.empty());
}

TEST_CASE("a schema 2 onnx-only generation exposes no tensorrt accessor values",
          "[model_identity]") {
    Gen2 g; g.with_trt = false;
    auto r = parse_manifest(g.json());
    REQUIRE(r.manifest.has_value());
    const auto& gen = r.manifest->generations.at(0);
    REQUIRE(gen.tensorrt_engine().empty());
    REQUIRE(gen.tensorrt_sidecar().empty());
    REQUIRE(gen.tensorrt_engine_sha256().empty());
    REQUIRE(gen.tensorrt_sidecar_sha256().empty());
    REQUIRE(gen.built_for_trt().empty());
    REQUIRE(gen.built_for_cuda().empty());
    REQUIRE(gen.built_for_sm().empty());
}

// ─── 19. find_by_engine across both schemas ──────────────────────────────────

TEST_CASE("find_by_engine resolves a schema 1 generation", "[model_identity]") {
    auto r = parse_manifest(schema1_json());
    REQUIRE(r.manifest.has_value());
    REQUIRE(find_by_engine(*r.manifest, "digit-v3.1.engine") != nullptr);
    REQUIRE(find_by_engine(*r.manifest, "nope.engine") == nullptr);
}

TEST_CASE("find_by_engine resolves a schema 2 generation by its tensorrt engine",
          "[model_identity]") {
    Gen2 g;
    auto r = parse_manifest(g.json());
    REQUIRE(r.manifest.has_value());
    const auto* found = find_by_engine(*r.manifest, "digitv3.engine");
    REQUIRE(found != nullptr);
    REQUIRE(found->canonical_id == "digitv3");
}

TEST_CASE("find_by_engine never matches the onnx filename", "[model_identity]") {
    Gen2 g;
    auto r = parse_manifest(g.json());
    REQUIRE(r.manifest.has_value());
    REQUIRE(find_by_engine(*r.manifest, "digitv3.onnx") == nullptr);
}

TEST_CASE("find_by_engine does not match an onnx-only generation on empty input",
          "[model_identity]") {
    // Guards the degenerate case: an ORT-only generation has no tensorrt engine,
    // so an empty query must not resolve to it.
    Gen2 g; g.with_trt = false;
    auto r = parse_manifest(g.json());
    REQUIRE(r.manifest.has_value());
    REQUIRE(find_by_engine(*r.manifest, "") == nullptr);
    REQUIRE(find_by_engine(*r.manifest, "digitv3.engine") == nullptr);
}

// ─── a stray allowed_modes is INERT, never an authority ──────────────────────

TEST_CASE("a stray allowed_modes key is ignored, not honoured", "[model_identity]") {
    Gen2 g;
    g.extra_root_keys = R"("allowed_modes":["digit_reader","ball_leveler"],)";
    auto r = parse_manifest(g.json());
    REQUIRE(r.manifest.has_value());
    REQUIRE_FALSE(validate_manifest(*r.manifest).has_value());
    // It parses and validates — and leaves no trace. There is no field for it to
    // land in, so it cannot become a second compatibility authority.
    const auto& gen = r.manifest->generations.at(0);
    REQUIRE(gen.canonical_id == "digitv3");
    REQUIRE(gen.family == "digit_numeric");
}

// ─── declared runtime filenames — the integrity manifested-set input ─────────

TEST_CASE("declared_runtime_files lists the root engine for schema 1",
          "[model_identity]") {
    auto r = parse_manifest(schema1_json());
    REQUIRE(r.manifest.has_value());
    const auto files = r.manifest->generations.at(0).declared_runtime_files();
    REQUIRE(files == std::vector<std::string>{"digit-v3.1.engine"});
}

TEST_CASE("declared_runtime_files lists both backend artifacts for schema 2",
          "[model_identity]") {
    Gen2 g;
    auto r = parse_manifest(g.json());
    REQUIRE(r.manifest.has_value());
    const auto files = r.manifest->generations.at(0).declared_runtime_files();
    REQUIRE(files.size() == 2);
    bool has_onnx = false, has_engine = false;
    for (const auto& f : files) {
        if (f == "digitv3.onnx") has_onnx = true;
        if (f == "digitv3.engine") has_engine = true;
    }
    REQUIRE(has_onnx);
    REQUIRE(has_engine);
}

TEST_CASE("declared_runtime_files lists only the present block", "[model_identity]") {
    Gen2 onnx_only; onnx_only.with_trt = false;
    auto a = parse_manifest(onnx_only.json());
    REQUIRE(a.manifest.has_value());
    REQUIRE(a.manifest->generations.at(0).declared_runtime_files() ==
            std::vector<std::string>{"digitv3.onnx"});

    Gen2 trt_only; trt_only.with_ort = false;
    auto b = parse_manifest(trt_only.json());
    REQUIRE(b.manifest.has_value());
    REQUIRE(b.manifest->generations.at(0).declared_runtime_files() ==
            std::vector<std::string>{"digitv3.engine"});
}

// ─── carried-over schema-1 semantics that must hold for schema 2 too ─────────

TEST_CASE("schema 2 rejects state other than installed", "[model_identity]") {
    Gen2 g;
    auto r = parse_manifest(g.json());
    REQUIRE(r.manifest.has_value());
    r.manifest->generations.at(0).state = "staged";
    REQUIRE(validate_manifest(*r.manifest).has_value());
}

TEST_CASE("schema 2 rejects a duplicate class name", "[model_identity]") {
    Gen2 g; g.class_names = R"(["0","0"])";
    REQUIRE_FALSE(accepted(g.json()));
}

TEST_CASE("schema 2 rejects a whitespace-only class name", "[model_identity]") {
    Gen2 g; g.class_names = R"(["0","  "])";
    REQUIRE_FALSE(accepted(g.json()));
}

TEST_CASE("schema 2 rejects a duplicate tensorrt engine filename",
          "[model_identity]") {
    Gen2 g;
    auto r = parse_manifest(g.json());
    REQUIRE(r.manifest.has_value());
    auto dup = r.manifest->generations.at(0);
    dup.name = "digitv3-b";
    dup.canonical_id = "digitv3-b";
    r.manifest->generations.push_back(dup);   // same engine filename
    REQUIRE(validate_manifest(*r.manifest).has_value());
}

TEST_CASE("schema 2 rejects an empty installed_utc", "[model_identity]") {
    Gen2 g;
    auto r = parse_manifest(g.json());
    REQUIRE(r.manifest.has_value());
    r.manifest->generations.at(0).installed_utc.clear();
    REQUIRE(validate_manifest(*r.manifest).has_value());
}

// ─── schema-1 rule ORDER, not just the rule set ──────────────────────────────

TEST_CASE("schema 1 reports the digest before the state for a doubly-invalid entry",
          "[model_identity]") {
    // Refactoring the shared rules into a common helper must not reorder them.
    // The verdict would be Blocked either way, so only the operator-facing
    // message changes -- which is exactly the kind of drift that goes unnoticed.
    // Schema 1 checks the digests BEFORE `state`; this pins that order.
    auto r = parse_manifest(schema1_json());
    REQUIRE(r.manifest.has_value());
    auto& g = r.manifest->generations.at(0);
    g.state = "staged";
    g.engine_sha256 = "short";
    const auto err = validate_manifest(*r.manifest);
    REQUIRE(err.has_value());
    REQUIRE(err->find("engine_sha256") != std::string::npos);
}

TEST_CASE("schema 1 still rejects a bad state when every digest is valid",
          "[model_identity]") {
    auto r = parse_manifest(schema1_json());
    REQUIRE(r.manifest.has_value());
    r.manifest->generations.at(0).state = "staged";
    const auto err = validate_manifest(*r.manifest);
    REQUIRE(err.has_value());
    REQUIRE(err->find("state") != std::string::npos);
}

// ═════════════════════════════════════════════════════════════════════════════
// Slice 6 — resolve_model_metadata + ManifestView backend resolution.
//
// These cases test BOTH backends independent of the build host by constructing
// ManifestView instances with an explicit Backend. Artifacts are written to a
// temporary directory with MEASURED hashes; no real model file is touched.
// ═════════════════════════════════════════════════════════════════════════════

using denso::detection::DetectionModel;
using denso::models::active_backend;
using denso::models::Backend;
using denso::models::BuiltFor;
using denso::models::Manifest;
using denso::models::ManifestView;
using denso::models::model_compatibility;
using denso::models::ModelGeneration;
using denso::models::ModelMetadata;
using denso::models::OnnxRuntimeArtifact;
using denso::models::PlatformInfo;
using denso::models::resolve_model_metadata;
using denso::models::TensorRtArtifact;
using denso::models::Verdict;
using denso::mode::TargetMode;

namespace {

const PlatformInfo kJetson{"10.3", "12.6", "87"};

// Write bytes to <dir>/<name>; return the file's MEASURED sha256.
std::string put(const QString& dir, const std::string& name, const std::string& body) {
    QFile f(QDir(dir).filePath(QString::fromStdString(name)));
    REQUIRE(f.open(QIODevice::WriteOnly));
    const QByteArray bytes = QByteArray::fromStdString(body);
    REQUIRE(f.write(bytes) == bytes.size());
    f.close();
    auto h = denso::models::file_sha256(f.fileName());
    REQUIRE(h.has_value());
    return *h;
}

struct GenSpec {
    std::string canonical_id = "digitv3";
    std::string family       = "digit_numeric";
    std::vector<std::string> class_names = {"0", "1"};
    bool with_ort = true;
    bool with_trt = true;
    std::string onnx    = "digitv3.onnx";
    std::string engine  = "digitv3.engine";
    std::string sidecar = "digitv3.names.json";
    BuiltFor built_for{"10.3", "12.6", "87"};
};

// A DECLARED (schema-2) generation whose artifacts are written into `dir` with
// matching hashes, so provenance passes until a test deliberately corrupts it.
ModelGeneration make_gen(const QString& dir, const GenSpec& s) {
    ModelGeneration g;
    g.declared      = true;
    g.name          = s.canonical_id;
    g.canonical_id  = s.canonical_id;
    g.family        = s.family;
    g.task          = "detect";
    g.input_size    = 640;
    g.class_names   = s.class_names;
    g.class_count   = static_cast<int>(s.class_names.size());
    g.installed_utc = "2026-07-22T00:00:00Z";
    g.state         = "installed";
    if (s.with_ort) {
        OnnxRuntimeArtifact o;
        o.model                 = s.onnx;
        o.model_sha256          = put(dir, s.onnx, "onnx:" + s.canonical_id);
        o.class_metadata_source = denso::models::kSourceOnnxMetadataNames;
        g.runtime.onnxruntime   = o;
    }
    if (s.with_trt) {
        TensorRtArtifact t;
        t.engine                = s.engine;
        t.engine_sha256         = put(dir, s.engine, "engine:" + s.canonical_id);
        t.sidecar               = s.sidecar;
        t.sidecar_sha256        = put(dir, s.sidecar, "sidecar:" + s.canonical_id);
        t.class_metadata_source = denso::models::kSourceNamesSidecar;
        t.built_for             = s.built_for;
        g.runtime.tensorrt      = t;
    }
    return g;
}

Manifest one(const ModelGeneration& g) {
    Manifest m;
    m.schema = 2;
    m.generations.push_back(g);
    return m;
}

DetectionModel row(const std::string& filename,
                   const std::vector<std::string>& class_names = {"0", "1"}) {
    DetectionModel r;
    r.filename    = filename;
    r.class_names = class_names;
    return r;
}

}  // namespace

// ─── 1-3. TensorRT resolves the engine, hashes engine + sidecar, checks built_for

TEST_CASE("tensorrt resolves the engine artifact and corroborates provenance",
          "[model_identity]") {
    QTemporaryDir tmp; REQUIRE(tmp.isValid());
    ManifestView view(one(make_gen(tmp.path(), {})), tmp.path(), Backend::TensorRt);
    auto md = resolve_model_metadata(view, row("digitv3.engine"), kJetson);
    REQUIRE(md.declared);
    REQUIRE(md.canonical_id == "digitv3");
    REQUIRE(md.family == "digit_numeric");
    REQUIRE(md.filename == "digitv3.engine");
    REQUIRE(md.artifact_matches);
    REQUIRE(md.provenance_ok);
}

TEST_CASE("tensorrt fails provenance when the engine hash does not match",
          "[model_identity]") {
    QTemporaryDir tmp; REQUIRE(tmp.isValid());
    auto g = make_gen(tmp.path(), {});
    g.runtime.tensorrt->engine_sha256 = std::string(64, '0');  // no longer matches disk
    ManifestView view(one(g), tmp.path(), Backend::TensorRt);
    auto md = resolve_model_metadata(view, row("digitv3.engine"), kJetson);
    REQUIRE(md.declared);          // a bad hash does NOT make it undeclared
    REQUIRE(md.artifact_matches);  // and does NOT touch class corroboration
    REQUIRE_FALSE(md.provenance_ok);
}

TEST_CASE("tensorrt fails provenance when the sidecar hash does not match",
          "[model_identity]") {
    QTemporaryDir tmp; REQUIRE(tmp.isValid());
    auto g = make_gen(tmp.path(), {});
    g.runtime.tensorrt->sidecar_sha256 = std::string(64, '0');
    ManifestView view(one(g), tmp.path(), Backend::TensorRt);
    auto md = resolve_model_metadata(view, row("digitv3.engine"), kJetson);
    REQUIRE_FALSE(md.provenance_ok);
}

// ─── 4. TensorRT wrong built_for → provenance false (declared/artifact intact) ─

TEST_CASE("tensorrt with a wrong built_for fails provenance only", "[model_identity]") {
    QTemporaryDir tmp; REQUIRE(tmp.isValid());
    ManifestView view(one(make_gen(tmp.path(), {})), tmp.path(), Backend::TensorRt);
    auto md = resolve_model_metadata(view, row("digitv3.engine"),
                                     PlatformInfo{"9.9", "12.6", "87"});
    REQUIRE(md.declared);
    REQUIRE(md.artifact_matches);
    REQUIRE_FALSE(md.provenance_ok);
    REQUIRE(model_compatibility(TargetMode::DigitReader, md).verdict ==
            Verdict::RejectedProvenance);
}

// ─── 5-8. ONNX Runtime hashes only the ONNX and never reads built_for ─────────

TEST_CASE("onnxruntime resolves the onnx artifact and hashes only the onnx",
          "[model_identity]") {
    QTemporaryDir tmp; REQUIRE(tmp.isValid());
    // Deliberately WRONG built_for in the manifest: ORT must never read it.
    GenSpec s; s.built_for = BuiltFor{"WRONG", "WRONG", "WRONG"};
    ManifestView view(one(make_gen(tmp.path(), s)), tmp.path(), Backend::OnnxRuntime);
    auto md = resolve_model_metadata(view, row("digitv3.onnx"), PlatformInfo{});
    REQUIRE(md.declared);
    REQUIRE(md.filename == "digitv3.onnx");
    REQUIRE(md.artifact_matches);
    REQUIRE(md.provenance_ok);  // onnx hash matches; built_for is irrelevant here
}

TEST_CASE("a wrong tensorrt built_for cannot reject an onnx deployment",
          "[model_identity]") {
    QTemporaryDir tmp; REQUIRE(tmp.isValid());
    GenSpec s; s.built_for = BuiltFor{"WRONG", "WRONG", "WRONG"};
    ManifestView view(one(make_gen(tmp.path(), s)), tmp.path(), Backend::OnnxRuntime);
    auto md = resolve_model_metadata(view, row("digitv3.onnx"), PlatformInfo{});
    REQUIRE(model_compatibility(TargetMode::DigitReader, md).allowed());
}

// ─── 9-10. A block for the OTHER backend does not make the model available ────

TEST_CASE("an onnx-only generation is declared on ORT but undeclared on TRT",
          "[model_identity]") {
    QTemporaryDir tmp; REQUIRE(tmp.isValid());
    GenSpec s; s.with_trt = false;
    Manifest m = one(make_gen(tmp.path(), s));

    ManifestView ort(m, tmp.path(), Backend::OnnxRuntime);
    REQUIRE(resolve_model_metadata(ort, row("digitv3.onnx"), PlatformInfo{}).declared);

    ManifestView trt(m, tmp.path(), Backend::TensorRt);
    auto md = resolve_model_metadata(trt, row("digitv3.engine"), kJetson);
    REQUIRE_FALSE(md.declared);
    REQUIRE(model_compatibility(TargetMode::DigitReader, md).reason_code == "model_undeclared");
}

TEST_CASE("a tensorrt-only generation is declared on TRT but undeclared on ORT",
          "[model_identity]") {
    QTemporaryDir tmp; REQUIRE(tmp.isValid());
    GenSpec s; s.with_ort = false;
    Manifest m = one(make_gen(tmp.path(), s));

    ManifestView trt(m, tmp.path(), Backend::TensorRt);
    REQUIRE(resolve_model_metadata(trt, row("digitv3.engine"), kJetson).declared);

    ManifestView ort(m, tmp.path(), Backend::OnnxRuntime);
    REQUIRE_FALSE(resolve_model_metadata(ort, row("digitv3.onnx"), PlatformInfo{}).declared);
}

// ─── 11. the same canonical id resolves on both backends ──────────────────────

TEST_CASE("the same canonical id resolves on both backends", "[model_identity]") {
    QTemporaryDir tmp; REQUIRE(tmp.isValid());
    Manifest m = one(make_gen(tmp.path(), {}));
    ManifestView ort(m, tmp.path(), Backend::OnnxRuntime);
    ManifestView trt(m, tmp.path(), Backend::TensorRt);
    auto a = resolve_model_metadata(ort, row("digitv3.onnx"), PlatformInfo{});
    auto b = resolve_model_metadata(trt, row("digitv3.engine"), kJetson);
    REQUIRE(a.canonical_id == "digitv3");
    REQUIRE(b.canonical_id == "digitv3");
    REQUIRE(a.filename == "digitv3.onnx");   // backend-appropriate artifact
    REQUIRE(b.filename == "digitv3.engine");
}

// ─── 12. no matching generation → undeclared ──────────────────────────────────

TEST_CASE("a filename with no matching generation resolves undeclared",
          "[model_identity]") {
    QTemporaryDir tmp; REQUIRE(tmp.isValid());
    ManifestView view(one(make_gen(tmp.path(), {})), tmp.path(), Backend::TensorRt);
    auto md = resolve_model_metadata(view, row("nonexistent.engine"), kJetson);
    REQUIRE_FALSE(md.declared);
    REQUIRE_FALSE(md.artifact_matches);
    REQUIRE_FALSE(md.provenance_ok);
}

// ─── 13-14. class order / count mismatch → artifact mismatch (not undeclared) ─

TEST_CASE("a reordered class list fails artifact corroboration only",
          "[model_identity]") {
    QTemporaryDir tmp; REQUIRE(tmp.isValid());
    ManifestView view(one(make_gen(tmp.path(), {})), tmp.path(), Backend::TensorRt);
    auto md = resolve_model_metadata(view, row("digitv3.engine", {"1", "0"}), kJetson);
    REQUIRE(md.declared);
    REQUIRE(md.provenance_ok);
    REQUIRE_FALSE(md.artifact_matches);
    REQUIRE(model_compatibility(TargetMode::DigitReader, md).reason_code ==
            "model_classes_mismatch");
}

TEST_CASE("a class-count mismatch fails artifact corroboration only",
          "[model_identity]") {
    QTemporaryDir tmp; REQUIRE(tmp.isValid());
    ManifestView view(one(make_gen(tmp.path(), {})), tmp.path(), Backend::TensorRt);
    auto md = resolve_model_metadata(view, row("digitv3.engine", {"0"}), kJetson);
    REQUIRE(md.declared);
    REQUIRE_FALSE(md.artifact_matches);
}

TEST_CASE("a renamed class (same count and order length) fails corroboration only",
          "[model_identity]") {
    // Exercises the resolver's own ordered-equality: one label differs, count and
    // position are unchanged. This is distinct from the reordered and wrong-count
    // cases and kills a mutation that compared only counts/sizes.
    QTemporaryDir tmp; REQUIRE(tmp.isValid());
    ManifestView view(one(make_gen(tmp.path(), {})), tmp.path(), Backend::TensorRt);
    auto md = resolve_model_metadata(view, row("digitv3.engine", {"0", "X"}), kJetson);
    REQUIRE(md.declared);
    REQUIRE(md.provenance_ok);
    REQUIRE_FALSE(md.artifact_matches);
    REQUIRE(model_compatibility(TargetMode::DigitReader, md).reason_code ==
            "model_classes_mismatch");
}

// ─── 15. a runtime hash mismatch is provenance, not identity ──────────────────
// (covered by "tensorrt fails provenance when the engine hash does not match")

// ─── 16. a schema-1 generation is never a declared identity ───────────────────

TEST_CASE("a schema-1 generation resolves undeclared even if its engine matches",
          "[model_identity]") {
    QTemporaryDir tmp; REQUIRE(tmp.isValid());
    ModelGeneration g1;             // declared == false (schema 1)
    g1.name = "digit-v3.1";
    g1.engine = "digitv3.engine";   // matches the query filename at the ROOT
    g1.sidecar = "digitv3.names.json";
    g1.class_names = {"0", "1"};
    Manifest m; m.schema = 1; m.generations.push_back(g1);
    ManifestView view(m, tmp.path(), Backend::TensorRt);
    auto md = resolve_model_metadata(view, row("digitv3.engine"), kJetson);
    REQUIRE_FALSE(md.declared);
    REQUIRE(model_compatibility(TargetMode::DigitReader, md).reason_code == "model_undeclared");
}

// ─── the active backend is the one #ifdef split, and is fixed ─────────────────

TEST_CASE("active_backend matches the build platform", "[model_identity]") {
#ifdef _WIN32
    REQUIRE(active_backend() == Backend::OnnxRuntime);
#else
    REQUIRE(active_backend() == Backend::TensorRt);
#endif
}
