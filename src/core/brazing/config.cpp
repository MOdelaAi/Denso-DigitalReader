#include "brazing/config.h"

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

void set(const QSqlDatabase& db, const QString& key, const QString& value) {
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "INSERT INTO settings (key, value) VALUES (?, ?) "
        "ON CONFLICT(key) DO UPDATE SET value = excluded.value"));
    q.addBindValue(key);
    q.addBindValue(value);
    q.exec();
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

void save(const QSqlDatabase& db, const BrazingConfig& cfg) {
    set(db, QStringLiteral("brazing.enabled"),
        cfg.enabled ? QStringLiteral("1") : QStringLiteral("0"));
    set(db, QStringLiteral("brazing.base_url"), QString::fromStdString(cfg.base_url));
}

} // namespace denso::brazing
