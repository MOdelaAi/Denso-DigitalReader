#include "detection/repo.h"

#include "detection/class_names.h"

#include <QSqlQuery>
#include <QString>
#include <QVariant>

#include <utility>

namespace denso::detection {

std::optional<int64_t> upsert_model(const QSqlDatabase& db, const DetectionModel& m) {
    QSqlQuery q(db);
    // UPSERT on the unique filename; RETURNING gives the row id for both paths.
    q.prepare(QStringLiteral(
        "INSERT INTO model (name, filename, class_names) VALUES (?, ?, ?) "
        "ON CONFLICT(filename) DO UPDATE SET name=excluded.name, "
        "class_names=excluded.class_names "
        "RETURNING id"));
    q.addBindValue(QString::fromStdString(m.name));
    q.addBindValue(QString::fromStdString(m.filename));
    q.addBindValue(QString::fromStdString(serialize_class_names(m.class_names)));
    if (!q.exec() || !q.next()) {
        return std::nullopt;
    }
    return q.value(0).toLongLong();
}

std::vector<DetectionModel> list_models(const QSqlDatabase& db) {
    std::vector<DetectionModel> out;
    QSqlQuery q(db);
    if (!q.exec(QStringLiteral(
            "SELECT id, name, filename, class_names FROM model ORDER BY id"))) {
        return out;
    }
    while (q.next()) {
        DetectionModel m;
        m.id = q.value(0).toLongLong();
        m.name = q.value(1).toString().toStdString();
        m.filename = q.value(2).toString().toStdString();
        m.class_names = parse_class_names(q.value(3).toString().toStdString());
        out.push_back(std::move(m));
    }
    return out;
}

std::vector<SelectableModel> selectable_models(
    const QSqlDatabase& db, denso::mode::TargetMode mode,
    const denso::models::ManifestView& view,
    const denso::models::PlatformInfo& platform) {
    std::vector<SelectableModel> out;
    // list_models is already ORDER BY id, and this only ever DROPS entries, so
    // catalog-id order is preserved by construction — no second sort, and nothing
    // that could reorder the survivors.
    for (DetectionModel& row : list_models(db)) {
        // Resolve for the view's ACTIVE backend, then ask the ONE policy. Identity
        // comes from the declaration; nothing here inspects the filename, the
        // display name or the class names to decide availability.
        denso::models::ModelMetadata md =
            denso::models::resolve_model_metadata(view, row, platform);
        if (!denso::models::model_compatibility(mode, md).allowed()) {
            continue;
        }
        out.push_back(SelectableModel{std::move(row), std::move(md)});
    }
    return out;
}

std::optional<std::vector<std::string>>
try_attached_model_filenames(const QSqlDatabase& db, denso::mode::TargetMode mode,
                             const denso::models::ManifestView& view,
                             const denso::models::PlatformInfo& platform) {
    // Select the full row needed to resolve identity — the filename joins the
    // manifest generation, the class_names corroborate it. filename is UNIQUE in
    // `model`, so DISTINCT m.filename,m.class_names dedupes exactly as before.
    std::vector<std::string> out;
    QSqlQuery q(db);
    if (!q.exec(QStringLiteral(
            "SELECT DISTINCT m.filename, m.class_names FROM camera_model cm "
            "JOIN model m ON m.id = cm.model_id ORDER BY m.filename"))) {
        return std::nullopt;
    }
    while (q.next()) {
        DetectionModel row;
        row.filename = q.value(0).toString().toStdString();
        row.class_names = parse_class_names(q.value(1).toString().toStdString());
        // Resolve for the active backend and keep ONLY what the policy allows.
        // A rejected attachment is dropped here so it never reaches the warm-up
        // required set (spec §7.0). The returned name is the active-backend
        // filename from the resolved metadata.
        const denso::models::ModelMetadata md =
            denso::models::resolve_model_metadata(view, row, platform);
        if (denso::models::model_compatibility(mode, md).allowed()) {
            out.push_back(md.filename);
        }
    }
    return out;
}

std::vector<std::string> attached_model_filenames(
    const QSqlDatabase& db, denso::mode::TargetMode mode,
    const denso::models::ManifestView& view,
    const denso::models::PlatformInfo& platform) {
    return try_attached_model_filenames(db, mode, view, platform)
        .value_or(std::vector<std::string>{});
}

static std::vector<ModelClassSelection> classes_for(const QSqlDatabase& db,
                                                    int64_t camera_model_id) {
    std::vector<ModelClassSelection> out;
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "SELECT class_id, conf FROM camera_model_class "
        "WHERE camera_model_id = ? ORDER BY id"));
    q.addBindValue(static_cast<qlonglong>(camera_model_id));
    if (!q.exec()) {
        return out;
    }
    while (q.next()) {
        out.push_back(ModelClassSelection{q.value(0).toInt(),
                                          q.value(1).toFloat()});
    }
    return out;
}

