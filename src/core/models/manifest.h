// The model manifest — the SOLE artifact-identity authority (SHA-bound).
//
// TWO SCHEMAS live here at once, and the distinction is load-bearing:
//
//   schema 1 — the shipped format. Root-level `engine`/`sidecar`/`*_sha256` and a
//              root `built_for`. Parsed and validated EXACTLY as before; nothing
//              about its behaviour may change (an appliance in the field has one).
//   schema 2 — adds a DECLARED identity (`canonical_id`, `family`, `task`,
//              `input_size`, `class_count`), a PER-BACKEND `runtime` block, and a
//              `provenance` block. `built_for` moves INSIDE `runtime.tensorrt`.
//
// Why `built_for` moved: it is a TensorRT platform assertion. Left at the root it
// would be readable — and therefore eventually read — on a Windows/ONNX Runtime
// deployment, where a TRT/CUDA/sm mismatch is meaningless. Nesting it makes a
// cross-platform rejection structurally impossible rather than merely discouraged,
// which is why a root `built_for` is REJECTED for schema 2.
//
// The manifest deliberately carries NO `allowed_modes` / mode matrix of any kind.
// It declares WHAT AN ARTIFACT IS; what a model may do is decided elsewhere by a
// compiled policy. A stray `allowed_modes` key in a file is inert: there is no
// field for it to land in, so it can never become a second authority.
//
// Consumers must read TensorRT artifact data through the schema-aware accessors
// below, never the raw root fields — those are empty for schema 2, and a direct
// read would silently compare against nothing.
#pragma once
#include <optional>
#include <string>
#include <vector>
namespace denso::models {

// The CLOSED vocabulary for `class_metadata_source`. Not free text: a merely
// non-empty check would accept a typo that silently described the wrong source
// while every other field validated cleanly.
inline constexpr const char* kSourceOnnxMetadataNames = "onnx_metadata_names";
inline constexpr const char* kSourceNamesSidecar      = "names_sidecar";

/// The TensorRT platform triple. Schema 1 keeps this at the generation root;
/// schema 2 carries it inside the TensorRT runtime block only.
struct BuiltFor {
    std::string trt, cuda, sm;
};

/// What an ONNX Runtime deployment (Windows/MSYS2) loads. Class names come from
/// the ONNX file's own `names` metadata, so there is no sidecar here.
struct OnnxRuntimeArtifact {
    std::string model;                   // "<stem>.onnx"
    std::string model_sha256;
    std::string class_metadata_source;   // must equal kSourceOnnxMetadataNames
};

/// What a TensorRT deployment (Jetson) loads: a prebuilt plan plus the sidecar it
/// takes class names from, and the platform the plan was compiled for.
struct TensorRtArtifact {
    std::string engine;                  // "<stem>.engine"
    std::string engine_sha256;
    std::string sidecar;                 // "<stem>.names.json"
    std::string sidecar_sha256;
    std::string class_metadata_source;   // must equal kSourceNamesSidecar
    BuiltFor    built_for;
};

/// One logical generation's runtime artifacts, one entry per backend. At least
/// one must be present; a generation with no block for the running backend is
/// simply not available there.
struct RuntimeArtifacts {
    std::optional<OnnxRuntimeArtifact> onnxruntime;
    std::optional<TensorRtArtifact>    tensorrt;
};

/// How the artifacts were produced. Recorded for humans and for the packaging
/// approval flow; only the fields listed in validate_manifest are enforced.
struct Provenance {
    std::string source_pt, source_pt_sha256;
    std::string onnx, onnx_sha256;
    int         onnx_opset = 0;
    std::string training_ultralytics, export_ultralytics;
    int         batch   = 0;
    bool        dynamic = false;
    bool        nms     = false;
    std::string precision, jetpack;
    std::string export_onnx_command, export_engine_command;
};

struct ModelGeneration {
    // ── schema 1 (root) — untouched, and still the only fields a schema-1
    //    generation populates.
    std::string name, engine, engine_sha256, sidecar, sidecar_sha256;
    std::vector<std::string> class_names;
    std::string trt, cuda, sm, installed_utc, state;

    // ── schema 2 additions. `declared` is the discriminator: true iff this
    //    generation came from a schema-2 manifest, i.e. iff it carries an
    //    explicit identity declaration.
    bool             declared = false;
    std::string      canonical_id, family, task;
    int              input_size  = 0;
    int              class_count = 0;
    RuntimeArtifacts runtime;
    Provenance       provenance;

    // ── schema-aware accessors. Schema 1 -> the root field; schema 2 -> the
    //    nested TensorRT field; no TensorRT block -> empty. Every consumer uses
    //    these so that adding schema 2 could not silently feed an empty string to
    //    a path-escape guard or a hash comparison.
    const std::string& tensorrt_engine() const;
    const std::string& tensorrt_sidecar() const;
    const std::string& tensorrt_engine_sha256() const;
    const std::string& tensorrt_sidecar_sha256() const;
    const std::string& built_for_trt() const;
    const std::string& built_for_cuda() const;
    const std::string& built_for_sm() const;

    /// Every runtime artifact filename this generation DECLARES, across both
    /// backends — the input to the integrity manifested-set bookkeeping.
    ///
    /// Deliberately backend- and mode-INDEPENDENT: a file is "manifested" because
    /// it is described, not because it is usable on this box. Sidecars are
    /// excluded because the on-disk scan they would be compared against only
    /// enumerates *.engine and *.onnx.
    std::vector<std::string> declared_runtime_files() const;
};

struct Manifest { int schema = 0; std::vector<ModelGeneration> generations; };
struct ParseResult { std::optional<Manifest> manifest; std::string error; };

/// Structural parse. Accepts schema 1 and schema 2; anything else is an error.
/// Key-presence rules (an unknown `runtime` backend, a schema-2 root `built_for`,
/// an absent `runtime`/`provenance`) are enforced here, since they are invisible
/// once parsed into typed fields.
ParseResult parse_manifest(const std::string& json_text);

/// Semantic rules. Schema-1 generations are checked exactly as before; schema-2
/// generations additionally get the declared-identity and per-backend rules.
std::optional<std::string> validate_manifest(const Manifest& m);

/// Resolve a generation by its TensorRT engine filename — root for schema 1,
/// `runtime.tensorrt.engine` for schema 2. Never matches an ONNX filename, and
/// never matches a generation that declares no TensorRT engine.
const ModelGeneration* find_by_engine(const Manifest& m, const std::string& engine);
}
