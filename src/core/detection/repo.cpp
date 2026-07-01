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

bool set_camera_models(const QSqlDatabase& db, int64_t camera_id,
                       const std::vector<CameraModel>& models) {
    QSqlDatabase conn(db);
    if (!conn.transaction()) {
        return false;
    }
    const auto rollback = [&conn] { conn.rollback(); return false; };

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

CameraDetection detection_for(const QSqlDatabase& db, int64_t camera_id) {
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
        out.models.push_back(std::move(rm));
    }
    return out;
}

} // namespace denso::detection