std::vector<CameraModel> models_for(const QSqlDatabase& db, int64_t camera_id) {
    std::vector<CameraModel> out;
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "SELECT id, camera_id, model_id FROM camera_model "
        "WHERE camera_id = ? ORDER BY id"));
    q.addBindValue(static_cast<qlonglong>(camera_id));
    if (!q.exec()) {
        return out;
    }
    while (q.next()) {
        CameraModel cm;
        cm.id = q.value(0).toLongLong();
        cm.camera_id = q.value(1).toLongLong();
        cm.model_id = q.value(2).toLongLong();
        cm.classes = classes_for(db, cm.id);
        out.push_back(std::move(cm));
    }
    return out;
}

namespace {

/// Result of a catalog lookup. The three states must stay distinct:
///   Ok        — the row was read.
///   NoRow     — the catalog genuinely has no such model. That is a REFUSAL
///               reason: an attachment naming a model that does not exist can
///               never be resolved, so it can never be Allowed.
///   QueryFailed — the DATABASE is broken. That is NOT a compatibility verdict,
///               and must never be reported as one: telling an operator their
///               model is "undeclared" when the real fault is a corrupt schema
///               sends them to inspect a manifest that was never the problem.
enum class RowLookup { Ok, NoRow, QueryFailed };

RowLookup model_row(const QSqlDatabase& db, int64_t model_id, DetectionModel& out) {
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "SELECT id, name, filename, class_names FROM model WHERE id = ?"));
    q.addBindValue(static_cast<qlonglong>(model_id));
    if (!q.exec()) {
        return RowLookup::QueryFailed;
    }
    if (!q.next()) {
        return RowLookup::NoRow;
    }
    out.id = q.value(0).toLongLong();
    out.name = q.value(1).toString().toStdString();
    out.filename = q.value(2).toString().toStdString();
    out.class_names = parse_class_names(q.value(3).toString().toStdString());
    return RowLookup::Ok;
}

/// The canonical id for a diagnostic, or the explicit "<undeclared>" sentinel.
/// Identity is NEVER inferred from a filename, so an undeclared model genuinely
/// has no canonical id to report and must say so rather than borrowing its stem.
std::string diag_canonical_id(const denso::models::ModelMetadata& md) {
    return md.canonical_id.empty() ? std::string("<undeclared>") : md.canonical_id;
}

}  // namespace

