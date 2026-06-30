#include "settings/repo.h"

#include <QByteArray>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QSqlQuery>
#include <QVariant>

#include <optional>

namespace denso::settings {

namespace {

/// Read a single setting's raw value, or nullopt if absent or unreadable.
std::optional<QString> get(const QSqlDatabase& db, const QString& key) {
    QSqlQuery q(db);
    q.prepare(QStringLiteral("SELECT value FROM settings WHERE key = ?"));
    q.addBindValue(key);
    if (q.exec() && q.next()) {
        return q.value(0).toString();
    }
    return std::nullopt;
}

/// Upsert a single setting. Errors are silently ignored.
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

Settings load(const QSqlDatabase& db) {
    const Settings d;  // defaults
    Settings out = d;

    if (const auto v = get(db, QStringLiteral("width"))) {
        bool ok = false;
        const uint parsed = v->toUInt(&ok);
        if (ok) {
            out.width = parsed;
        }
    }
    if (const auto v = get(db, QStringLiteral("height"))) {
        bool ok = false;
        const uint parsed = v->toUInt(&ok);
        if (ok) {
            out.height = parsed;
        }
    }
    if (const auto v = get(db, QStringLiteral("dark"))) {
        out.dark = (*v == QStringLiteral("1"));
    }
    if (const auto v = get(db, QStringLiteral("fullscreen"))) {
        out.fullscreen = (*v == QStringLiteral("1"));
    }
    return out;
}

void save(const QSqlDatabase& db, const Settings& settings) {
    set(db, QStringLiteral("width"), QString::number(settings.width));
    set(db, QStringLiteral("height"), QString::number(settings.height));
    set(db, QStringLiteral("dark"), settings.dark ? QStringLiteral("1") : QStringLiteral("0"));
    set(db, QStringLiteral("fullscreen"),
        settings.fullscreen ? QStringLiteral("1") : QStringLiteral("0"));
}

void import_legacy(const QSqlDatabase& db, const QString& json_path) {
    if (!QFile::exists(json_path)) {
        return;
    }
    QFile file(json_path);
    if (!file.open(QIODevice::ReadOnly)) {
        return;  // unreadable → leave untouched (mirrors read_to_string().ok())
    }
    const QByteArray bytes = file.readAll();
    file.close();

    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(bytes, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        return;  // corrupt → kept for inspection, nothing imported
    }
    const QJsonObject obj = doc.object();

    // serde requires width/height (no default) and rejects wrong-typed fields;
    // dark defaults to true, fullscreen to false. Mirror that contract exactly.
    // serde's u32 also rejects negative, fractional, or out-of-range numbers —
    // a plain isDouble() would wrongly accept "-5"/"3.5"/"5e12", so require an
    // exact, in-range integer here too (all valid u32 are exact as double).
    const QJsonValue wv = obj.value(QStringLiteral("width"));
    const QJsonValue hv = obj.value(QStringLiteral("height"));
    const auto is_u32 = [](const QJsonValue& v) {
        if (!v.isDouble()) {
            return false;
        }
        const double d = v.toDouble();
        return d >= 0.0 && d <= 4294967295.0
            && d == static_cast<double>(static_cast<uint32_t>(d));
    };
    if (!is_u32(wv) || !is_u32(hv)) {
        return;
    }
    Settings s;  // defaults supply dark=true, fullscreen=false
    s.width = static_cast<uint32_t>(wv.toDouble());
    s.height = static_cast<uint32_t>(hv.toDouble());
    if (obj.contains(QStringLiteral("dark"))) {
        const QJsonValue dv = obj.value(QStringLiteral("dark"));
        if (!dv.isBool()) {
            return;
        }
        s.dark = dv.toBool();
    }
    if (obj.contains(QStringLiteral("fullscreen"))) {
        const QJsonValue fv = obj.value(QStringLiteral("fullscreen"));
        if (!fv.isBool()) {
            return;
        }
        s.fullscreen = fv.toBool();
    }

    save(db, s);
    QFile::remove(json_path);
}

} // namespace denso::settings
