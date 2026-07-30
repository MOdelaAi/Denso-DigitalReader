#include "mode/config.h"

#include "level/repo.h"

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

std::optional<bool> mode_setup_required(const QSqlDatabase& db, TargetMode mode) {
    if (mode == TargetMode::BallLeveler) {
        // Driven by REAL configuration, not hardcoded. Setup is required until at
        // least one camera carries a Ball Leveler calibration whose geometry still
        // validates - "a row exists" is not enough, because a row can be
        // hand-edited or restored from a backup.
        //
        // The REAL query is the one that must be checked. A preliminary `SELECT 1`
        // probe is not a substitute: it can succeed while the actual read fails,
        // and then an empty result would be reported as "setup required" — a
        // GUESS, which the contract in mode/config.h forbids.
        const auto cams = denso::level::try_cameras_with_valid_config(db);
        if (!cams) return std::nullopt;  // undeterminable is never guessed
        return cams->empty();
    }
    // digit_reader: independent of `active`; a query failure is UNDETERMINABLE and
    // must never be guessed as a boolean (the caller omits the field instead).
    QSqlQuery q(db);
    if (!q.exec(QStringLiteral("SELECT 1 FROM camera WHERE setup_complete = 1 LIMIT 1"))) {
        return std::nullopt;
    }
    return !q.next();  // true = none completed (setup required); false = ≥1 completed
}

} // namespace denso::mode
