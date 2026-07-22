#include "mode/config.h"

#include <QSqlQuery>
#include <QString>
#include <QVariant>

#include <optional>

namespace denso::mode {
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

} // namespace

TargetMode load(const QSqlDatabase& db) {
    if (const auto v = get(db, QStringLiteral("mode.target"))) {
        return parse_target_mode(v->toStdString());
    }
    return TargetMode::DigitReader;
}

bool save(const QSqlDatabase& db, TargetMode m) {
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "INSERT INTO settings (key, value) VALUES ('mode.target', ?) "
        "ON CONFLICT(key) DO UPDATE SET value = excluded.value"));
    q.addBindValue(QString::fromLatin1(to_string(m)));
    return q.exec();
}

} // namespace denso::mode
