#include "network/repo.h"

#include <QSqlQuery>
#include <QString>
#include <QVariant>

namespace denso::network {

namespace {

const QString COLUMNS =
    QStringLiteral("iface, mode, ip, prefix, gateway, dns1, dns2, ssid, security");

/// An optional string as a bind value: the string, or a typed SQL NULL.
QVariant bind_str(const std::optional<std::string>& v) {
    return v ? QVariant(QString::fromStdString(*v)) : QVariant(QMetaType(QMetaType::QString));
}

/// An optional unsigned as a bind value: the number, or a typed SQL NULL.
QVariant bind_uint(const std::optional<uint32_t>& v) {
    return v ? QVariant(static_cast<uint>(*v)) : QVariant(QMetaType(QMetaType::UInt));
}

/// A nullable text column back into an optional string.
std::optional<std::string> col_str(const QVariant& v) {
    return v.isNull() ? std::nullopt
                      : std::optional<std::string>(v.toString().toStdString());
}

/// A nullable integer column back into an optional unsigned.
std::optional<uint32_t> col_uint(const QVariant& v) {
    return v.isNull() ? std::nullopt : std::optional<uint32_t>(v.toUInt());
}

NetConfig from_row(const QSqlQuery& q) {
    NetConfig c;
    c.iface = q.value(0).toString().toStdString();
    c.mode = q.value(1).toString().toStdString();
    c.ip = col_str(q.value(2));
    c.prefix = col_uint(q.value(3));
    c.gateway = col_str(q.value(4));
    c.dns1 = col_str(q.value(5));
    c.dns2 = col_str(q.value(6));
    c.ssid = col_str(q.value(7));
    c.security = col_str(q.value(8));
    return c;
}

} // namespace

bool save(const QSqlDatabase& db, const NetConfig& c) {
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "INSERT INTO net_config (iface, mode, ip, prefix, gateway, dns1, dns2, ssid, security) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?) "
        "ON CONFLICT(iface) DO UPDATE SET "
        "    mode=excluded.mode, ip=excluded.ip, prefix=excluded.prefix, "
        "    gateway=excluded.gateway, dns1=excluded.dns1, dns2=excluded.dns2, "
        "    ssid=excluded.ssid, security=excluded.security"));
    q.addBindValue(QString::fromStdString(c.iface));
    q.addBindValue(QString::fromStdString(c.mode));
    q.addBindValue(bind_str(c.ip));
    q.addBindValue(bind_uint(c.prefix));
    q.addBindValue(bind_str(c.gateway));
    q.addBindValue(bind_str(c.dns1));
    q.addBindValue(bind_str(c.dns2));
    q.addBindValue(bind_str(c.ssid));
    q.addBindValue(bind_str(c.security));
    return q.exec();
}

std::optional<NetConfig> load(const QSqlDatabase& db, const std::string& iface) {
    QSqlQuery q(db);
    q.prepare(QStringLiteral("SELECT %1 FROM net_config WHERE iface = ?").arg(COLUMNS));
    q.addBindValue(QString::fromStdString(iface));
    if (!q.exec() || !q.next()) {
        return std::nullopt;
    }
    return from_row(q);
}

std::vector<NetConfig> all(const QSqlDatabase& db) {
    std::vector<NetConfig> out;
    QSqlQuery q(db);
    if (!q.exec(QStringLiteral("SELECT %1 FROM net_config ORDER BY iface").arg(COLUMNS))) {
        return out;
    }
    while (q.next()) {
        out.push_back(from_row(q));
    }
    return out;
}

} // namespace denso::network
