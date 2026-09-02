#include "camera/repo.h"

#include "camera/area_points.h"

#include <algorithm>

#include <QMetaType>
#include <QSqlError>
#include <QSqlQuery>
#include <QString>
#include <QVariant>

#include <set>
#include <utility>

namespace denso::camera {

namespace {

const QString COLUMNS = QStringLiteral(
    "id, name, camera_type, active, cam_index, ip, rtsp, username, "
    "width, height, fps, pitch, roll, rotation, password, "
    "channel, stream, manufacturer, areas_need_review, setup_complete");

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
    q.addBindValue(c.areas_need_review ? 1 : 0);
    q.addBindValue(c.setup_complete ? 1 : 0);
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
    c.areas_need_review = q.value(18).toInt() != 0;
    c.setup_complete = q.value(19).toInt() != 0;
    return c;
}

} // namespace

std::optional<int64_t> insert(const QSqlDatabase& db, const Camera& c) {
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "INSERT INTO camera (name, camera_type, active, cam_index, ip, rtsp, username, "
        "width, height, fps, pitch, roll, rotation, password, channel, stream, manufacturer, "
        "areas_need_review, setup_complete) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)"));
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
        "channel=?, stream=?, manufacturer=?, areas_need_review=?, setup_complete=? "
        "WHERE id=?"));
    bind_fields(q, c);
    q.addBindValue(static_cast<qlonglong>(c.id));
    return q.exec();
}

