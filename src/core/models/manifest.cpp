#include "models/manifest.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <algorithm>
#include <set>
namespace denso::models {
namespace {
ParseResult fail(const std::string& why) { return {std::nullopt, why}; }
bool str(const QJsonObject& o, const char* k, std::string& out) {
    if (!o.contains(k) || !o.value(k).isString()) return false;
    out = o.value(k).toString().toStdString(); return true;
}
// An integral JSON number, rejecting 1.5 and "1" alike.
bool whole(const QJsonObject& o, const char* k, int& out) {
    if (!o.contains(k)) return false;
    const QJsonValue v = o.value(k);
    if (!v.isDouble() || v.toDouble() != static_cast<double>(v.toInt())) return false;
    out = v.toInt(); return true;
}
// Optional, best-effort: provenance carries descriptive fields whose absence is
// caught by validate_manifest where it matters, not here.
void opt_str(const QJsonObject& o, const char* k, std::string& out) {
    if (o.contains(k) && o.value(k).isString()) out = o.value(k).toString().toStdString();
}
void opt_whole(const QJsonObject& o, const char* k, int& out) {
    if (o.contains(k) && o.value(k).isDouble()) out = o.value(k).toInt();
}
void opt_bool(const QJsonObject& o, const char* k, bool& out) {
    if (o.contains(k) && o.value(k).isBool()) out = o.value(k).toBool();
}

bool parse_built_for(const QJsonObject& parent, BuiltFor& bf) {
    if (!parent.value("built_for").isObject()) return false;
    const QJsonObject o = parent.value("built_for").toObject();
    return str(o, "trt", bf.trt) && str(o, "cuda", bf.cuda) && str(o, "sm", bf.sm);
}

// Schema 1: every field at the generation root, exactly as shipped.
std::optional<std::string> parse_schema1(const QJsonObject& o, ModelGeneration& g) {
    if (!str(o,"name",g.name) || !str(o,"engine",g.engine) ||
        !str(o,"engine_sha256",g.engine_sha256) || !str(o,"sidecar",g.sidecar) ||
        !str(o,"sidecar_sha256",g.sidecar_sha256) ||
        !str(o,"installed_utc",g.installed_utc) || !str(o,"state",g.state))
        return "generation missing a required string field";
    if (!o.value("class_names").isArray()) return "generation missing class_names";
    for (const auto c : o.value("class_names").toArray()) {
        if (!c.isString()) return "class_names must be strings";
        g.class_names.push_back(c.toString().toStdString());
    }
    BuiltFor bf;
    if (!parse_built_for(o, bf)) return "generation missing built_for.{trt,cuda,sm}";
    g.trt = bf.trt; g.cuda = bf.cuda; g.sm = bf.sm;
    return std::nullopt;
}

// Schema 2: declared identity + per-backend runtime + provenance.
std::optional<std::string> parse_schema2(const QJsonObject& o, ModelGeneration& g) {
    g.declared = true;
    if (!str(o,"name",g.name) || !str(o,"canonical_id",g.canonical_id) ||
        !str(o,"family",g.family) || !str(o,"task",g.task) ||
        !str(o,"installed_utc",g.installed_utc) || !str(o,"state",g.state))
        return "generation missing a required string field";
    if (!whole(o, "input_size", g.input_size))
        return "generation missing an integer input_size";
    if (!whole(o, "class_count", g.class_count))
        return "generation missing an integer class_count";
    if (!o.value("class_names").isArray()) return "generation missing class_names";
    for (const auto c : o.value("class_names").toArray()) {
        if (!c.isString()) return "class_names must be strings";
        g.class_names.push_back(c.toString().toStdString());
    }

    // built_for belongs to the TensorRT block ALONE. A root copy is refused here
    // rather than ignored: ignoring it would leave a plausible-looking platform
    // assertion in the file that nothing enforces, and the whole point of nesting
    // it is that a TensorRT platform check can never reach an ONNX deployment.
    if (o.contains("built_for"))
        return "schema 2 must not carry built_for at the generation root "
               "(it belongs to runtime.tensorrt) for generation " + g.name;

    if (!o.value("runtime").isObject())
        return "generation missing a runtime object";
    const QJsonObject rt = o.value("runtime").toObject();
    // No silent third backend: an unrecognised key would otherwise be dropped and
    // the generation would look fully declared while an artifact went unchecked.
    for (auto it = rt.begin(); it != rt.end(); ++it) {
        const QString k = it.key();
        if (k != QStringLiteral("onnxruntime") && k != QStringLiteral("tensorrt"))
            return "unknown runtime backend '" + k.toStdString() +
                   "' for generation " + g.name;
    }
    if (rt.contains("onnxruntime")) {
        if (!rt.value("onnxruntime").isObject())
            return "runtime.onnxruntime must be an object for generation " + g.name;
        const QJsonObject ort = rt.value("onnxruntime").toObject();
        OnnxRuntimeArtifact a;
        if (!str(ort,"model",a.model) || !str(ort,"model_sha256",a.model_sha256) ||
            !str(ort,"class_metadata_source",a.class_metadata_source))
            return "runtime.onnxruntime missing a required string field for generation " + g.name;
        g.runtime.onnxruntime = a;
    }
    if (rt.contains("tensorrt")) {
        if (!rt.value("tensorrt").isObject())
            return "runtime.tensorrt must be an object for generation " + g.name;
        const QJsonObject trt = rt.value("tensorrt").toObject();
        TensorRtArtifact a;
        if (!str(trt,"engine",a.engine) || !str(trt,"engine_sha256",a.engine_sha256) ||
            !str(trt,"sidecar",a.sidecar) || !str(trt,"sidecar_sha256",a.sidecar_sha256) ||
            !str(trt,"class_metadata_source",a.class_metadata_source))
            return "runtime.tensorrt missing a required string field for generation " + g.name;
        if (!parse_built_for(trt, a.built_for))
            return "runtime.tensorrt missing built_for.{trt,cuda,sm} for generation " + g.name;
        g.runtime.tensorrt = a;
    }
    if (!g.runtime.onnxruntime && !g.runtime.tensorrt)
        return "runtime must declare at least one backend for generation " + g.name;

    if (!o.value("provenance").isObject())
        return "generation missing a provenance object";
    const QJsonObject p = o.value("provenance").toObject();
    opt_str(p, "source_pt", g.provenance.source_pt);
    opt_str(p, "source_pt_sha256", g.provenance.source_pt_sha256);
    opt_str(p, "onnx", g.provenance.onnx);
    opt_str(p, "onnx_sha256", g.provenance.onnx_sha256);
    opt_whole(p, "onnx_opset", g.provenance.onnx_opset);
    opt_str(p, "training_ultralytics", g.provenance.training_ultralytics);
    opt_str(p, "export_ultralytics", g.provenance.export_ultralytics);
    opt_whole(p, "batch", g.provenance.batch);
    opt_bool(p, "dynamic", g.provenance.dynamic);
    opt_bool(p, "nms", g.provenance.nms);
    opt_str(p, "precision", g.provenance.precision);
    opt_str(p, "jetpack", g.provenance.jetpack);
    opt_str(p, "export_onnx_command", g.provenance.export_onnx_command);
    opt_str(p, "export_engine_command", g.provenance.export_engine_command);
    return std::nullopt;
}
}  // namespace

// ─── schema-aware accessors ─────────────────────────────────────────────────
namespace {
const std::string& empty_string() {
    static const std::string kEmpty;
    return kEmpty;
}
}  // namespace

const std::string& ModelGeneration::tensorrt_engine() const {
    if (!declared) return engine;
    return runtime.tensorrt ? runtime.tensorrt->engine : empty_string();
}
const std::string& ModelGeneration::tensorrt_sidecar() const {
    if (!declared) return sidecar;
    return runtime.tensorrt ? runtime.tensorrt->sidecar : empty_string();
}
const std::string& ModelGeneration::tensorrt_engine_sha256() const {
    if (!declared) return engine_sha256;
    return runtime.tensorrt ? runtime.tensorrt->engine_sha256 : empty_string();
}
const std::string& ModelGeneration::tensorrt_sidecar_sha256() const {
    if (!declared) return sidecar_sha256;
    return runtime.tensorrt ? runtime.tensorrt->sidecar_sha256 : empty_string();
}
const std::string& ModelGeneration::built_for_trt() const {
    if (!declared) return trt;
    return runtime.tensorrt ? runtime.tensorrt->built_for.trt : empty_string();
}
const std::string& ModelGeneration::built_for_cuda() const {
    if (!declared) return cuda;
    return runtime.tensorrt ? runtime.tensorrt->built_for.cuda : empty_string();
}
const std::string& ModelGeneration::built_for_sm() const {
    if (!declared) return sm;
    return runtime.tensorrt ? runtime.tensorrt->built_for.sm : empty_string();
}

std::vector<std::string> ModelGeneration::declared_runtime_files() const {
    std::vector<std::string> out;
    if (!declared) {
        // Schema 1 declares exactly one runtime artifact: the root engine. The
        // sidecar is deliberately not listed — see the header.
        if (!engine.empty()) out.push_back(engine);
        return out;
    }
    if (runtime.onnxruntime && !runtime.onnxruntime->model.empty())
        out.push_back(runtime.onnxruntime->model);
    if (runtime.tensorrt && !runtime.tensorrt->engine.empty())
        out.push_back(runtime.tensorrt->engine);
    return out;
}

ParseResult parse_manifest(const std::string& json_text) {
    QJsonParseError err{};
    const auto doc = QJsonDocument::fromJson(QByteArray::fromStdString(json_text), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject())
        return fail("manifest is not a JSON object");
    const QJsonObject root = doc.object();
    const QJsonValue sv = root.value("schema");
    // schema must be an INTEGER of 1 or 2 (reject 1.5, "2", 0, 3, ...)
    if (!sv.isDouble() || sv.toDouble() != static_cast<double>(sv.toInt()) ||
        (sv.toInt() != 1 && sv.toInt() != 2))
        return fail("schema must be the integer 1 or 2");
    if (!root.value("generations").isArray()) return fail("missing generations array");
    Manifest m; m.schema = sv.toInt();
    for (const auto v : root.value("generations").toArray()) {
        if (!v.isObject()) return fail("generation is not an object");
        const QJsonObject o = v.toObject();
        ModelGeneration g;
        const auto err2 = m.schema == 1 ? parse_schema1(o, g) : parse_schema2(o, g);
        if (err2) return fail(*err2);
        m.generations.push_back(std::move(g));
    }
    return {m, {}};
}
}

