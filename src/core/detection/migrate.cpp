#include "detection/migrate.h"
#include "detection/class_names.h"
#include "detection/repo.h"
#include "models/class_map.h"
#include <QSqlQuery>
#include <QVariant>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <cstddef>
#include <set>
#include <utility>
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

MigrateResult migrate_model(const QSqlDatabase& db, const MigrateRequest& req) {
    if (auto e = validate_request(req)) return {false, *e, {}};
    QSqlDatabase conn(db);
    if (!conn.transaction()) return {false, "cannot begin transaction", {}};
    auto rb = [&](const std::string& e){ conn.rollback(); return MigrateResult{false, e, {}}; };

    // Old model identity (every named camera must attach exactly this filename).
    int64_t old_model_id = 0; std::string old_name; std::vector<std::string> old_names;
    { QSqlQuery q(db); q.prepare("SELECT id,name,class_names FROM model WHERE filename=?");
      q.addBindValue(QString::fromStdString(req.old_filename));
      if (!q.exec())  return rb("query old model failed");
      if (!q.next())  return rb("no catalog row for old filename " + req.old_filename);
      old_model_id = q.value(0).toLongLong();
      old_name     = q.value(1).toString().toStdString();
      old_names    = parse_class_names(q.value(2).toString().toStdString());
      if (q.next())   return rb("multiple catalog rows for old filename"); }

    std::vector<OldAttach> attaches;
    for (int64_t cam : req.camera_ids) {
        auto [st, a] = load_old_attachment(db, cam, req.old_filename);   // 7a loader (real name)
        switch (st) {
            case LoadStatus::QueryFailed: return rb("CAS query failed for camera " + std::to_string(cam));
            case LoadStatus::NotAttached: return rb("camera " + std::to_string(cam) + " does not attach " + req.old_filename);
            case LoadStatus::Ambiguous:   return rb("camera " + std::to_string(cam) + " attaches " + req.old_filename + " more than once");
            case LoadStatus::Ok: break;
        }
        attaches.push_back(a);
    }

    auto cm = denso::models::resolve_class_map(old_names, req.new_class_names, req.explicit_remap);
    if (!cm.map) return rb(cm.error);
    const auto& fwd = *cm.map;
    std::map<int,int> inv; for (const auto& [k,v] : fwd) inv[v] = k;

    DetectionModel nm{0, req.new_name, req.new_filename, req.new_class_names};
    auto nid = upsert_model(db, nm);
    if (!nid) return rb("upsert new model failed");

    QJsonArray attJson;
    for (std::size_t k = 0; k < attaches.size(); ++k) {
        const OldAttach& a = attaches[k];
        QSqlQuery up(db); up.prepare("UPDATE camera_model SET model_id=? WHERE id=?");
        up.addBindValue(static_cast<qlonglong>(*nid)); up.addBindValue(static_cast<qlonglong>(a.camera_model_id));
        if (!up.exec()) return rb("repoint failed");
        QSqlQuery del(db); del.prepare("DELETE FROM camera_model_class WHERE camera_model_id=?");
        del.addBindValue(static_cast<qlonglong>(a.camera_model_id));
        if (!del.exec()) return rb("clearing old class rows failed");
        QJsonArray sel;
        for (const auto& s : a.classes) {
            const auto it = fwd.find(s.class_id);
            if (it == fwd.end()) return rb("selected class " + std::to_string(s.class_id) + " has no mapping in the new model");
            QSqlQuery ins(db);
            ins.prepare("INSERT INTO camera_model_class(camera_model_id,class_id,conf) VALUES(?,?,?)");
            ins.addBindValue(static_cast<qlonglong>(a.camera_model_id)); ins.addBindValue(it->second);
            ins.addBindValue(static_cast<double>(s.conf));
            if (!ins.exec()) return rb("inserting remapped class row failed");
            QJsonObject so; so["class_id"] = s.class_id; so["conf"] = s.conf; sel.append(so);
        }
        // 64-bit row ids as decimal STRINGS — a QJsonValue stores numbers as
        // double, which would silently lose precision above 2^53 and could then
        // misidentify the exact attachment a rollback must repoint. The receipt
        // is the rollback contract, so its ids must round-trip losslessly.
        QJsonObject ao;
        ao["camera_id"] = QString::number(static_cast<qlonglong>(req.camera_ids[k]));
        ao["camera_model_id"] = QString::number(static_cast<qlonglong>(a.camera_model_id));
        ao["classes"] = sel;
        attJson.append(ao);
    }

    auto jmap = [](const std::map<int,int>& m){ QJsonObject o; for (auto [k,v] : m) o[QString::number(k)] = v;
        return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)); };
    QJsonArray oldcn; for (const auto& n : old_names) oldcn.append(QString::fromStdString(n));
    QSqlQuery rec(db);
    rec.prepare("INSERT INTO model_migration_receipt"
                "(created_utc,old_filename,old_model_id,old_name,old_class_names,"
                " new_filename,new_model_id,new_engine_sha256,forward_map,inverse_map,attachments)"
                " VALUES(?,?,?,?,?,?,?,?,?,?,?)");
    rec.addBindValue(QString::fromStdString(req.created_utc));
    rec.addBindValue(QString::fromStdString(req.old_filename));
    rec.addBindValue(static_cast<qlonglong>(old_model_id));
    rec.addBindValue(QString::fromStdString(old_name));
    rec.addBindValue(QString::fromUtf8(QJsonDocument(oldcn).toJson(QJsonDocument::Compact)));
    rec.addBindValue(QString::fromStdString(req.new_filename));
    rec.addBindValue(static_cast<qlonglong>(*nid));
    rec.addBindValue(QString::fromStdString(req.new_engine_sha256));
    rec.addBindValue(jmap(fwd)); rec.addBindValue(jmap(inv));
    rec.addBindValue(QString::fromUtf8(QJsonDocument(attJson).toJson(QJsonDocument::Compact)));
    if (!rec.exec()) return rb("writing the migration receipt failed");

    if (!conn.commit()) return rb("commit failed");
    MigrateResult r; r.ok = true; r.affected_cameras = req.camera_ids; return r;
}
}