bool remove(const QSqlDatabase& db, int64_t id) {
    // ONE transaction, like replace_areas: the areas were deleted first and the
    // camera second with no transaction, so a failure on the second statement
    // destroyed the operator's hand-drawn ROI polygons while leaving the camera
    // that referenced them. Either both go or neither does. (A QSqlDatabase copy
    // shares the same underlying connection; transaction() is non-const.)
    QSqlDatabase conn(db);
    if (!conn.transaction()) {
        return false;
    }
    const auto rollback = [&conn] {
        conn.rollback();
        return false;
    };

    QSqlQuery areas(db);
    areas.prepare(QStringLiteral("DELETE FROM camera_area WHERE camera_id = ?"));
    areas.addBindValue(static_cast<qlonglong>(id));
    if (!areas.exec()) {
        return rollback();
    }
    QSqlQuery cam(db);
    cam.prepare(QStringLiteral("DELETE FROM camera WHERE id = ?"));
    cam.addBindValue(static_cast<qlonglong>(id));
    if (!cam.exec()) {
        return rollback();
    }
    if (!conn.commit()) {
        // SQLite can leave the transaction OPEN when commit fails (a busy/locked
        // connection), and this handle is shared — the next write on it would
        // then join a transaction nobody owns. Close it out explicitly.
        return rollback();
    }
    return true;
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

std::vector<Camera> runtime(const QSqlDatabase& db) {
    std::vector<Camera> out;
    QSqlQuery q(db);
    // Filtered in SQL, BEFORE the grid's "first 4 by id" truncation — an
    // unfinished camera used to consume one of the four tile slots and hide a
    // real one behind it.
    if (!q.exec(QStringLiteral("SELECT %1 FROM camera "
                               "WHERE active = 1 AND setup_complete = 1 "
                               "ORDER BY id")
                    .arg(COLUMNS))) {
        return out;
    }
    while (q.next()) {
        out.push_back(from_row(q));
    }
    return out;
}

std::vector<Camera> active(const QSqlDatabase& db) {
    std::vector<Camera> out;
    QSqlQuery q(db);
    // Filtered in SQL for the same reason runtime() is: the grid truncates to the
    // first four by id, so a disabled camera must be gone BEFORE that cut or it
    // would occupy a tile slot and hide a live one behind it.
    if (!q.exec(QStringLiteral("SELECT %1 FROM camera WHERE active = 1 ORDER BY id")
                    .arg(COLUMNS))) {
        return out;
    }
    while (q.next()) {
        out.push_back(from_row(q));
    }
    return out;
}

bool mark_setup_complete(const QSqlDatabase& db, int64_t id) {
    QSqlQuery q(db);
    q.prepare(QStringLiteral("UPDATE camera SET setup_complete = 1 WHERE id = ?"));
    q.addBindValue(static_cast<qlonglong>(id));
    if (!q.exec()) {
        return false;
    }
    // exec() succeeds on an UPDATE that matched NOTHING — valid SQL, zero rows.
    // For a method whose whole job is "this camera is now finished", that is a
    // lie: a stale or concurrently-deleted id would report success and the caller
    // would believe the camera is live. Require the row to exist.
    return q.numRowsAffected() == 1;
}

std::vector<CameraArea> areas_for(const QSqlDatabase& db, int64_t camera_id) {
    std::vector<CameraArea> out;
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "SELECT id, camera_id, name, points, zone, decimal_places FROM camera_area "
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
        const QVariant zv = q.value(4);
        if (!zv.isNull()) {
            a.zone = zv.toInt();
        }
        // Clamped on the way OUT as well as on the way in. The column has a
        // CHECK, but a database can also arrive from a backup or a hand edit,
        // and an out-of-range format would otherwise reach the renderer.
        a.decimal_places = std::clamp(q.value(5).toInt(), 0, 3);
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

    // Zone numbers are unique machine-wide: the combined brazing payload keys by
    // zone number across all cameras, so two ROIs sharing a number would collide
    // two readings onto one key. Reject a save that repeats a zone within this
    // camera or claims one already assigned to another camera. (This camera's own
    // rows were just deleted above, so the cross-camera check can't self-conflict.)
    std::set<int> zones_this_camera;
    for (const CameraArea& a : areas) {
        // `a.zone` engaged is the WHOLE test: NULL is the one unassigned form,
        // so anything engaged is a claim that must be range- and
        // uniqueness-checked. There is deliberately no `*a.zone != 0` escape —
        // 0 is out of range and gets REFUSED below, not silently treated as
        // "unassigned" the way it was before v17.
        if (a.zone) {
            // Authoritative range enforcement — the UI validator is UX only, and
            // this repo is also reached by the wizard controller and by tests.
            // The column carries no CHECK, so without this an out-of-range number
            // would persist happily and reach the payload as "zone500".
            if (!camera::zone_in_range(*a.zone)) {
                return rollback();
            }
            if (!zones_this_camera.insert(*a.zone).second) {
                return rollback();  // duplicated within this same save
            }
            // Both modes draw from ONE zone-number namespace, so the check spans
            // both tables. `switch_mode` is non-destructive: a machine can hold a
            // digit configuration and a Ball configuration simultaneously, and
            // whichever mode is running writes the same `zoneN` payload keys.
            // Checking only camera_area here would let a digit area silently take
            // a number a Ball zone already reports, and the collision would not
            // appear until the operator switched modes.
            QSqlQuery chk(db);
            chk.prepare(QStringLiteral(
                "SELECT 1 FROM camera_area WHERE zone = ? AND camera_id != ? "
                "UNION ALL "
                "SELECT 1 FROM ball_level_zone WHERE zone_no = ? AND camera_id != ? "
                "LIMIT 1"));
            chk.addBindValue(*a.zone);
            chk.addBindValue(static_cast<qlonglong>(camera_id));
            chk.addBindValue(*a.zone);
            chk.addBindValue(static_cast<qlonglong>(camera_id));
            if (!chk.exec()) {
                return rollback();
            }
            if (chk.next()) {
                return rollback();  // already owned by another camera
            }
            // next() false is end-of-rows OR a fetch error. Untested, a fetch
            // error reads as "no conflict" and this save takes a number another
            // camera already reports.
            if (chk.lastError().isValid()) {
                return rollback();
            }
        }
        QSqlQuery ins(db);
        ins.prepare(QStringLiteral(
            "INSERT INTO camera_area (camera_id, name, points, zone, decimal_places) "
            "VALUES (?, ?, ?, ?, ?)"));
        ins.addBindValue(static_cast<qlonglong>(camera_id));
        ins.addBindValue(QString::fromStdString(a.name));
        ins.addBindValue(QString::fromStdString(serialize_points(a.points)));
        ins.addBindValue(a.zone ? QVariant(*a.zone)
                                : QVariant(QMetaType(QMetaType::Int)));
        // Out-of-range never reaches persistence: the column CHECK would reject
        // the row and fail the whole save, so an invalid in-memory value is
        // normalised to the behaviour-preserving 0 rather than losing the set.
        ins.addBindValue(std::clamp(a.decimal_places, 0, 3));
        if (!ins.exec()) {
            return rollback();
        }
    }

    // Saving the ROI set IS the verification: whatever quarantine flag was set by
    // a prior source/geometry change is cleared atomically with the new areas, so
    // ROI-filtering and zone reporting resume. (A no-op when not under review.)
    QSqlQuery clr(db);
    clr.prepare(QStringLiteral("UPDATE camera SET areas_need_review = 0 WHERE id = ?"));
    clr.addBindValue(static_cast<qlonglong>(camera_id));
    if (!clr.exec()) {
        return rollback();
    }

    return conn.commit() || rollback();
}

std::optional<std::map<int, std::string>> try_zones_owned_by_other_cameras(
    const QSqlDatabase& db, int64_t camera_id) {
    std::map<int, std::string> owned;
    QSqlQuery q(db);
    // ONE ownership query over BOTH modes' zone tables — this function is the
    // single authority the pickers and validators consult, and a Ball-specific
    // twin of it would be a second zone-numbering authority. A number claimed by
    // the OTHER mode is just as unavailable as one claimed by another camera in
    // this mode, because the payload key is the same either way.
    q.prepare(QStringLiteral(
        "SELECT a.zone, c.name FROM camera_area a JOIN camera c "
        "ON c.id = a.camera_id "
        // No `AND a.zone != 0`: v17 normalised every legacy zero to NULL, so
        // IS NOT NULL is the whole ownership test. Re-adding the filter would
        // hide a 0 that should never exist rather than surfacing it.
        "WHERE a.camera_id != ? AND a.zone IS NOT NULL "
        "UNION "
        "SELECT z.zone_no, c.name FROM ball_level_zone z JOIN camera c "
        "ON c.id = z.camera_id "
        "WHERE z.camera_id != ?"));
    q.addBindValue(static_cast<qlonglong>(camera_id));
    q.addBindValue(static_cast<qlonglong>(camera_id));
    if (!q.exec()) {
        return std::nullopt;
    }
    while (q.next()) {
        owned.emplace(q.value(0).toInt(), q.value(1).toString().toStdString());
    }
    // next() returns false BOTH at end-of-rows and on a fetch error. Without
    // this, a mid-scan failure would return a SHORT ownership map that reads as
    // a complete one — and a number whose row was never fetched would look free.
    if (q.lastError().isValid()) {
        return std::nullopt;
    }
    return owned;
}

std::map<int, std::string> zones_owned_by_other_cameras(const QSqlDatabase& db,
                                                        int64_t camera_id) {
    // Failing OPEN is correct here and only here: this overload exists for the
    // pickers, which use the answer to grey out taken numbers and name their
    // owner. A page that cannot read the database shows nothing greyed out and
    // the save still refuses — whereas a save that cannot read the database must
    // refuse outright, which is why the write chokepoints take the try_ form.
    return try_zones_owned_by_other_cameras(db, camera_id)
        .value_or(std::map<int, std::string>{});
}

bool set_areas_need_review(const QSqlDatabase& db, int64_t camera_id, bool need) {
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "UPDATE camera SET areas_need_review = ? WHERE id = ?"));
    q.addBindValue(need ? 1 : 0);
    q.addBindValue(static_cast<qlonglong>(camera_id));
    return q.exec();
}

} // namespace denso::camera
