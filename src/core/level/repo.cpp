#include "level/repo.h"

#include "camera/camera.h"  // kMaxZone
#include "camera/repo.h"    // zones_owned_by_other_cameras — the ONE authority
#include "detection/class_names.h"
#include "detection/detection.h"

#include <QSqlError>
#include <QSqlQuery>
#include <QString>
#include <QVariant>

#include <set>

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
                              const std::vector<LevelZone>& zones,
                              const std::string& view_revision,
                              const denso::models::ManifestView& view,
                              const denso::models::PlatformInfo& platform,
                              SaveRefusal* refusal) {
    const auto refuse = [&](const std::string& code, int64_t model_id,
                            const std::string& canonical,
                            const std::string& filename,
                            std::optional<int> zone_no = std::nullopt) {
        if (refusal) {
            refusal->camera_id = camera_id;
            refusal->model_id = model_id;
            refusal->canonical_id = canonical;
            refusal->filename = filename;
            refusal->reason_code = code;
            refusal->zone_no = zone_no;
        }
        return false;
    };

    // ── Arity, BEFORE any transaction ────────────────────────────────────────
    // Ball Leveler binds exactly one model with exactly one class, per CAMERA.
    // Checked first and outside the transaction because they need no database at
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

    // ── Zone-set arity and numbering, also DB-free ───────────────────────────
    // A camera owns 1..kMaxBallZones zones. Zero is rejected as loudly as five:
    // a camera with a model and nothing to measure is a configuration the
    // runtime cannot act on, and silently accepting it would strand the operator
    // in a state the wizard cannot show them.
    if (zones.empty() || zones.size() > static_cast<size_t>(kMaxBallZones)) {
        return refuse("level_zone_count", binding.model_id, std::string(),
                      std::string());
    }
    std::set<int> zone_nos;
    for (const LevelZone& z : zones) {
        // The SAME predicate the digit chokepoint applies — one namespace, one
        // bound, so the two modes cannot drift. It re-spells nothing: the floor
        // it enforces is the one ball_level_zone's own CHECK (zone_no >= 1) has
        // carried since v15.
        if (!denso::camera::zone_in_range(z.zone_no)) {
            return refuse("level_zone_out_of_range", binding.model_id, std::string(),
                          std::string(), z.zone_no);
        }
        if (!zone_nos.insert(z.zone_no).second) {
            // Two zones of ONE camera claiming one number would collide two
            // measurements onto one payload key just as surely as two cameras
            // would — and this is the case a cross-camera query cannot see.
            return refuse("level_zone_duplicate", binding.model_id, std::string(),
                          std::string(), z.zone_no);
        }
    }

    QSqlDatabase conn(db);
    if (!conn.transaction()) return false;
    const auto rollback = [&conn] { conn.rollback(); return false; };

    // ── Machine-wide zone ownership ──────────────────────────────────────────
    // Asked of the ONE authority that also answers for the digit Areas page, so
    // both modes read the same answer. This camera's own rows are excluded (they
    // are replaced wholesale below), which is what stops a re-save of an
    // unchanged set from conflicting with itself.
    // try_ form, NOT the picker's: a database that cannot answer must stop this
    // save, not be read as "every number is free". Leave `refusal` untouched so
    // the caller reports a write failure rather than inventing a policy reason.
    const auto taken_probe =
        denso::camera::try_zones_owned_by_other_cameras(db, camera_id);
    if (!taken_probe) {
        return rollback();
    }
    const std::map<int, std::string>& taken = *taken_probe;
    for (const LevelZone& z : zones) {
        if (taken.count(z.zone_no)) {
            conn.rollback();
            return refuse("level_zone_taken", binding.model_id, std::string(),
                          std::string(), z.zone_no);
        }
    }

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
    // THE MODE IS HARDCODED. A row in these tables IS a Ball Leveler binding, so
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

    // ── Geometry gate — EVERY zone, still before any mutation ────────────────
    // All zones are validated up front rather than as they are written, so a bad
    // fourth zone cannot be discovered after three have already been inserted.
    // The rollback would undo them, but only a transaction that was never dirtied
    // makes that guarantee independent of the rollback working.
    for (const LevelZone& z : zones) {
        const CalibrationCheck check = validate_calibration(z.calibration);
        if (!check.ok) {
            conn.rollback();
            return refuse(check.reason_code, binding.model_id, md.canonical_id,
                          denso::models::diagnostic_filename(row.filename),
                          z.zone_no);
        }
    }

    // ── The camera-level binding. UPSERT on camera_id, which is the PRIMARY KEY,
    //    so one camera can never accumulate a second model.
    QSqlQuery ins(db);
    ins.prepare(QStringLiteral(
        "INSERT INTO ball_level_binding "
        "(camera_id, model_id, class_id, view_revision) VALUES (?, ?, ?, ?) "
        "ON CONFLICT(camera_id) DO UPDATE SET "
        "  model_id = excluded.model_id, class_id = excluded.class_id, "
        "  view_revision = excluded.view_revision"));
    ins.addBindValue(static_cast<qlonglong>(camera_id));
    ins.addBindValue(static_cast<qlonglong>(binding.model_id));
    ins.addBindValue(class_id);
    ins.addBindValue(QString::fromStdString(view_revision));
    if (!ins.exec()) return rollback();

    // ── The zone rows: delete-all + re-insert, mirroring camera::replace_areas.
    //    Replacing wholesale is what makes the stored set equal the submitted set
    //    — an upsert-only write would silently retain a zone the operator deleted.
    QSqlQuery del(db);
    del.prepare(QStringLiteral("DELETE FROM ball_level_zone WHERE camera_id = ?"));
    del.addBindValue(static_cast<qlonglong>(camera_id));
    if (!del.exec()) return rollback();

    for (const LevelZone& z : zones) {
        QSqlQuery zi(db);
        zi.prepare(QStringLiteral(
            "INSERT INTO ball_level_zone "
            "(camera_id, zone_no, conf, rect_x, rect_y, rect_w, rect_h, "
            " y_100, y_0, hold_ms) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)"));
        zi.addBindValue(static_cast<qlonglong>(camera_id));
        zi.addBindValue(z.zone_no);
        zi.addBindValue(z.calibration.conf);
        zi.addBindValue(z.calibration.rect_x);
        zi.addBindValue(z.calibration.rect_y);
        zi.addBindValue(z.calibration.rect_w);
        zi.addBindValue(z.calibration.rect_h);
        zi.addBindValue(z.calibration.y_100);
        zi.addBindValue(z.calibration.y_0);
        zi.addBindValue(z.calibration.hold_ms);
        if (!zi.exec()) return rollback();
    }

    return conn.commit() || rollback();
}

