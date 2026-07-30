#include "health/integrity.h"

#include "level/repo.h"

#include "db/db.h"
#include "detection/class_names.h"
#include "detection/detection.h"
#include "models/compatibility.h"
#include "models/manifest.h"
#include "models/model_identity.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSqlError>
#include <QSqlQuery>
#include <optional>
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
        case ZoneIssue::Kind::ModelCompatibilityRejected:
            return QStringLiteral("model_compatibility_rejected");
        case ZoneIssue::Kind::LevelCalibrationInvalid:
            return QStringLiteral("level_calibration_invalid");
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

IntegrityVerdict evaluate_db_schema(const QString& db_path) {
    IntegrityVerdict v;

    // A missing DB is a fresh install: there is nothing to migrate and nothing
    // is newer. Classifying must NOT create it (mirrors the --check contract).
    if (!QFileInfo::exists(db_path)) {
        return v;  // Ready
    }

    // Read-only so this preflight never mutates the primary database — the same
    // call is safe for --check and for boot BEFORE its read-write open+migrate.
    auto ro = denso::db::Db::open_read_only(db_path);
    if (!ro) {
        v.blockers.push_back({GlobalBlocker::Kind::DbUnopenable, db_path});
        v.status = Readiness::Blocked;
        return v;
    }
    const std::optional<int> ver = denso::db::read_user_version(ro->handle());
    if (!ver) {
        // The file exists and opened but its schema version is unreadable — it is
        // not a usable SQLite database. A no-restart-fixes configuration fault.
        v.blockers.push_back({GlobalBlocker::Kind::DbUnopenable,
            QStringLiteral("cannot read schema version: %1").arg(db_path)});
        v.status = Readiness::Blocked;
        return v;
    }
    const int supported = denso::db::supported_schema_version();
    if (*ver > supported) {
        v.blockers.push_back({GlobalBlocker::Kind::SchemaNewer,
            QStringLiteral("database schema v%1 is newer than supported v%2 (%3)")
                .arg(*ver).arg(supported).arg(db_path)});
        v.status = Readiness::Blocked;
        return v;
    }

    // A valid header + readable user_version does NOT prove the store is sound: a
    // damaged b-tree page deeper in the file would otherwise be waved through as
    // Ready and only surface on a later query. Probe structurally so a corrupt DB
    // fails CLOSED here (DbUnopenable), not mid-run on the appliance.
    if (!denso::db::quick_check(ro->handle())) {
        v.blockers.push_back({GlobalBlocker::Kind::DbUnopenable,
            QStringLiteral("database failed integrity check (quick_check): %1").arg(db_path)});
        v.status = Readiness::Blocked;
        return v;
    }
    return v;  // Ready — an older-or-equal, structurally sound schema migrates normally
}

