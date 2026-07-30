#include "health/status_file.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

namespace denso::health {
namespace {

QString status_text(health::Readiness r) {
    switch (r) {
        case health::Readiness::Ready:    return QStringLiteral("ready");
        case health::Readiness::Degraded: return QStringLiteral("degraded");
        case health::Readiness::Blocked:  return QStringLiteral("blocked");
    }
    return QStringLiteral("blocked");
}

} // namespace

bool write_status_file(const QString& path,
                       const health::IntegrityVerdict& verdict,
                       const std::map<int64_t, uint32_t>& camera_causes,
                       const std::set<int>& held_zones,
                       const std::set<int>& inhibited_zones,
                       const std::optional<QString>& mode,
                       std::optional<bool> mode_setup_required,
                       const std::vector<ZoneInhibitRecord>& zone_inhibit_onsets) {
    QJsonObject root;
    root["status"] = status_text(verdict.status);

    // Optional mode fields — emitted ONLY when the caller could determine them
    // (spec §9); omitted, never guessed, when the DB could not be read.
    if (mode) root["mode"] = *mode;
    if (mode_setup_required) root["mode_setup_required"] = *mode_setup_required;

    QJsonArray blockers;
    for (const auto& b : verdict.blockers) {
        QJsonObject o;
        o["reason"] = health::reason_code(b.kind);   // stable string, never an ordinal
        o["detail"] = b.detail;
        blockers.append(o);
    }
    root["blockers"] = blockers;

    QJsonArray issues;
    for (const auto& i : verdict.issues) {
        QJsonObject o;
        o["reason"] = health::reason_code(i.kind);   // stable string, never an ordinal
        // Ids as STRINGS: QJsonValue is a double, so >2^53 would lose precision.
        o["camera_id"] = QString::number(i.camera_id);
        o["detail"] = i.detail;
        // The policy's OWN reason, alongside the kind's — so a diagnosis names the
        // REAL cause (a hash mismatch reads model_provenance_failed, not
        // model_mode_incompatible). Both strings are a file format. Emitted only
        // when a policy actually produced one, so the record for kinds that
        // consult no policy (EngineMissing / EnginesUnmanifested) is byte-for-byte
        // what it has always been.
        if (!i.policy_reason.isEmpty()) o["policy_reason"] = i.policy_reason;
        issues.append(o);
    }
    root["issues"] = issues;

    QJsonArray causes;
    for (const auto& [camera_id, mask] : camera_causes) {
        QJsonObject o;
        o["camera_id"] = QString::number(camera_id);
        o["causes"] = static_cast<int>(mask);
        causes.append(o);
    }
    root["camera_causes"] = causes;

    QJsonArray held;
    for (const int z : held_zones) held.append(z);
    root["held_zones"] = held;

    QJsonArray inhibited;
    for (const int z : inhibited_zones) inhibited.append(z);
    root["inhibited_zones"] = inhibited;

    // Zone inhibit ONSETS — the escalations drained since the last write. ADDITIVE
    // and omitted entirely when empty, so a document from a run that raised no
    // alarm is byte-for-byte what it has always been (the rule policy_reason above
    // already follows). Distinct from inhibited_zones: that array is the standing
    // condition, this one is what just changed and carries the camera identity a
    // bare zone number cannot.
    if (!zone_inhibit_onsets.empty()) {
        QJsonArray onsets;
        for (const auto& o : zone_inhibit_onsets) {
            QJsonObject j;
            // Id as a STRING for the same reason as issues above: QJsonValue is a
            // double, so an id beyond 2^53 would lose precision.
            j["camera_id"] = QString::number(o.camera_id);
            j["zone_no"]   = o.zone_no;
            j["reason"]    = o.reason;   // stable string code, never an ordinal
            onsets.append(j);
        }
        root["zone_inhibit_onsets"] = onsets;
    }

    // QSaveFile is write-to-temp + atomic rename on commit, and removes its temp
    // on failure — so no partial file is ever observable at `path`.
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        return false;
    }
    f.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    return f.commit();
}

} // namespace denso::health
