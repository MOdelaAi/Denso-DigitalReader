#include "camera/repo.h"

#include "camera/area_points.h"

#include <QMetaType>
#include <QSqlQuery>
#include <QString>
#include <QVariant>

#include <utility>

namespace denso::camera {

namespace {

const QString COLUMNS = QStringLiteral(
    "id, name, camera_type, active, cam_index, ip, rtsp, username, "
    "width, height, fps, pitch, roll, rotation, password, "
    "channel, stream, manufacturer");

QVariant bind_str(const std::optional<std::string>& v) {
    return v ? QVariant(QString::fromStdString(*v)) : QVariant(QMetaType(QMetaType::QString));
}

QVariant bind_uint(const std::optional<uint32_t>& v) {
    return v ? QVariant(static_cast<uint>(*v)) : QVariant(QMetaType(QMetaType::UInt));
}

std::optional<std::string> col_str(const QVariant& v) {
    return v.isNull() ? std::nullopt
                      : std::optional<std::string>(v.toString().toStdString());
}

std::optional<uint32_t> col_uint(const QVariant& v) {
    return v.isNull() ? std::nullopt : std::optional<uint32_t>(v.toUInt());
}

/// Bind every non-id column, in COLUMNS order (used by insert and update).
void bind_fields(QSqlQuery& q, const Camera& c) {
    q.addBindValue(QString::fromStdString(c.name));
    q.addBindValue(QString::fromStdString(c.camera_type));
    q.addBindValue(c.active ? 1 : 0);
    q.addBindValue(bind_uint(c.index));
    q.addBindValue(bind_str(c.ip));
    q.addBindValue(bind_str(c.rtsp));
    q.addBindValue(bind_str(c.username));
    q.addBindValue(static_cast<uint>(c.width));
    q.addBindValue(static_cast<uint>(c.height));
    q.addBindValue(static_cast<uint>(c.fps));
    q.addBindValue(static_cast<double>(c.pitch));
    q.addBindValue(static_cast<double>(c.roll));
    q.addBindValue(static_cast<uint>(c.rotation));
    q.addBindValue(bind_str(c.password));
    q.addBindValue(bind_uint(c.channel));
    q.addBindValue(bind_uint(c.stream));
    q.addBindValue(bind_str(c.manufacturer));
}

Camera from_row(const QSqlQuery& q) {
    Camera c;
    c.id = q.value(0).toLongLong();
    c.name = q.value(1).toString().toStdString();
    c.camera_type = q.value(2).toString().toStdString();
    c.active = q.value(3).toInt() != 0;
    c.index = col_uint(q.value(4));
    c.ip = col_str(q.value(5));
    c.rtsp = col_str(q.value(6));
    c.username = col_str(q.value(7));
    c.width = q.value(8).toUInt();
    c.height = q.value(9).toUInt();
    c.fps = q.value(10).toUInt();
    c.pitch = q.value(11).toFloat();
    c.roll = q.value(12).toFloat();
    c.rotation = q.value(13).toUInt();
    c.password = col_str(q.value(14));
    c.channel = col_uint(q.value(15));
    c.stream = col_uint(q.value(16));
    c.manufacturer = col_str(q.value(17));
    return c;
}

} // namespace

std::optional<int64_t> insert(const QSqlDatabase& db, const Camera& c) {
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "INSERT INTO camera (name, camera_type, active, cam_index, ip, rtsp, username, "
        "width, height, fps, pitch, roll, rotation, password, channel, stream, manufacturer) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)"));
    bind_fields(q, c);
    if (!q.exec()) {
        return std::nullopt;
    }
    return q.lastInsertId().toLongLong();
}

bool update(const QSqlDatabase& db, const Camera& c) {
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "UPDATE camera SET name=?, camera_type=?, active=?, cam_index=?, ip=?, rtsp=?, "
        "username=?, width=?, height=?, fps=?, pitch=?, roll=?, rotation=?, password=?, "
        "channel=?, stream=?, manufacturer=? "
        "WHERE id=?"));
    bind_fields(q, c);
    q.addBindValue(static_cast<qlonglong>(c.id));
    return q.exec();
}

bool remove(const QSqlDatabase& db, int64_t id) {
    QSqlQuery areas(db);
    areas.prepare(QStringLiteral("DELETE FROM camera_area WHERE camera_id = ?"));
    areas.addBindValue(static_cast<qlonglong>(id));
    if (!areas.exec()) {
        return false;
    }
    QSqlQuery cam(db);
    cam.prepare(QStringLiteral("DELETE FROM camera WHERE id = ?"));
    cam.addBindValue(static_cast<qlonglong>(id));
    return cam.exec();
}

std::optional<Camera> get(const QSqlDatabase& db, int64_t id) {
    QSqlQuery q(db);
    q.prepare(QStringLiteral("SELECT %1 FROM camera WHERE id = ?").arg(COLUMNS));
    q.addBindValue(static_cast<qlonglong>(id));
    if (!q.exec() || !q.next()) {
        return std::nullopt;
    }
    return from_row(q);
}

std::vector<Camera> all(const QSqlDatabase& db) {
    std::vector<Camera> out;
    QSqlQuery q(db);
    if (!q.exec(QStringLiteral("SELECT %1 FROM camera ORDER BY id").arg(COLUMNS))) {
        return out;
    }
    while (q.next()) {
        out.push_back(from_row(q));
    }
    return out;
}

std::vector<CameraArea> areas_for(const QSqlDatabase& db, int64_t camera_id) {
    std::vector<CameraArea> out;
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "SELECT id, camera_id, name, points FROM camera_area "
        "WHERE camera_id = ? ORDER BY id"));
    q.addBindValue(static_cast<qlonglong>(camera_id));
    if (!q.exec()) {
        return out;
    }
    while (q.next()) {
        CameraArea a;
        a.id = q.value(0).toLongLong();
        a.camera_id = q.value(1).toLongLong();
        a.name = q.value(2).toString().toStdString();
        a.points = parse_points(q.value(3).toString().toStdString());
        out.push_back(std::move(a));
    }
    return out;
}

bool replace_areas(const QSqlDatabase& db, int64_t camera_id,
                   const std::vector<CameraArea>& areas) {
    // Delete-all + re-insert as one unit so a mid-write failure can't leave a
    // half-updated ROI set behind. transaction()/commit()/rollback() are
    // non-const; a QSqlDatabase copy shares the same underlying connection.
    QSqlDatabase conn(db);
    if (!conn.transaction()) {
        return false;
    }
    const auto rollback = [&conn] {
        conn.rollback();
        return false;
    };

    QSqlQuery del(db);
    del.prepare(QStringLiteral("DELETE FROM camera_area WHERE camera_id = ?"));
    del.addBindValue(static_cast<qlonglong>(camera_id));
    if (!del.exec()) {
        return rollback();
    }

    for (const CameraArea& a : areas) {
        QSqlQuery ins(db);
        ins.prepare(QStringLiteral(
            "INSERT INTO camera_area (camera_id, name, points) VALUES (?, ?, ?)"));
        ins.addBindValue(static_cast<qlonglong>(camera_id));
        ins.addBindValue(QString::fromStdString(a.name));
        ins.addBindValue(QString::fromStdString(serialize_points(a.points)));
        if (!ins.exec()) {
            return rollback();
        }
    }

    return conn.commit() || rollback();
}

} // namespace denso::camera
