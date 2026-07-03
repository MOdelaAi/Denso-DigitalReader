#include "reading/repo.h"

#include <QSqlQuery>
#include <QString>
#include <QVariant>

namespace denso::reading {

std::optional<int64_t> insert(const QSqlDatabase& db, const Reading& r) {
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "INSERT INTO reading (camera_id, ts_ms, value, conf) "
        "VALUES (?, ?, ?, ?)"));
    q.addBindValue(static_cast<qint64>(r.camera_id));
    q.addBindValue(static_cast<qint64>(r.ts_ms));
    q.addBindValue(QString::fromStdString(r.value));
    q.addBindValue(static_cast<double>(r.conf));
    if (!q.exec()) {
        return std::nullopt;
    }
    return q.lastInsertId().toLongLong();
}

std::vector<Reading> query(const QSqlDatabase& db, int64_t camera_id,
                           int64_t from_ms, int64_t to_ms) {
    std::vector<Reading> out;
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "SELECT id, camera_id, ts_ms, value, conf FROM reading "
        "WHERE camera_id = ? AND ts_ms >= ? AND ts_ms <= ? "
        "ORDER BY ts_ms ASC, id ASC"));
    q.addBindValue(static_cast<qint64>(camera_id));
    q.addBindValue(static_cast<qint64>(from_ms));
    q.addBindValue(static_cast<qint64>(to_ms));
    if (!q.exec()) {
        return out;
    }
    while (q.next()) {
        Reading r;
        r.id        = q.value(0).toLongLong();
        r.camera_id = q.value(1).toLongLong();
        r.ts_ms     = q.value(2).toLongLong();
        r.value     = q.value(3).toString().toStdString();
        r.conf      = static_cast<float>(q.value(4).toDouble());
        out.push_back(std::move(r));
    }
    return out;
}

} // namespace denso::reading