IntegrityVerdict evaluate_integrity(const QSqlDatabase& db, const QString& models_dir,
                                    denso::mode::TargetMode mode,
                                    const denso::models::ManifestView& view,
                                    const denso::models::PlatformInfo& platform) {
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
    }

    // BOOKKEEPING, NOT AUTHORIZATION. The manifested set is the union of every
    // runtime artifact filename DECLARED across both backend blocks, for every
    // generation — irrespective of the committed mode and irrespective of the
    // backend this build actually runs. A file is "manifested" because it is
    // described, not because it is usable here.
    //
    // Reading the raw g.engine would have been silently wrong once schema 2
    // nested the filenames: the root field is empty there, so an appliance's own
    // correctly-declared engine would report EnginesUnmanifested and degrade a box
    // that is in fact healthy.
    //
    // Declarations are taken from the PASSED VIEW, not from the parse above, so
    // this function has exactly ONE authority on what is declared — the same one
    // the compatibility resolution below consults. The parse above exists solely
    // to answer "is the file corrupt?", which a ManifestView cannot express
    // (load_manifest_view collapses a corrupt file to an empty manifest). In
    // production both come from the same path, and a corrupt file has already
    // returned Blocked, so the two can never disagree — but deriving the set from
    // the view means they cannot drift even in principle.
    for (const auto& g : view.manifest().generations)
        for (const auto& f : g.declared_runtime_files()) manifested.insert(f);

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

    // ── Per-zone: every camera-attached engine must exist on disk, and must be
    //    ALLOWED by the central compatibility policy for the committed mode ────
    //
    // Both checks are camera-scoped and both are Degraded, so a row that fails
    // both reports both — the file-existence fault and the policy fault are
    // independent diagnoses, and suppressing either would hide a real cause.
    //
    // NOTE the deliberate asymmetry with the on-disk scan above: only ATTACHED
    // models are judged here. A declared, valid artifact that no camera uses is a
    // NORMAL installation state and must not degrade the appliance (spec §5.1) —
    // reporting an ordinary, intended state as damage would train operators to
    // ignore the field that exists to tell them something is wrong.
    // MODE-SCOPED (Slice 1). `camera_model` is digit_reader's table, and because a
    // mode switch is now non-destructive its rows PERSIST while ball_leveler is
    // active. Judging those dormant rows against ball_leveler would report
    // model_mode_incompatible for a perfectly healthy appliance and degrade it
    // permanently. So the ACTIVE mode's own configuration is what gets checked;
    // the inactive mode's is dormant, not damaged.
    if (mode == denso::mode::TargetMode::BallLeveler) {
        // Every camera the Ball runtime would admit needs a calibration whose
        // geometry validates. Camera-scoped and Degraded - a missing calibration is
        // a per-camera setup gap, never a whole-machine blocker.
        QSqlQuery bq(db);
        if (!bq.exec(QStringLiteral(
                "SELECT id FROM camera WHERE active = 1 ORDER BY id"))) {
            v.blockers.push_back(
                {GlobalBlocker::Kind::DbQueryFailed, bq.lastError().text()});
            v.status = Readiness::Blocked;
            return v;
        }
        std::vector<int64_t> active_cameras;
        while (bq.next()) active_cameras.push_back(bq.value(0).toLongLong());
        // A fetch error would silently SHORTEN the fleet, so cameras that were
        // never examined would look healthy.
        if (bq.lastError().isValid()) {
            v.blockers.push_back(
                {GlobalBlocker::Kind::DbQueryFailed, bq.lastError().text()});
            v.status = Readiness::Blocked;
            return v;
        }

        // The calibration table is read UNCONDITIONALLY, before the per-camera
        // loop and regardless of how many cameras are active. With no active
        // camera the loop below never runs, so a missing or broken
        // `ball_level_calibration` would otherwise go unqueried and the appliance
        // would report READY on a corrupt database. A schema fault is
        // infrastructure: Blocked / exit 78, never Degraded and never Ready.
        if (!denso::level::try_cameras_with_valid_config(db)) {
            v.blockers.push_back(
                {GlobalBlocker::Kind::DbQueryFailed,
                 QStringLiteral("ball_level_calibration is unreadable")});
            v.status = Readiness::Blocked;
            return v;
        }

        for (int64_t cam : active_cameras) {
            // Fallible read: a FAILED query is a broken database (Blocked / 78),
            // an ABSENT row is an uncalibrated camera (Degraded / 10). Conflating
            // them would report a corrupt table as a routine setup gap.
            const auto probe = denso::level::try_level_config_for(db, cam);
            if (!probe) {
                v.blockers.push_back(
                    {GlobalBlocker::Kind::DbQueryFailed,
                     QStringLiteral("ball_level_calibration read failed for camera %1")
                         .arg(cam)});
                v.status = Readiness::Blocked;
                return v;
            }
            const auto& cfg = *probe;
            if (!cfg) {
                v.issues.push_back(
                    {ZoneIssue::Kind::LevelCalibrationInvalid, cam,
                     QStringLiteral("camera %1: no Ball Leveler calibration").arg(cam),
                     QStringLiteral("level_calibration_missing")});
                continue;
            }
            const auto check = denso::level::validate_calibration(cfg->calibration);
            if (!check.ok) {
                v.issues.push_back(
                    {ZoneIssue::Kind::LevelCalibrationInvalid, cam,
                     QStringLiteral("camera %1: Ball Leveler calibration invalid (%2)")
                         .arg(cam)
                         .arg(QString::fromStdString(check.reason_code)),
                     QString::fromStdString(check.reason_code)});
                continue;
            }
            // The bound Float model is judged by the SAME central policy - one
            // authority, two callers.
            QSqlQuery mq(db);
            mq.prepare(QStringLiteral(
                "SELECT filename, class_names FROM model WHERE id = ?"));
            mq.addBindValue(static_cast<qlonglong>(cfg->model_id));
            if (!mq.exec()) {
                v.blockers.push_back(
                    {GlobalBlocker::Kind::DbQueryFailed, mq.lastError().text()});
                v.status = Readiness::Blocked;
                return v;
            }
            if (!mq.next()) {
                // Absent row vs fetch failure: only the former is "undeclared".
                if (mq.lastError().isValid()) {
                    v.blockers.push_back(
                        {GlobalBlocker::Kind::DbQueryFailed, mq.lastError().text()});
                    v.status = Readiness::Blocked;
                    return v;
                }
                v.issues.push_back(
                    {ZoneIssue::Kind::ModelCompatibilityRejected, cam,
                     QStringLiteral("camera %1: bound model #%2 is not in the catalog")
                         .arg(cam)
                         .arg(cfg->model_id),
                     QStringLiteral("model_undeclared")});
                continue;
            }
            denso::detection::DetectionModel brow;
            brow.filename = mq.value(0).toString().toStdString();
            brow.class_names = denso::detection::parse_class_names(
                mq.value(1).toString().toStdString());
            const QString safe = QString::fromStdString(
                denso::models::diagnostic_filename(brow.filename));
            if (!QFileInfo::exists(
                    dir.filePath(QString::fromStdString(brow.filename)))) {
                v.issues.push_back(
                    {ZoneIssue::Kind::EngineMissing, cam,
                     QStringLiteral("%1 (model #%2)").arg(safe).arg(cfg->model_id),
                     {}});
            }
            const denso::models::ModelMetadata bmd =
                denso::models::resolve_model_metadata(view, brow, platform);
            const auto bverdict = denso::models::model_compatibility(mode, bmd);
            if (!bverdict.allowed()) {
                v.issues.push_back(
                    {ZoneIssue::Kind::ModelCompatibilityRejected, cam,
                     QStringLiteral("camera %1: model %2 (%3) rejected: %4")
                         .arg(cam)
                         .arg(safe,
                              bmd.canonical_id.empty()
                                  ? QStringLiteral("<undeclared>")
                                  : QString::fromStdString(bmd.canonical_id),
                              QString::fromStdString(bverdict.reason_code)),
                     QString::fromStdString(bverdict.reason_code)});
            }
        }
        if (!v.blockers.empty())    v.status = Readiness::Blocked;
        else if (!v.issues.empty()) v.status = Readiness::Degraded;
        else                        v.status = Readiness::Ready;
        return v;
    }

    QSqlQuery q(db);
    if (!q.exec(QStringLiteral(
            "SELECT cm.camera_id, m.filename, m.class_names, m.id FROM camera_model cm "
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
        const int64_t model_id  = q.value(3).toLongLong();
        // Same database-controlled column, same redaction rule as below: the detail
        // reaches status.json, so the NAME is reduced to a plain filename or to
        // "<invalid>". The catalog ROW ID is emitted beside it, unconditionally: it
        // is an integer, so it can never carry a secret, and it keeps a model whose
        // name cannot safely be printed precisely identifiable. For every ordinary
        // filename this is the identity string EngineMissing has always reported,
        // plus the id.
        const QString safe_filename = QString::fromStdString(
            denso::models::diagnostic_filename(filename.toStdString()));
        const QString model_ref =
            QStringLiteral("%1 (model #%2)").arg(safe_filename).arg(model_id);
        if (!QFileInfo::exists(dir.filePath(filename))) {
            v.issues.push_back(
                {ZoneIssue::Kind::EngineMissing, camera_id, model_ref, {}});
        }

        // Identity is resolved from the DECLARATION, never inferred from the
        // filename, the display name or the class signature. The rules live in
        // exactly one place — models::model_compatibility — and this is a caller
        // of it, not a second copy.
        denso::detection::DetectionModel row;
        row.filename = filename.toStdString();
        row.class_names =
            denso::detection::parse_class_names(q.value(2).toString().toStdString());
        const denso::models::ModelMetadata md =
            denso::models::resolve_model_metadata(view, row, platform);
        const auto verdict = denso::models::model_compatibility(mode, md);
        if (!verdict.allowed()) {
            // Redaction-safe detail (spec §12): camera id + model identity only.
            // No camera row is read here at all, so no camera URL/username/password
            // can reach the diagnostic. The FILENAME, however, is a database column
            // that a restored or hand-edited row controls — exactly the database
            // this check exists for — so it is reduced before it can carry a
            // credential-bearing URL into status.json, and the catalog row id is
            // carried alongside so an unprintable name is still identifiable.
            const QString canonical =
                md.canonical_id.empty() ? QStringLiteral("<undeclared>")
                                        : QString::fromStdString(md.canonical_id);
            v.issues.push_back(
                {ZoneIssue::Kind::ModelCompatibilityRejected, camera_id,
                 QStringLiteral("camera %1: model %2 (%3) rejected: %4")
                     .arg(camera_id)
                     .arg(model_ref, canonical,
                          QString::fromStdString(verdict.reason_code)),
                 QString::fromStdString(verdict.reason_code)});
        }
    }

    if (!v.blockers.empty())      v.status = Readiness::Blocked;
    else if (!v.issues.empty())   v.status = Readiness::Degraded;
    else                          v.status = Readiness::Ready;
    return v;
}

} // namespace denso::health