std::optional<std::optional<LevelConfig>> try_level_config_for(
    const QSqlDatabase& db, int64_t camera_id) {
    LevelConfig c;
    {
        QSqlQuery q(db);
        q.prepare(QStringLiteral(
            "SELECT camera_id, model_id, class_id, view_revision "
            "FROM ball_level_binding WHERE camera_id = ?"));
        q.addBindValue(static_cast<qlonglong>(camera_id));
        // exec() and next() are checked SEPARATELY and mean different things:
        // exec() false is a broken table/schema (infrastructure), next() false is
        // simply no row for this camera (an ordinary setup gap). Folding them into
        // one `||` is precisely the bug this function replaces.
        if (!q.exec()) return std::nullopt;
        if (!q.next()) {
            // next() returns false BOTH at end-of-rows and on a FETCH error, so
            // "no row" is only an honest answer once lastError() is clear.
            if (q.lastError().isValid()) return std::nullopt;
            return std::optional<LevelConfig>{};
        }
        c.camera_id = q.value(0).toLongLong();
        c.model_id = q.value(1).toLongLong();
        c.class_id = q.value(2).toInt();
        c.view_revision = q.value(3).toString().toStdString();
    }

    QSqlQuery zq(db);
    zq.prepare(QStringLiteral(
        "SELECT zone_no, conf, rect_x, rect_y, rect_w, rect_h, y_100, y_0, "
        "hold_ms FROM ball_level_zone WHERE camera_id = ? ORDER BY zone_no"));
    zq.addBindValue(static_cast<qlonglong>(camera_id));
    if (!zq.exec()) return std::nullopt;
    while (zq.next()) {
        LevelZone z;
        z.zone_no = zq.value(0).toInt();
        z.calibration = read_calibration(zq, 1);
        c.zones.push_back(std::move(z));
    }
    // A fetch error mid-scan would otherwise return a SHORT zone set that reads
    // as a complete configuration — the camera would measure three of its four
    // zones and report nothing wrong.
    if (zq.lastError().isValid()) return std::nullopt;

    // A binding with no zones is not a configuration. The chokepoint cannot
    // write that state (it refuses an empty set), so reaching it means the rows
    // were removed outside the app; reporting it as "configured" would build a
    // measuring pipeline with nothing to measure.
    if (c.zones.empty()) return std::optional<LevelConfig>{};
    return std::optional<LevelConfig>{std::move(c)};
}

