#include "cli/migrate_coordinator.h"
#include <QByteArray>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <map>
#include <optional>
#include <string>
#include "db/db.h"
#include "detection/migrate.h"
#include "models/hashing.h"
#include "models/manifest.h"
namespace denso::cli {
namespace {
MigrateOutcome fail(int code, const QString& slug, const QString& msg) {
    QJsonObject o; o["ok"] = false; o["error"] = msg; o["code"] = slug;
    return {code, QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)), msg};
}
std::optional<std::string> read_file(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return std::nullopt;
    return f.readAll().toStdString();
}
}  // namespace

MigrateOutcome run_migrate(const MigrateInputs& in) {
    // (1) optional class-map: strict JSON object of string->string
    std::map<std::string, std::string> remap;
    if (!in.class_map_path.isEmpty()) {
        auto text = read_file(in.class_map_path);
        if (!text) return fail(2, "bad-class-map", "cannot read class-map: " + in.class_map_path);
        QJsonParseError perr{};
        const auto doc = QJsonDocument::fromJson(QByteArray::fromStdString(*text), &perr);
        if (perr.error != QJsonParseError::NoError || !doc.isObject())
            return fail(2, "bad-class-map", "class-map is not a JSON object");
        const auto obj = doc.object();
        for (auto it = obj.begin(); it != obj.end(); ++it) {
            if (!it.value().isString())
                return fail(2, "bad-class-map", "class-map values must be strings");
            remap[it.key().toStdString()] = it.value().toString().toStdString();
        }
    }
    // (2) open DB + migrate (Db::open does NOT migrate — Task 8's missing step)
    auto db = denso::db::Db::open(in.db_path);
    if (!db) return fail(3, "db-open-failed", "cannot open database: " + in.db_path);
    if (!denso::db::run_migrations(db->handle()))
        return fail(3, "db-open-failed", "run_migrations failed");
    // (3) manifest load + parse + validate
    const QString manifest_path = QDir(in.models_dir).filePath("manifest.json");
    auto mtext = read_file(manifest_path);
    if (!mtext) return fail(4, "bad-manifest", "cannot read manifest: " + manifest_path);
    auto pr = denso::models::parse_manifest(*mtext);
    if (!pr.manifest) return fail(4, "bad-manifest", "parse: " + QString::fromStdString(pr.error));
    if (auto verr = denso::models::validate_manifest(*pr.manifest))
        return fail(4, "bad-manifest", "validate: " + QString::fromStdString(*verr));
    // (4) find the target generation
    const std::string new_engine = in.new_engine.toStdString();
    const auto* gen = denso::models::find_by_engine(*pr.manifest, new_engine);
    if (!gen) return fail(5, "no-such-generation",
                          "manifest has no generation for engine " + in.new_engine);
    // (5) canonicalize + assert engine & sidecar resolve UNDER models_dir (symlink-escape guard)
    const QString canon_dir = QFileInfo(in.models_dir).canonicalFilePath();
    if (canon_dir.isEmpty()) return fail(6, "path-escape", "models_dir does not exist: " + in.models_dir);
    const QString prefix = canon_dir.endsWith('/') ? canon_dir : canon_dir + '/';
    const QString canon_engine =
        QFileInfo(QDir(in.models_dir).filePath(QString::fromStdString(gen->engine))).canonicalFilePath();
    const QString canon_sidecar =
        QFileInfo(QDir(in.models_dir).filePath(QString::fromStdString(gen->sidecar))).canonicalFilePath();
    if (canon_engine.isEmpty() || !canon_engine.startsWith(prefix))
        return fail(6, "path-escape", "engine resolves outside models_dir: " + in.new_engine);
    if (canon_sidecar.isEmpty() || !canon_sidecar.startsWith(prefix))
        return fail(6, "path-escape",
                    "sidecar resolves outside models_dir: " + QString::fromStdString(gen->sidecar));
    // (6) content hashes must equal the manifest
    auto eh = denso::models::file_sha256(canon_engine);
    if (!eh || *eh != gen->engine_sha256)
        return fail(7, "hash-mismatch", "engine sha256 mismatch for " + in.new_engine);
    auto sh = denso::models::file_sha256(canon_sidecar);
    if (!sh || *sh != gen->sidecar_sha256)
        return fail(7, "hash-mismatch",
                    "sidecar sha256 mismatch for " + QString::fromStdString(gen->sidecar));
    // (7) build request + run the transaction
    denso::detection::MigrateRequest req;
    req.old_filename = in.old_engine.toStdString();
    req.new_filename = new_engine;
    req.new_name = gen->name;
    req.new_engine_sha256 = gen->engine_sha256;
    req.new_class_names = gen->class_names;
    req.explicit_remap = remap;
    for (qint64 c : in.cameras) req.camera_ids.push_back(static_cast<int64_t>(c));
    req.created_utc = QDateTime::currentDateTimeUtc().toString(Qt::ISODate).toStdString();
    auto res = denso::detection::migrate_model(db->handle(), req);
    if (!res.ok) return fail(8, "migrate-failed", QString::fromStdString(res.error));
    QJsonObject ok; ok["ok"] = true;
    QJsonArray cams; for (auto c : res.affected_cameras)
        cams.append(QString::number(static_cast<qlonglong>(c)));
    ok["cameras"] = cams;
    return {0, QString::fromUtf8(QJsonDocument(ok).toJson(QJsonDocument::Compact)), {}};
}
}  // namespace denso::cli