namespace denso::models {
namespace {
bool is_basename(const std::string& s) {
    if (s.empty()) return false;
    if (s.find('/') != std::string::npos) return false;
    if (s.find('\\') != std::string::npos) return false;
    if (s.find("..") != std::string::npos) return false;
    return true;
}
// The same discipline is_basename applies to filenames, for an identity token:
// [A-Za-z0-9._-]+, no separators, no traversal.
bool is_safe_token(const std::string& s) {
    if (!is_basename(s)) return false;
    return std::all_of(s.begin(), s.end(), [](unsigned char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
               (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
    });
}
bool is_lower_hex64(const std::string& s) {
    if (s.size() != 64) return false;
    return std::all_of(s.begin(), s.end(), [](unsigned char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}
bool ends_with(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() &&
           s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}
// "digit-v3.1.engine" -> "digit-v3.1" (strip the last dot-extension)
std::string stem_of(const std::string& s) {
    const auto pos = s.find_last_of('.');
    return pos == std::string::npos ? s : s.substr(0, pos);
}
// "digit-v3.1.names.json" -> "digit-v3.1" (strip the trailing ".names.json")
std::string sidecar_stem(const std::string& s) {
    const auto pos1 = s.find_last_of('.');
    if (pos1 == std::string::npos || pos1 == 0) return s;
    const auto pos2 = s.find_last_of('.', pos1 - 1);
    return pos2 == std::string::npos ? s.substr(0, pos1) : s.substr(0, pos2);
}
bool is_blank(const std::string& s) {
    return s.find_first_not_of(" \t\r\n\f\v") == std::string::npos;
}

// Rules shared by both schemas: the class vocabulary and the human-facing
// metadata. The `state` check is deliberately NOT here — schema 1 applies it
// last, after the digests, and that order is preserved verbatim below so a
// doubly-invalid schema-1 manifest reports exactly the error it reports today.
std::optional<std::string> validate_common(const ModelGeneration& g) {
    if (g.class_names.empty()) return "class_names must be non-empty for generation " + g.name;
    std::set<std::string> seen_classes;
    for (const auto& c : g.class_names) {
        if (is_blank(c))
            return "class_names must not contain blanks for generation " + g.name;
        if (!seen_classes.insert(c).second)
            return "duplicate class name '" + c + "' for generation " + g.name;
    }
    if (g.name.empty()) return "generation name must be non-empty";
    if (g.installed_utc.empty()) return "installed_utc must be non-empty for generation " + g.name;
    return std::nullopt;
}

// The shipped schema-1 rules, unchanged.
std::optional<std::string> validate_schema1(const ModelGeneration& g) {
    if (!is_basename(g.engine)) return "engine is not a safe basename: " + g.engine;
    if (!is_basename(g.sidecar)) return "sidecar is not a safe basename: " + g.sidecar;
    if (stem_of(g.engine) != sidecar_stem(g.sidecar))
        return "engine/sidecar stem mismatch for generation " + g.name;
    if (auto e = validate_common(g)) return e;
    if (g.trt.empty()) return "built_for.trt must be non-empty for generation " + g.name;
    if (g.cuda.empty()) return "built_for.cuda must be non-empty for generation " + g.name;
    if (g.sm.empty()) return "built_for.sm must be non-empty for generation " + g.name;
    if (!is_lower_hex64(g.engine_sha256))
        return "engine_sha256 must be 64 lowercase hex chars for generation " + g.name;
    if (!is_lower_hex64(g.sidecar_sha256))
        return "sidecar_sha256 must be 64 lowercase hex chars for generation " + g.name;
    if (g.state != "installed") return "state must be 'installed' for generation " + g.name;
    return std::nullopt;
}

std::optional<std::string> validate_schema2(const ModelGeneration& g) {
    if (g.canonical_id.empty()) return "canonical_id must be non-empty for generation " + g.name;
    if (!is_safe_token(g.canonical_id))
        return "canonical_id is not a safe token: " + g.canonical_id;
    if (g.family.empty()) return "family must be non-empty for generation " + g.name;
    if (!is_safe_token(g.family)) return "family is not a safe token: " + g.family;
    if (g.task.empty()) return "task must be non-empty for generation " + g.name;
    if (g.input_size <= 0) return "input_size must be positive for generation " + g.name;
    if (auto e = validate_common(g)) return e;
    if (g.state != "installed") return "state must be 'installed' for generation " + g.name;
    if (g.class_count <= 0) return "class_count must be positive for generation " + g.name;
    if (static_cast<size_t>(g.class_count) != g.class_names.size())
        return "class_count does not match class_names for generation " + g.name;

    if (!g.runtime.onnxruntime && !g.runtime.tensorrt)
        return "runtime must declare at least one backend for generation " + g.name;

    if (g.runtime.onnxruntime) {
        const auto& a = *g.runtime.onnxruntime;
        if (!is_basename(a.model))
            return "runtime.onnxruntime.model is not a safe basename: " + a.model;
        if (!ends_with(a.model, ".onnx"))
            return "runtime.onnxruntime.model must be a .onnx file: " + a.model;
        if (!is_lower_hex64(a.model_sha256))
            return "runtime.onnxruntime.model_sha256 must be 64 lowercase hex chars "
                   "for generation " + g.name;
        if (a.class_metadata_source != kSourceOnnxMetadataNames)
            return "runtime.onnxruntime.class_metadata_source must be '" +
                   std::string(kSourceOnnxMetadataNames) + "' for generation " + g.name;
    }
    if (g.runtime.tensorrt) {
        const auto& a = *g.runtime.tensorrt;
        if (!is_basename(a.engine))
            return "runtime.tensorrt.engine is not a safe basename: " + a.engine;
        if (!is_basename(a.sidecar))
            return "runtime.tensorrt.sidecar is not a safe basename: " + a.sidecar;
        if (stem_of(a.engine) != sidecar_stem(a.sidecar))
            return "runtime.tensorrt engine/sidecar stem mismatch for generation " + g.name;
        if (!is_lower_hex64(a.engine_sha256))
            return "runtime.tensorrt.engine_sha256 must be 64 lowercase hex chars "
                   "for generation " + g.name;
        if (!is_lower_hex64(a.sidecar_sha256))
            return "runtime.tensorrt.sidecar_sha256 must be 64 lowercase hex chars "
                   "for generation " + g.name;
        if (a.class_metadata_source != kSourceNamesSidecar)
            return "runtime.tensorrt.class_metadata_source must be '" +
                   std::string(kSourceNamesSidecar) + "' for generation " + g.name;
        if (a.built_for.trt.empty())
            return "runtime.tensorrt.built_for.trt must be non-empty for generation " + g.name;
        if (a.built_for.cuda.empty())
            return "runtime.tensorrt.built_for.cuda must be non-empty for generation " + g.name;
        if (a.built_for.sm.empty())
            return "runtime.tensorrt.built_for.sm must be non-empty for generation " + g.name;
    }

    // Provenance: only the fields that make an artifact traceable are enforced.
    const auto& p = g.provenance;
    if (p.source_pt_sha256.empty())
        return "provenance.source_pt_sha256 must be non-empty for generation " + g.name;
    if (p.onnx_sha256.empty())
        return "provenance.onnx_sha256 must be non-empty for generation " + g.name;
    if (p.export_ultralytics.empty())
        return "provenance.export_ultralytics must be non-empty for generation " + g.name;
    if (p.precision.empty())
        return "provenance.precision must be non-empty for generation " + g.name;
    if (p.export_engine_command.empty())
        return "provenance.export_engine_command must be non-empty for generation " + g.name;
    return std::nullopt;
}
}  // namespace

std::optional<std::string> validate_manifest(const Manifest& m) {
    std::set<std::string> names, engines, canonical_ids;
    for (const auto& g : m.generations) {
        if (auto e = g.declared ? validate_schema2(g) : validate_schema1(g)) return e;
        if (!names.insert(g.name).second) return "duplicate generation name: " + g.name;
        // Engine-filename uniqueness is keyed on the schema-aware accessor, so an
        // ORT-only generation (which declares no engine) never collides.
        const std::string& eng = g.tensorrt_engine();
        if (!eng.empty() && !engines.insert(eng).second)
            return "duplicate engine filename: " + eng;
        if (g.declared && !canonical_ids.insert(g.canonical_id).second)
            return "duplicate canonical_id: " + g.canonical_id;
    }
    return std::nullopt;
}

const ModelGeneration* find_by_engine(const Manifest& m, const std::string& engine) {
    // An empty query must never resolve — an ORT-only generation has no engine,
    // and matching "" against it would hand a caller the wrong generation.
    if (engine.empty()) return nullptr;
    for (const auto& g : m.generations)
        if (g.tensorrt_engine() == engine) return &g;
    return nullptr;
}
}  // namespace denso::models
