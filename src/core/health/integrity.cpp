#include "health/integrity.h"

#include "models/manifest.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSqlError>
#include <QSqlQuery>
#include <set>
#include <string>

namespace denso::health {
namespace {

bool read_text(const QString& path, std::string& out) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return false;
    out = f.readAll().toStdString();
    return true;
}

} // namespace

QString reason_code(GlobalBlocker::Kind k) {
    switch (k) {
        case GlobalBlocker::Kind::DbUnopenable:        return QStringLiteral("db_unopenable");
        case GlobalBlocker::Kind::SchemaNewer:         return QStringLiteral("schema_newer");
        case GlobalBlocker::Kind::MigrationFailed:     return QStringLiteral("migration_failed");
        case GlobalBlocker::Kind::DbQueryFailed:       return QStringLiteral("db_query_failed");
        case GlobalBlocker::Kind::ModelsDirUnreadable: return QStringLiteral("models_dir_unreadable");
        case GlobalBlocker::Kind::ManifestCorrupt:     return QStringLiteral("manifest_corrupt");
    }
    return QStringLiteral("unknown");
}

QString reason_code(ZoneIssue::Kind k) {
    switch (k) {
        case ZoneIssue::Kind::EngineMissing:       return QStringLiteral("engine_missing");
        case ZoneIssue::Kind::EnginesUnmanifested: return QStringLiteral("engines_unmanifested");
    }
    return QStringLiteral("unknown");
}

int exit_code_for(Readiness r) {
    switch (r) {
        case Readiness::Ready:    return 0;
        case Readiness::Degraded: return 10;
        case Readiness::Blocked:  return 78;   // EX_CONFIG
    }
    return 78;
}

IntegrityVerdict evaluate_integrity(const QSqlDatabase& db, const QString& models_dir) {
    IntegrityVerdict v;

    // ── Global: the models dir must exist and be listable ────────────────────
    QDir dir(models_dir);
    if (!QFileInfo(models_dir).isDir() || !dir.isReadable()) {
        v.blockers.push_back({GlobalBlocker::Kind::ModelsDirUnreadable, models_dir});
        v.status = Readiness::Blocked;
        return v;
    }

    // ── Global: a manifest, if present, must parse and validate ──────────────
    std::set<std::string> manifested;
    const QString manifest_path = dir.filePath(QStringLiteral("manifest.json"));
    const bool has_manifest = QFileInfo::exists(manifest_path);
    if (has_manifest) {
        std::string text;
        if (!read_text(manifest_path, text)) {
            v.blockers.push_back({GlobalBlocker::Kind::ManifestCorrupt,
                                  QStringLiteral("unreadable: %1").arg(manifest_path)});
            v.status = Readiness::Blocked;
            return v;
        }
        auto pr = denso::models::parse_manifest(text);
        if (!pr.manifest) {
            v.blockers.push_back({GlobalBlocker::Kind::ManifestCorrupt,
                                  QString::fromStdString(pr.error)});
            v.status = Readiness::Blocked;
            return v;
        }
        if (auto err = denso::models::validate_manifest(*pr.manifest)) {
            v.blockers.push_back({GlobalBlocker::Kind::ManifestCorrupt,
                                  QString::fromStdString(*err)});
            v.status = Readiness::Blocked;
            return v;
        }
        for (const auto& g : pr.manifest->generations) manifested.insert(g.engine);
    }

    // ── Degraded: engines on disk that the manifest does not describe ────────
    // COMPATIBILITY (spec §2.3): the production Jetson has engines and no
    // manifest, and works only because sync_models() scans the directory. This
    // is reported as actionable, and MUST NEVER block.
    const QStringList on_disk =
        dir.entryList({QStringLiteral("*.engine"), QStringLiteral("*.onnx")}, QDir::Files);
    for (const QString& f : on_disk) {
        if (manifested.count(f.toStdString()) == 0) {
            v.issues.push_back({ZoneIssue::Kind::EnginesUnmanifested, 0, f});
        }
    }

    // ── Per-zone: every camera-attached engine must exist on disk ────────────
    QSqlQuery q(db);
    if (!q.exec(QStringLiteral(
            "SELECT cm.camera_id, m.filename FROM camera_model cm "
            "JOIN model m ON m.id = cm.model_id"))) {
        // A FAILED query is a global blocker. Conflating it with "no rows" would
        // turn a broken DB into a silently empty fleet (spec §2.2).
        v.blockers.push_back({GlobalBlocker::Kind::DbQueryFailed, q.lastError().text()});
        v.status = Readiness::Blocked;
        return v;
    }
    while (q.next()) {
        const int64_t camera_id = q.value(0).toLongLong();
        const QString filename  = q.value(1).toString();
        if (!QFileInfo::exists(dir.filePath(filename))) {
            v.issues.push_back({ZoneIssue::Kind::EngineMissing, camera_id, filename});
        }
    }

    if (!v.blockers.empty())      v.status = Readiness::Blocked;
    else if (!v.issues.empty())   v.status = Readiness::Degraded;
    else                          v.status = Readiness::Ready;
    return v;
}

} // namespace denso::health
