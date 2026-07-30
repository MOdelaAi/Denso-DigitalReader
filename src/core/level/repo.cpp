#include "level/repo.h"

#include "detection/class_names.h"
#include "detection/detection.h"

#include <QSqlError>
#include <QSqlQuery>
#include <QString>
#include <QVariant>

namespace denso::level {
namespace {

enum class RowLookup { Ok, Missing, QueryFailed };

/// The catalog row for a model id. Mirrors detection/repo's private helper: a
/// MISSING row and a FAILED query must stay distinguishable, because a missing row
/// is an undeclared model (a policy refusal) while a failed query is a broken
/// database (a write failure), and reporting one as the other would be a lie.
RowLookup model_row(const QSqlDatabase& db, int64_t model_id,
                    denso::detection::DetectionModel& out) {
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "SELECT id, name, filename, class_names FROM model WHERE id = ?"));
    q.addBindValue(static_cast<qlonglong>(model_id));
    if (!q.exec()) return RowLookup::QueryFailed;
    if (!q.next()) {
        // A FETCH failure is a broken database, not an undeclared model.
        // Collapsing it into Missing would make save_level_configuration
        // report a disk fault as a POLICY refusal - the caller would tell the
        // operator their model is incompatible when the database is broken.
        if (q.lastError().isValid()) return RowLookup::QueryFailed;
        return RowLookup::Missing;
    }
    out.id = q.value(0).toLongLong();
    out.name = q.value(1).toString().toStdString();
    out.filename = q.value(2).toString().toStdString();
    out.class_names =
        denso::detection::parse_class_names(q.value(3).toString().toStdString());
    return RowLookup::Ok;
}

LevelCalibration read_calibration(const QSqlQuery& q, int first) {
    LevelCalibration c;
    c.conf = q.value(first).toDouble();
    c.rect_x = q.value(first + 1).toDouble();
    c.rect_y = q.value(first + 2).toDouble();
    c.rect_w = q.value(first + 3).toDouble();
    c.rect_h = q.value(first + 4).toDouble();
    c.y_100 = q.value(first + 5).toDouble();
    c.y_0 = q.value(first + 6).toDouble();
    c.hold_ms = q.value(first + 7).toInt();
    return c;
}

}  // namespace