std::optional<LevelConfig> level_config_for(const QSqlDatabase& db,
                                            int64_t camera_id) {
    return try_level_config_for(db, camera_id)
        .value_or(std::optional<LevelConfig>{});
}

std::optional<std::vector<int64_t>> try_cameras_with_valid_config(
    const QSqlDatabase& db) {
    std::vector<int64_t> candidates;
    {
        QSqlQuery q(db);
        if (!q.exec(QStringLiteral(
                "SELECT camera_id FROM ball_level_binding ORDER BY camera_id"))) {
            return std::nullopt;  // undeterminable — NOT "no camera is configured"
        }
        while (q.next()) candidates.push_back(q.value(0).toLongLong());
        if (q.lastError().isValid()) return std::nullopt;
    }

    std::vector<int64_t> out;
    for (int64_t id : candidates) {
        // Go back through try_level_config_for so a read fault on ONE camera is
        // still a fault for the WHOLE answer. Returning the others would be a
        // short list that reads as a complete one.
        const auto cfg = try_level_config_for(db, id);
        if (!cfg) return std::nullopt;
        if (!*cfg) continue;  // binding with no zones — not configured
        // "Stored" is NOT "valid": rows can be hand-edited or restored from a
        // backup, so every zone's geometry is re-validated on every read. A
        // camera qualifies only if ALL its zones validate — one broken zone is
        // an operator-visible fault, not something to quietly drop.
        bool all_ok = true;
        for (const LevelZone& z : (*cfg)->zones) {
            if (!validate_calibration(z.calibration).ok) { all_ok = false; break; }
        }
        if (all_ok) out.push_back(id);
    }
    return out;
}

std::vector<int64_t> cameras_with_valid_config(const QSqlDatabase& db) {
    return try_cameras_with_valid_config(db).value_or(std::vector<int64_t>{});
}

bool has_valid_config(const QSqlDatabase& db, int64_t camera_id) {
    const auto c = level_config_for(db, camera_id);
    if (!c || c->zones.empty()) return false;
    for (const LevelZone& z : c->zones) {
        if (!validate_calibration(z.calibration).ok) return false;
    }
    return true;
}

std::map<int, std::string> zones_owned_elsewhere(const QSqlDatabase& db,
                                                 int64_t camera_id) {
    // Delegates rather than duplicates: that function already spans BOTH modes'
    // zone tables, so the Ball editor and the digit Areas page cannot disagree
    // about which numbers are free.
    return denso::camera::zones_owned_by_other_cameras(db, camera_id);
}

bool clear_level_configuration(const QSqlDatabase& db, int64_t camera_id) {
    QSqlDatabase conn(db);
    if (!conn.transaction()) return false;
    const auto rollback = [&conn] { conn.rollback(); return false; };

    QSqlQuery zd(db);
    zd.prepare(QStringLiteral("DELETE FROM ball_level_zone WHERE camera_id = ?"));
    zd.addBindValue(static_cast<qlonglong>(camera_id));
    if (!zd.exec()) return rollback();

    QSqlQuery bd(db);
    bd.prepare(QStringLiteral("DELETE FROM ball_level_binding WHERE camera_id = ?"));
    bd.addBindValue(static_cast<qlonglong>(camera_id));
    if (!bd.exec()) return rollback();

    return conn.commit() || rollback();
}

}  // namespace denso::level
