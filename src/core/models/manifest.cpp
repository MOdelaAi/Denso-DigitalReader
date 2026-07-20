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
}
ParseResult parse_manifest(const std::string& json_text) {
    QJsonParseError err{};
    const auto doc = QJsonDocument::fromJson(QByteArray::fromStdString(json_text), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject())
        return fail("manifest is not a JSON object");
    const QJsonObject root = doc.object();
    const QJsonValue sv = root.value("schema");
    // schema must be an INTEGER equal to 1 (reject 1.5, strings, etc.)
    if (!sv.isDouble() || sv.toDouble() != static_cast<double>(sv.toInt()) || sv.toInt() != 1)
        return fail("schema must be the integer 1");
    if (!root.value("generations").isArray()) return fail("missing generations array");
    Manifest m; m.schema = sv.toInt();
    for (const auto v : root.value("generations").toArray()) {
        if (!v.isObject()) return fail("generation is not an object");
        const QJsonObject o = v.toObject();
        ModelGeneration g;
        if (!str(o,"name",g.name) || !str(o,"engine",g.engine) ||
            !str(o,"engine_sha256",g.engine_sha256) || !str(o,"sidecar",g.sidecar) ||
            !str(o,"sidecar_sha256",g.sidecar_sha256) ||
            !str(o,"installed_utc",g.installed_utc) || !str(o,"state",g.state))
            return fail("generation missing a required string field");
        if (!o.value("class_names").isArray()) return fail("generation missing class_names");
        for (const auto c : o.value("class_names").toArray()) {
            if (!c.isString()) return fail("class_names must be strings");
            g.class_names.push_back(c.toString().toStdString());
        }
        const QJsonObject bf = o.value("built_for").toObject();
        if (!str(bf,"trt",g.trt) || !str(bf,"cuda",g.cuda) || !str(bf,"sm",g.sm))
            return fail("generation missing built_for.{trt,cuda,sm}");
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
bool is_lower_hex64(const std::string& s) {
    if (s.size() != 64) return false;
    return std::all_of(s.begin(), s.end(), [](unsigned char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
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
}  // namespace

std::optional<std::string> validate_manifest(const Manifest& m) {
    std::set<std::string> names, engines;
    for (const auto& g : m.generations) {
        if (!is_basename(g.engine)) return "engine is not a safe basename: " + g.engine;
        if (!is_basename(g.sidecar)) return "sidecar is not a safe basename: " + g.sidecar;
        if (stem_of(g.engine) != sidecar_stem(g.sidecar))
            return "engine/sidecar stem mismatch for generation " + g.name;
        if (g.class_names.empty()) return "class_names must be non-empty for generation " + g.name;
        std::set<std::string> seen_classes;
        for (const auto& c : g.class_names) {
            if (c.find_first_not_of(" \t\r\n\f\v") == std::string::npos)
                return "class_names must not contain blanks for generation " + g.name;
            if (!seen_classes.insert(c).second)
                return "duplicate class name '" + c + "' for generation " + g.name;
        }
        if (g.name.empty()) return "generation name must be non-empty";
        if (g.installed_utc.empty()) return "installed_utc must be non-empty for generation " + g.name;
        if (g.trt.empty()) return "built_for.trt must be non-empty for generation " + g.name;
        if (g.cuda.empty()) return "built_for.cuda must be non-empty for generation " + g.name;
        if (g.sm.empty()) return "built_for.sm must be non-empty for generation " + g.name;
        if (!is_lower_hex64(g.engine_sha256))
            return "engine_sha256 must be 64 lowercase hex chars for generation " + g.name;
        if (!is_lower_hex64(g.sidecar_sha256))
            return "sidecar_sha256 must be 64 lowercase hex chars for generation " + g.name;
        if (g.state != "installed") return "state must be 'installed' for generation " + g.name;
        if (!names.insert(g.name).second) return "duplicate generation name: " + g.name;
        if (!engines.insert(g.engine).second) return "duplicate engine filename: " + g.engine;
    }
    return std::nullopt;
}

const ModelGeneration* find_by_engine(const Manifest& m, const std::string& engine) {
    for (const auto& g : m.generations)
        if (g.engine == engine) return &g;
    return nullptr;
}
}  // namespace denso::models