bool set_camera_models(const QSqlDatabase& db, int64_t camera_id,
                       const std::vector<CameraModel>& models,
                       denso::mode::TargetMode mode,
                       const denso::models::ManifestView& view,
                       const denso::models::PlatformInfo& platform,
                       AttachRefusal* refusal) {
    QSqlDatabase conn(db);
    if (!conn.transaction()) {
        return false;
    }
    const auto rollback = [&conn] { conn.rollback(); return false; };

    // ── Compatibility gate (spec §7.1) ───────────────────────────────────────
    // Runs INSIDE the transaction but BEFORE any mutation, so a refusal rolls the
    // whole thing back having changed nothing — not even the delete of the
    // previous set. The whole requested set is judged first: a mixed
    // allowed/rejected request must store NOTHING, so the loop cannot begin
    // writing the allowed members and discover the rejected one later.
    //
    // An empty `models` (detach everything) has nothing to judge and is allowed in
    // every mode — the operator must always be able to clear an attachment.
    for (const CameraModel& cm : models) {
        DetectionModel row;
        const RowLookup lookup = model_row(db, cm.model_id, row);
        if (lookup == RowLookup::QueryFailed) {
            // A broken database, not a rejected model. Roll back and fail plainly,
            // leaving `refusal` UNTOUCHED so the caller reports a write failure
            // rather than inventing a compatibility reason for it.
            return rollback();
        }
        const denso::models::ModelMetadata md =
            lookup == RowLookup::Ok
                ? denso::models::resolve_model_metadata(view, row, platform)
                : denso::models::ModelMetadata{};  // no catalog row → undeclared
        const auto verdict = denso::models::model_compatibility(mode, md);
        if (!verdict.allowed()) {
            if (refusal) {
                refusal->camera_id = camera_id;
                refusal->model_id = cm.model_id;
                refusal->canonical_id = diag_canonical_id(md);
                // The catalog filename when the row exists, reduced to its
                // basename: this string is shown to an operator and logged, and
                // the column is writable by hand, so it must not be able to carry
                // a credential-bearing URL (spec §12).
                refusal->filename =
                    lookup == RowLookup::Ok
                        ? denso::models::diagnostic_filename(row.filename)
                        : std::string();
                refusal->policy_reason = verdict.reason_code;
            }
            return rollback();
        }
    }

    // Delete children first (class rows for this camera's attachments), then
    // the attachments themselves.
    QSqlQuery delc(db);
    delc.prepare(QStringLiteral(
        "DELETE FROM camera_model_class WHERE camera_model_id IN "
        "(SELECT id FROM camera_model WHERE camera_id = ?)"));
    delc.addBindValue(static_cast<qlonglong>(camera_id));
    if (!delc.exec()) {
        return rollback();
    }
    QSqlQuery delm(db);
    delm.prepare(QStringLiteral("DELETE FROM camera_model WHERE camera_id = ?"));
    delm.addBindValue(static_cast<qlonglong>(camera_id));
    if (!delm.exec()) {
        return rollback();
    }

    for (const CameraModel& cm : models) {
        QSqlQuery ins(db);
        ins.prepare(QStringLiteral(
            "INSERT INTO camera_model (camera_id, model_id) VALUES (?, ?)"));
        ins.addBindValue(static_cast<qlonglong>(camera_id));
        ins.addBindValue(static_cast<qlonglong>(cm.model_id));
        if (!ins.exec()) {
            return rollback();
        }
        const qlonglong cmid = ins.lastInsertId().toLongLong();
        for (const ModelClassSelection& s : cm.classes) {
            QSqlQuery insc(db);
            insc.prepare(QStringLiteral(
                "INSERT INTO camera_model_class (camera_model_id, class_id, conf) "
                "VALUES (?, ?, ?)"));
            insc.addBindValue(cmid);
            insc.addBindValue(s.class_id);
            insc.addBindValue(static_cast<double>(s.conf));
            if (!insc.exec()) {
                return rollback();
            }
        }
    }
    return conn.commit() || rollback();
}

CameraDetection detection_for(const QSqlDatabase& db, int64_t camera_id,
                              denso::mode::TargetMode mode,
                              const denso::models::ManifestView& view,
                              const denso::models::PlatformInfo& platform) {
    CameraDetection out;
    out.camera_id = camera_id;
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "SELECT cm.id, m.filename, m.class_names "
        "FROM camera_model cm JOIN model m ON m.id = cm.model_id "
        "WHERE cm.camera_id = ? ORDER BY cm.id"));
    q.addBindValue(static_cast<qlonglong>(camera_id));
    if (!q.exec()) {
        return out;
    }
    while (q.next()) {
        ResolvedModel rm;
        const int64_t cmid = q.value(0).toLongLong();
        rm.filename = q.value(1).toString().toStdString();
        rm.class_names = parse_class_names(q.value(2).toString().toStdString());
        rm.classes = classes_for(db, cmid);

        // Runtime enforcement (spec §7.2). The row may have been written by hand
        // or restored from a backup, so the write-path gate above cannot be
        // assumed to have run: judge EVERY attachment here, at read time.
        DetectionModel row;
        row.filename = rm.filename;
        row.class_names = rm.class_names;
        const denso::models::ModelMetadata md =
            denso::models::resolve_model_metadata(view, row, platform);
        const auto verdict = denso::models::model_compatibility(mode, md);
        if (!verdict.allowed()) {
            // Inhibit the camera AS A WHOLE. Returning the allowed subset would
            // silently change what this camera reports; returning it empty WITHOUT
            // the flag would let the caller demote it to orientation-only, which
            // looks like a working camera that has quietly stopped reading.
            out.models.clear();
            out.compatibility_rejected = true;
            out.policy_reason = verdict.reason_code;
            return out;
        }
        out.models.push_back(std::move(rm));
    }
    return out;
}

} // namespace denso::detection