bool save_level_configuration(const QSqlDatabase& db, int64_t camera_id,
                              const std::vector<LevelBinding>& models,
                              const LevelCalibration& calibration,
                              const std::string& view_revision,
                              const denso::models::ManifestView& view,
                              const denso::models::PlatformInfo& platform,
                              SaveRefusal* refusal) {
    const auto refuse = [&](const std::string& code, int64_t model_id,
                            const std::string& canonical,
                            const std::string& filename) {
        if (refusal) {
            refusal->camera_id = camera_id;
            refusal->model_id = model_id;
            refusal->canonical_id = canonical;
            refusal->filename = filename;
            refusal->reason_code = code;
        }
        return false;
    };

    // ── Arity, BEFORE any transaction ────────────────────────────────────────
    // Ball Leveler v1 is exactly one model with exactly one class. These are
    // checked first and outside the transaction because they need no database at
    // all: a malformed request is a caller bug, not a persistence failure.
    if (models.size() != 1) {
        return refuse("level_model_count", models.size() == 1 ? models[0].model_id : 0,
                      std::string(), std::string());
    }
    const LevelBinding& binding = models.front();
    if (binding.class_ids.size() != 1) {
        return refuse("level_class_count", binding.model_id, std::string(),
                      std::string());
    }

    QSqlDatabase conn(db);
    if (!conn.transaction()) return false;
    const auto rollback = [&conn] { conn.rollback(); return false; };

    // ── Compatibility gate — INSIDE the transaction, BEFORE any mutation ──────
    // Asks the ONE central policy. This unit holds no family->mode rule of its
    // own; it is one more caller of models::model_compatibility, exactly like
    // detection::set_camera_models.
    denso::detection::DetectionModel row;
    const RowLookup lookup = model_row(db, binding.model_id, row);
    if (lookup == RowLookup::QueryFailed) {
        // A broken database, not a rejected model. Leave `refusal` UNTOUCHED so the
        // caller reports a write failure rather than inventing a policy reason.
        return rollback();
    }
    const denso::models::ModelMetadata md =
        lookup == RowLookup::Ok
            ? denso::models::resolve_model_metadata(view, row, platform)
            : denso::models::ModelMetadata{};  // no catalog row -> undeclared
    // THE MODE IS HARDCODED. A row in this table IS a Ball Leveler binding, so
    // BallLeveler is the only question that can be asked. See repo.h.
    const auto verdict =
        denso::models::model_compatibility(denso::mode::TargetMode::BallLeveler, md);
    if (!verdict.allowed()) {
        conn.rollback();
        // The filename is a database-controlled column, so it is reduced before it
        // can carry a credential-bearing URL into an operator-visible refusal.
        return refuse(verdict.reason_code, binding.model_id,
                      md.canonical_id.empty() ? std::string("<undeclared>")
                                              : md.canonical_id,
                      lookup == RowLookup::Ok
                          ? denso::models::diagnostic_filename(row.filename)
                          : std::string());
    }

    // ── Class MEMBERSHIP gate ────────────────────────────────────────────────
    // Arity ("exactly one class") was checked above; that says nothing about
    // whether the class EXISTS. Without this, class_id -1 or 999 would persist as
    // a durable, unusable binding that no runtime could ever resolve — a silent
    // mis-measurement waiting to happen, written through the very chokepoint that
    // exists to make invalid states unrepresentable.
    //
    // The authority is `md.class_names` — the canonical class list resolved from
    // the manifest/sidecar for the ACTIVE backend, NOT the raw `model.class_names`
    // column. The compatibility gate above already proved the artifact corroborates
    // that declaration (artifact_matches), so this is the same set the runtime will
    // index. No Float class id is hardcoded here or anywhere: the model declares
    // its own classes and this only checks membership.
    const int class_id = binding.class_ids.front();
    if (class_id < 0 ||
        static_cast<size_t>(class_id) >= md.class_names.size()) {
        conn.rollback();
        return refuse("level_class_unknown", binding.model_id, md.canonical_id,
                      denso::models::diagnostic_filename(row.filename));
    }

    // ── Geometry gate ────────────────────────────────────────────────────────
    const CalibrationCheck check = validate_calibration(calibration);
    if (!check.ok) {
        conn.rollback();
        return refuse(check.reason_code, binding.model_id, md.canonical_id,
                      denso::models::diagnostic_filename(row.filename));
    }

    // ── The single row. UPSERT on camera_id, which is the PRIMARY KEY, so one
    //    camera can never accumulate a second measurement configuration.
    QSqlQuery ins(db);
    ins.prepare(QStringLiteral(
        "INSERT INTO ball_level_calibration "
        "(camera_id, model_id, class_id, conf, rect_x, rect_y, rect_w, rect_h, "
        " y_100, y_0, hold_ms, view_revision) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?) "
        "ON CONFLICT(camera_id) DO UPDATE SET "
        "  model_id = excluded.model_id, class_id = excluded.class_id, "
        "  conf = excluded.conf, rect_x = excluded.rect_x, "
        "  rect_y = excluded.rect_y, rect_w = excluded.rect_w, "
        "  rect_h = excluded.rect_h, y_100 = excluded.y_100, "
        "  y_0 = excluded.y_0, hold_ms = excluded.hold_ms, "
        "  view_revision = excluded.view_revision"));
    ins.addBindValue(static_cast<qlonglong>(camera_id));
    ins.addBindValue(static_cast<qlonglong>(binding.model_id));
    ins.addBindValue(class_id);
    ins.addBindValue(calibration.conf);
    ins.addBindValue(calibration.rect_x);
    ins.addBindValue(calibration.rect_y);
    ins.addBindValue(calibration.rect_w);
    ins.addBindValue(calibration.rect_h);
    ins.addBindValue(calibration.y_100);
    ins.addBindValue(calibration.y_0);
    ins.addBindValue(calibration.hold_ms);
    ins.addBindValue(QString::fromStdString(view_revision));
    if (!ins.exec()) return rollback();

    return conn.commit() || rollback();
}

