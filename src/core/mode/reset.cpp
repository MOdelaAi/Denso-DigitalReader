#include "mode/reset.h"

#include <QSqlError>
#include <QSqlQuery>
#include <QString>
#include <QVariant>

namespace denso::mode {
namespace {

// Run a prepared statement; on failure capture the verbatim SQL error text.
bool checked(QSqlQuery& q, std::string& err) {
    if (q.exec()) {
        return true;
    }
    err = q.lastError().text().toStdString();
    return false;
}

} // namespace

std::optional<SwitchCounts> preview_counts(const QSqlDatabase& db) {
    SwitchCounts c;
    // Any query failure → nullopt (abort the confirmation). Never fall back to 0:
    // a 0 an operator reads as "nothing to lose" would be a lie if the query
    // simply failed to run.
    const auto one = [&](const char* sql, int& out) -> bool {
        QSqlQuery q(db);
        if (!q.exec(QString::fromLatin1(sql)) || !q.next()) {
            return false;
        }
        out = q.value(0).toInt();
        return true;
    };
    if (!one("SELECT COUNT(*) FROM camera", c.cameras)) return std::nullopt;
    if (!one("SELECT COUNT(*) FROM camera_model", c.model_bindings)) return std::nullopt;
    if (!one("SELECT COUNT(*) FROM camera_area", c.areas)) return std::nullopt;
    if (!one("SELECT COUNT(*) FROM reading", c.readings)) return std::nullopt;
    if (!one("SELECT COUNT(*) FROM model_migration_receipt", c.receipts)) return std::nullopt;

    QSqlQuery z(db);
    if (!z.exec(QStringLiteral("SELECT DISTINCT zone FROM camera_area "
                               "WHERE zone IS NOT NULL AND zone != 0 ORDER BY zone"))) {
        return std::nullopt;
    }
    while (z.next()) {
        c.zones.push_back(z.value(0).toInt());
    }
    // next() returns false both at normal end-of-result AND on a fetch-time
    // driver error. Only the latter sets a valid lastError() — treat it as a
    // failed query (nullopt), honouring the "REAL counts or abort" contract; a
    // truncated zone list must never render as a complete preview.
    if (z.lastError().isValid()) {
        return std::nullopt;
    }
    return c;
}

ResetResult switch_and_reset(const QSqlDatabase& db, TargetMode new_mode) {
    ResetResult r;
    // A QSqlDatabase copy shares the same underlying connection; transaction() is
    // non-const, so we take a mutable copy of the const handle to drive the txn.
    QSqlDatabase conn(db);
    if (!conn.transaction()) {
        r.error = conn.lastError().text().toStdString();
        return r;
    }
    const auto rollback = [&](const std::string& e) {
        conn.rollback();
        r.error = e;
        return r;
    };

    // Destroy the mode-owned workspace. camera_model_class is deleted
    // UNCONDITIONALLY (NOT scoped by camera_model_id): camera::remove leaves such
    // rows orphaned, so a scoped delete would strand them (spec §6.3, §12.16).
    // Cameras are UPDATEd, never deleted — every id and connection/capture column
    // is preserved; only the two processing flags are cleared.
    for (const char* sql : {
             "DELETE FROM camera_model_class",  // unconditional — repairs orphans
             "DELETE FROM camera_model",
             "DELETE FROM camera_area",
             "DELETE FROM reading",
             "DELETE FROM model_migration_receipt",
             "UPDATE camera SET setup_complete = 0, areas_need_review = 0",
         }) {
        QSqlQuery q(db);
        q.prepare(QString::fromLatin1(sql));
        std::string e;
        if (!checked(q, e)) {
            return rollback(e);
        }
    }

    // The two settings upserts are issued on THIS connection inside THIS
    // transaction — NOT via mode::save / brazing::save, which run their own
    // (separate, possibly unchecked) writes and are not the transactional
    // boundary. brazing.enabled is the string "0" (matching brazing/config.cpp);
    // brazing.base_url is deliberately left untouched.
    {
        QSqlQuery q(db);
        q.prepare(QStringLiteral(
            "INSERT INTO settings (key, value) VALUES ('mode.target', ?) "
            "ON CONFLICT(key) DO UPDATE SET value = excluded.value"));
        q.addBindValue(QString::fromLatin1(to_string(new_mode)));
        std::string e;
        if (!checked(q, e)) {
            return rollback(e);
        }
    }
    {
        QSqlQuery q(db);
        q.prepare(QStringLiteral(
            "INSERT INTO settings (key, value) VALUES ('brazing.enabled', '0') "
            "ON CONFLICT(key) DO UPDATE SET value = excluded.value"));
        std::string e;
        if (!checked(q, e)) {
            return rollback(e);
        }
    }

    if (!conn.commit()) {
        // SQLite can leave the transaction OPEN when commit fails (a busy/locked
        // connection), and this handle is shared — the next write would then join
        // a transaction nobody owns. Close it out explicitly, mirroring
        // camera::remove (camera/repo.cpp:142-147).
        return rollback(conn.lastError().text().toStdString());
    }
    r.ok = true;
    return r;
}

} // namespace denso::mode
