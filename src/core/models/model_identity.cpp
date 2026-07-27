#include "models/model_identity.h"

#include "models/hashing.h"

#include <QDir>
#include <QFile>

#include <algorithm>

namespace denso::models {
namespace {

// Does <models_dir>/<filename> hash to `expected` under the active backend?
// Missing/unreadable file or mismatch -> false (fail closed).
bool hash_matches(const QString& models_dir, const std::string& filename,
                  const std::string& expected) {
    const QString path = QDir(models_dir).filePath(QString::fromStdString(filename));
    const auto actual = file_sha256(path);
    return actual.has_value() && *actual == expected;
}

// The active-backend provenance evidence. ONNX Runtime hashes only its own model
// and NEVER reads built_for; TensorRT hashes engine AND sidecar and corroborates
// built_for against the measured platform.
bool provenance_ok(const ManifestView& view, const ModelGeneration& gen,
                   const PlatformInfo& platform) {
    if (view.backend() == Backend::OnnxRuntime) {
        const auto& ort = *gen.runtime.onnxruntime;  // present — we matched on it
        return hash_matches(view.models_dir(), ort.model, ort.model_sha256);
    }
    const auto& trt = *gen.runtime.tensorrt;          // present — we matched on it
    if (!hash_matches(view.models_dir(), trt.engine, trt.engine_sha256)) return false;
    if (!hash_matches(view.models_dir(), trt.sidecar, trt.sidecar_sha256)) return false;
    // built_for is TensorRT-local and evaluated ONLY here (spec 3.2.1 rule 2).
    return trt.built_for.trt == platform.trt && trt.built_for.cuda == platform.cuda &&
           trt.built_for.sm == platform.sm;
}

}  // namespace

ModelMetadata resolve_model_metadata(const ManifestView& view,
                                     const denso::detection::DetectionModel& row,
                                     const PlatformInfo& platform) {
    ModelMetadata md;
    // filename is the ACTIVE backend's artifact — the catalog row is the on-disk
    // artifact this build loads. Carried even when undeclared, for diagnostics.
    md.filename = row.filename;
    md.class_names = row.class_names;  // ordered, from the artifact

    // Join to the declared generation whose ACTIVE-backend block declares this
    // filename. Identity is taken from the generation, never parsed from the name.
    // A schema-1 generation is not a declared identity and is skipped; so is a
    // generation with no block for the active backend (spec 3.2.1 rule 5).
    const ModelGeneration* gen = nullptr;
    for (const auto& g : view.manifest().generations) {
        if (!g.declared) continue;
        if (view.backend() == Backend::OnnxRuntime) {
            if (g.runtime.onnxruntime && g.runtime.onnxruntime->model == row.filename) {
                gen = &g;
                break;
            }
        } else {
            if (g.runtime.tensorrt && g.runtime.tensorrt->engine == row.filename) {
                gen = &g;
                break;
            }
        }
    }
    if (gen == nullptr) return md;  // declared/artifact_matches/provenance_ok all false

    md.declared = true;
    md.canonical_id = gen->canonical_id;
    md.family = gen->family;
    md.task = gen->task;
    md.input_size = gen->input_size;
    md.class_count = gen->class_count;

    // artifact_matches: ordered class names AND count agree with the declaration.
    // Independent of provenance so the policy can distinguish the two faults.
    md.artifact_matches =
        row.class_names == gen->class_names &&
        static_cast<int>(row.class_names.size()) == gen->class_count;

    md.provenance_ok = provenance_ok(view, *gen, platform);
    return md;
}

std::string diagnostic_filename(const std::string& raw) {
    // Step 1: last path segment only. Both separators are handled — the column is
    // written on two platforms and a hand-edited row may contain either.
    const size_t cut = raw.find_last_of("/\\");
    const std::string tail = (cut == std::string::npos) ? raw : raw.substr(cut + 1);
    if (tail.empty()) return std::string("<invalid>");

    // Step 2: an ALLOW-LIST over what survives, and fail closed if anything else
    // is present. Dropping the leading path is NOT sufficient on its own:
    // "m.engine?token=SECRET" keeps its query string, and a slashless
    // "user:password@host" is not reduced at all. Both would still carry a secret
    // into status.json.
    //
    // The set is [A-Za-z0-9._-]. This is STRICTER than the manifest's own filename
    // rule — `is_basename` rejects empty strings, separators and "..", and would
    // happily pass ':' '@' '?' '=' — so a legal-but-exotic declared name such as
    // `model+v1.engine` also reduces to "<invalid>". That is the intended trade
    // (see the header): callers pair this with the catalog row id, so an
    // unprintable name is still identifiable, and every artifact this product
    // ships is inside the set because canonical_id is an is_safe_token.
    //
    // An allow-list is used rather than stripping the offending characters,
    // because partial sanitisation is what produces bypasses: anything that is not
    // provably a plain filename is reported as "<invalid>" and nothing else.
    const bool safe = std::all_of(tail.begin(), tail.end(), [](unsigned char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
               (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
    });
    return safe ? tail : std::string("<invalid>");
}

ManifestView load_manifest_view(const QString& models_dir) {
    Manifest manifest;  // schema 0, no generations — the fail-closed default
    const QString path =
        QDir(models_dir).filePath(QStringLiteral("manifest.json"));
    QFile f(path);
    if (f.exists() && f.open(QIODevice::ReadOnly)) {
        auto pr = parse_manifest(f.readAll().toStdString());
        // A parse failure keeps the EMPTY manifest, never a partial one: half a
        // declaration must not authorize half a model. The corrupt file is
        // reported separately, as a global blocker, by evaluate_integrity.
        if (pr.manifest) manifest = std::move(*pr.manifest);
    }
    // The two-arg production ctor binds the ACTIVE backend — the one #ifdef split.
    return ManifestView(std::move(manifest), models_dir);
}

}  // namespace denso::models
