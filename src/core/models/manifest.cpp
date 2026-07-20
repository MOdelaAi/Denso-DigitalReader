#include "models/manifest.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
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
