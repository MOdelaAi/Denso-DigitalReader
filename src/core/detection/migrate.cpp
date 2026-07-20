#include "detection/migrate.h"
#include "detection/class_names.h"
#include <QSqlQuery>
#include <QVariant>
#include <set>
namespace denso::detection {
std::optional<std::string> validate_request(const MigrateRequest& r) {
    if (r.camera_ids.empty()) return "no cameras specified";
    std::set<int64_t> seen;
    for (auto id : r.camera_ids) {
        if (id <= 0) return "camera id must be positive: " + std::to_string(id);
        if (!seen.insert(id).second) return "duplicate camera id: " + std::to_string(id);
    }
    if (r.old_filename.empty()) return "old_filename must be non-empty";
    if (r.new_filename.empty()) return "new_filename must be non-empty";
    if (r.old_filename == r.new_filename) return "old_filename and new_filename must differ";
    if (r.new_name.empty()) return "new_name must be non-empty";
    if (r.new_class_names.empty()) return "new_class_names must be non-empty";
    if (r.new_engine_sha256.empty()) return "new_engine_sha256 must be non-empty";
    return std::nullopt;
}
LoadResult load_old_attachment(const QSqlDatabase& db, int64_t camera_id,
                               const std::string& old_filename) {
    QSqlQuery q(db);
    if (!q.prepare("SELECT cm.id, m.id, m.class_names FROM camera_model cm "
                   "JOIN model m ON m.id = cm.model_id "
                   "WHERE cm.camera_id = ? AND m.filename = ? LIMIT 2"))
        return {LoadStatus::QueryFailed, {}};
    q.addBindValue(static_cast<qlonglong>(camera_id));
    q.addBindValue(QString::fromStdString(old_filename));
    if (!q.exec()) return {LoadStatus::QueryFailed, {}};
    if (!q.next()) return {LoadStatus::NotAttached, {}};
    OldAttach a;
    a.camera_model_id = q.value(0).toLongLong();
    a.old_model_id    = q.value(1).toLongLong();
    a.old_class_names = parse_class_names(q.value(2).toString().toStdString());
    if (q.next()) return {LoadStatus::Ambiguous, {}};   // >1 attachment for this filename
    QSqlQuery c(db);
    if (!c.prepare("SELECT class_id, conf FROM camera_model_class WHERE camera_model_id = ?"))
        return {LoadStatus::QueryFailed, {}};
    c.addBindValue(static_cast<qlonglong>(a.camera_model_id));
    if (!c.exec()) return {LoadStatus::QueryFailed, {}};
    while (c.next()) a.classes.push_back({c.value(0).toInt(), c.value(1).toFloat()});
    return {LoadStatus::Ok, a};
}
}