std::optional<std::optional<LevelConfig>> try_level_config_for(
    const QSqlDatabase& db, int64_t camera_id) {
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "SELECT camera_id, model_id, class_id, conf, rect_x, rect_y, rect_w, "
        "rect_h, y_100, y_0, hold_ms, view_revision "
        "FROM ball_level_calibration WHERE camera_id = ?"));
    q.addBindValue(static_cast<qlonglong>(camera_id));
    // exec() and next() are checked SEPARATELY and mean different things: exec()
    // false is a broken table/schema (infrastructure), next() false is simply no
    // row for this camera (an ordinary setup gap). Folding them into one `||`
    // is precisely the bug this function replaces.
    if (!q.exec()) return std::nullopt;
    if (!q.next()) {
        // next() returns false BOTH at end-of-rows and on a FETCH error, so
        // "no row" is only an honest answer once lastError() is clear. Without
        // this, a fault that surfaces during iteration rather than at exec()
        // would still be reported as an uncalibrated camera.
        if (q.lastError().isValid()) return std::nullopt;
        return std::optional<LevelConfig>{};
    }
    LevelConfig c;
    c.camera_id = q.value(0).toLongLong();
    c.model_id = q.value(1).toLongLong();
    c.class_id = q.value(2).toInt();
    c.calibration = read_calibration(q, 3);
    c.view_revision = q.value(11).toString().toStdString();
    return std::optional<LevelConfig>{std::move(c)};
}

std::optional<LevelConfig> level_config_for(const QSqlDatabase& db,
                                            int64_t camera_id) {
    return try_level_config_for(db, camera_id)
        .value_or(std::optional<LevelConfig>{});
}

std::optional<std::vector<int64_t>> try_cameras_with_valid_config(
    const QSqlDatabase& db) {
    std::vector<int64_t> out;
    QSqlQuery q(db);
    if (!q.exec(QStringLiteral(
            "SELECT camera_id, conf, rect_x, rect_y, rect_w, rect_h, y_100, y_0, "
            "hold_ms FROM ball_level_calibration ORDER BY camera_id"))) {
        return std::nullopt;  // undeterminable — NOT "no camera is calibrated"
    }
    while (q.next()) {
        // "Stored" is NOT "valid": the row can be hand-edited or restored from a
        // backup, so the geometry is re-validated on every read.
        if (validate_calibration(read_calibration(q, 1)).ok) {
            out.push_back(q.value(0).toLongLong());
        }
    }
    // A fetch error mid-scan would otherwise return a SHORT list that reads as
    // a valid, complete answer - worse than no answer at all.
    if (q.lastError().isValid()) return std::nullopt;
    return out;
}

std::vector<int64_t> cameras_with_valid_config(const QSqlDatabase& db) {
    return try_cameras_with_valid_config(db).value_or(std::vector<int64_t>{});
}

bool has_valid_config(const QSqlDatabase& db, int64_t camera_id) {
    const auto c = level_config_for(db, camera_id);
    return c.has_value() && validate_calibration(c->calibration).ok;
}

bool clear_level_configuration(const QSqlDatabase& db, int64_t camera_id) {
    QSqlQuery q(db);
    q.prepare(
        QStringLiteral("DELETE FROM ball_level_calibration WHERE camera_id = ?"));
    q.addBindValue(static_cast<qlonglong>(camera_id));
    return q.exec();
}

}  // namespace denso::level
