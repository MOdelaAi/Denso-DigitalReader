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

} // namespace denso::detection
