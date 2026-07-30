#include "mode/reset.h"

#include <QSqlError>
#include <QSqlQuery>
#include <QString>
#include <QVariant>

namespace denso::mode {


ResetResult switch_mode(const QSqlDatabase& db, TargetMode new_mode) {
    // NON-DESTRUCTIVE. Deletes nothing: Digital Reader attachments, class
    // selections, ROI areas, readings and rollback receipts all persist while
    // ball_leveler is active, and the Ball Leveler calibration persists while
    // digit_reader is active. Camera rows are untouched in FULL, including
    // setup_complete and areas_need_review - resetting those was part of the
    // destructive contract this supersedes.
    QSqlDatabase conn(db);
    if (!conn.transaction()) {
        return {false, conn.lastError().text().toStdString()};
    }
    const auto fail = [&conn](const QSqlQuery& q) -> ResetResult {
        const std::string err = q.lastError().text().toStdString();
        conn.rollback();
        return {false, err};
    };

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
    // the operator need not retype it to re-enable reporting.
    QSqlQuery b(db);
    if (!b.exec(QStringLiteral(
            "INSERT INTO settings (key, value) VALUES ('brazing.enabled', '0') "
            "ON CONFLICT(key) DO UPDATE SET value = excluded.value"))) {
        return fail(b);
    }

    if (!conn.commit()) {
        const std::string err = conn.lastError().text().toStdString();
        conn.rollback();
        return {false, err};
    }
    return {true, {}};
}

} // namespace denso::mode
