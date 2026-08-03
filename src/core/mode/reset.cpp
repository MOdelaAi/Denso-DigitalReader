#include "mode/reset.h"

#include <QSqlError>
#include <QSqlQuery>
#include <QString>
#include <QVariant>

namespace denso::mode {

ResetResult switch_and_reset(const QSqlDatabase& db, TargetMode new_mode) {
    // A QSqlDatabase copy shares the same underlying connection; transaction() is
    // non-const, so take a mutable copy of the const handle to drive the txn.
    QSqlDatabase conn(db);
    if (!conn.transaction()) {
        return {false, conn.lastError().text().toStdString()};
    }
    const auto fail = [&conn](const QSqlQuery& q) -> ResetResult {
        const std::string err = q.lastError().text().toStdString();
        conn.rollback();
        return {false, err};
    };

    // Destroy the mode-owned workspace. Both modes are cleared, not just the one
    // being left: a switch now guarantees the DESTINATION opens unconfigured, and
    // clearing only the outgoing mode would let a stale binding from two switches
    // ago decide what the destination runtime loads.
    //
    // camera_model_class is deleted UNCONDITIONALLY (NOT scoped by
    // camera_model_id): camera::remove leaves such rows orphaned, so a scoped
    // delete would strand them.
    //
    // camera_area carries the Digital Number decimal formats, so clearing the
    // areas clears the formats with them - they are a column of the area, not a
    // separate authority that could be forgotten here.
    //
    // ball_level_calibration is the legacy v14 single-zone geometry that v15
    // backfilled into ball_level_zone. It is cleared TOO: it is Ball Leveler
    // configuration, the confirmation promises the calibration is gone, and
    // leaving it would make that copy the only surviving description of a setup
    // the operator was told had been cleared. Nothing re-reads it (the v15
    // backfill runs once, below the current schema version), so clearing it
    // cannot resurrect anything.
    //
    // Cameras are UPDATEd, never deleted - every id and connection/capture column
    // is preserved; only the two processing flags are cleared.
    for (const char* sql : {
             "DELETE FROM camera_model_class",  // unconditional - repairs orphans
             "DELETE FROM camera_model",
             "DELETE FROM camera_area",
             "DELETE FROM reading",
             "DELETE FROM model_migration_receipt",
             "DELETE FROM ball_level_binding",
             "DELETE FROM ball_level_zone",
             "DELETE FROM ball_level_calibration",
             "UPDATE camera SET setup_complete = 0, areas_need_review = 0",
         }) {
        QSqlQuery q(db);
        q.prepare(QString::fromLatin1(sql));
        if (!q.exec()) return fail(q);
    }

    // The two settings upserts are issued on THIS connection inside THIS
    // transaction - NOT via mode::save / brazing::save, which run their own
    // (separate, possibly unchecked) writes and are not the transactional
    // boundary.
    //
    // mode.target is written INSIDE the transaction, never before it: a failure
    // after an early write would leave the persisted mode advanced while the
    // runtime still served the old one.
    QSqlQuery m(db);
    m.prepare(QStringLiteral(
        "INSERT INTO settings (key, value) VALUES ('mode.target', ?) "
        "ON CONFLICT(key) DO UPDATE SET value = excluded.value"));
    m.addBindValue(QString::fromLatin1(to_string(new_mode)));
    if (!m.exec()) return fail(m);

    // Reporting is disabled on every switch; the ADDRESS is deliberately kept so
    // the operator need not retype it to re-enable reporting. The value is the
    // string "0", matching brazing/config.cpp.
    QSqlQuery b(db);
    if (!b.exec(QStringLiteral(
            "INSERT INTO settings (key, value) VALUES ('brazing.enabled', '0') "
            "ON CONFLICT(key) DO UPDATE SET value = excluded.value"))) {
        return fail(b);
    }

    if (!conn.commit()) {
        // SQLite can leave the transaction OPEN when commit fails (a busy/locked
        // connection), and this handle is shared - the next write would then join
        // a transaction nobody owns. Close it out explicitly, mirroring
        // camera::remove.
        const std::string err = conn.lastError().text().toStdString();
        conn.rollback();
        return {false, err};
    }
    return {true, {}};
}

} // namespace denso::mode
