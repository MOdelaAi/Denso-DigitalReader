#include "brazing/config.h"

#include <QDebug>
#include <QSqlDriver>
#include <QSqlQuery>
#include <QString>
#include <QVariant>

#include <optional>

namespace denso::brazing {
namespace {

std::optional<QString> get(const QSqlDatabase& db, const QString& key) {
    QSqlQuery q(db);
    q.prepare(QStringLiteral("SELECT value FROM settings WHERE key = ?"));
    q.addBindValue(key);
    if (q.exec() && q.next()) {
        return q.value(0).toString();
    }
    return std::nullopt;
}

bool set(const QSqlDatabase& db, const QString& key, const QString& value) {
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "INSERT INTO settings (key, value) VALUES (?, ?) "
        "ON CONFLICT(key) DO UPDATE SET value = excluded.value"));
    q.addBindValue(key);
    q.addBindValue(value);
    return q.exec();
}

} // namespace

BrazingConfig load(const QSqlDatabase& db) {
    BrazingConfig out;
    if (const auto v = get(db, QStringLiteral("brazing.enabled"))) {
        out.enabled = (*v == QStringLiteral("1"));
    }
    if (const auto v = get(db, QStringLiteral("brazing.base_url"))) {
        out.base_url = v->toStdString();
    }
    return out;
}

bool save(const QSqlDatabase& db, const BrazingConfig& cfg) {
    // ONE transaction over both rows, so the checked result is atomic: a caller
    // told "false" can state that nothing was applied, and a restart can never
    // load a half-written configuration (reporting enabled against the PREVIOUS
    // address, say). Modeled on mode::switch_and_reset's checked transaction.
    //
    // The handle is copied because transaction()/commit() are non-const;
    // QSqlDatabase is a reference to the connection, so this is the SAME
    // connection, not a second one.
    QSqlDatabase conn = db;

    // Two DISTINCT reasons transaction() can return false, and they need opposite
    // handling. A driver that has no transaction support was never going to give
    // atomicity, so refusing would break saving on such a build for nothing —
    // fall through to the plain upserts (the behaviour before this function was
    // transactional). But a driver that DOES support them and still failed to
    // BEGIN is a connection in an unknown state, and writing anyway is exactly
    // how a half-written configuration would appear behind a `false` return.
    // Write nothing.
    const QSqlDriver* driver = conn.driver();
    const bool supported =
        driver != nullptr && driver->hasFeature(QSqlDriver::Transactions);
    if (supported && !conn.transaction()) {
        qWarning().noquote()
            << "[brazing] could not begin the settings transaction;"
            << "nothing was written";
        return false;
    }

    const bool ok = set(conn, QStringLiteral("brazing.enabled"),
                        cfg.enabled ? QStringLiteral("1") : QStringLiteral("0")) &&
                    set(conn, QStringLiteral("brazing.base_url"),
                        QString::fromStdString(cfg.base_url));

    if (!supported) {
        // No transaction was opened, so there is nothing to commit or roll back.
        // `ok` is then a best-effort result, not an atomic one.
        return ok;
    }
    if (!ok || !conn.commit()) {
        // SQLite can leave the transaction OPEN when commit fails on a busy or
        // locked connection, and this handle is SHARED — close it out explicitly
        // so the next write does not join a transaction nobody owns. A rollback
        // that itself fails leaves the connection in that state, which the next
        // caller cannot diagnose on its own, so say so here.
        if (!conn.rollback()) {
            qCritical().noquote()
                << "[brazing] settings rollback failed; the database connection"
                << "may still hold an open transaction";
        }
        return false;
    }
    return true;
}

} // namespace denso::brazing
