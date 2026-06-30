#include "db/db.h"

#include <QCoreApplication>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

#include <atomic>

namespace denso::db {

namespace {

/// Current schema version. Bump and add a `version < N` block in
/// run_migrations() when changing the schema.
constexpr int SCHEMA_VERSION = 3;

/// Monotonic source of unique connection names so connections (especially
/// in-memory test DBs sharing the ":memory:" name) never collide.
QString next_connection_name() {
    static std::atomic<unsigned long long> counter{0};
    return QStringLiteral("denso_%1").arg(counter.fetch_add(1));
}

} // namespace

QString default_path() {
    const QString dir = QCoreApplication::applicationDirPath();
    if (dir.isEmpty()) {
        return QStringLiteral("denso.db");
    }
    return dir + QStringLiteral("/denso.db");
}

Db::Db(QString name) : name_(std::move(name)) {}

Db::Db(Db&& other) noexcept : name_(std::move(other.name_)) {
    other.name_.clear();
}

Db& Db::operator=(Db&& other) noexcept {
    if (this != &other) {
        if (!name_.isEmpty()) {
            QSqlDatabase::removeDatabase(name_);
        }
        name_ = std::move(other.name_);
        other.name_.clear();
    }
    return *this;
}

Db::~Db() {
    if (!name_.isEmpty()) {
        QSqlDatabase::removeDatabase(name_);
    }
}

QSqlDatabase Db::handle() const {
    return QSqlDatabase::database(name_);
}

std::optional<Db> Db::open(const QString& path) {
    const QString name = next_connection_name();
    bool ok = false;
    {
        // Scope the QSqlDatabase copy so it is destroyed before any
        // removeDatabase() on the failure path (else Qt warns "still in use").
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name);
        db.setDatabaseName(path);
        ok = db.open();
        if (ok) {
            // WAL mode so the UI can read while a background thread writes.
            QSqlQuery(db).exec(QStringLiteral("PRAGMA journal_mode = WAL"));
        }
    }
    if (!ok) {
        QSqlDatabase::removeDatabase(name);
        return std::nullopt;
    }
    return Db(name);
}

std::optional<Db> Db::open_in_memory() {
    const QString name = next_connection_name();
    bool ok = false;
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name);
        db.setDatabaseName(QStringLiteral(":memory:"));
        ok = db.open();
    }
    if (!ok) {
        QSqlDatabase::removeDatabase(name);
        return std::nullopt;
    }
    return Db(name);
}

bool run_migrations(const QSqlDatabase& db) {
    QSqlQuery q(db);
    if (!q.exec(QStringLiteral("PRAGMA user_version")) || !q.next()) {
        return false;
    }
    const int version = q.value(0).toInt();

    // QSQLITE executes a single statement per exec(), so the Rust
    // `execute_batch` blocks are split into one call per statement here.
    const auto run = [&db](const char* sql) -> bool {
        QSqlQuery s(db);
        return s.exec(QString::fromLatin1(sql));
    };

    if (version < 1) {
        if (!run("CREATE TABLE IF NOT EXISTS settings ("
                 "    key   TEXT PRIMARY KEY,"
                 "    value TEXT NOT NULL"
                 ")")) {
            return false;
        }
        if (!run("CREATE TABLE IF NOT EXISTS readings ("
                 "    id          INTEGER PRIMARY KEY,"
                 "    ts          INTEGER NOT NULL,"
                 "    value       TEXT    NOT NULL,"
                 "    confidence  REAL,"
                 "    image_path  TEXT"
                 ")")) {
            return false;
        }
        if (!run("CREATE INDEX IF NOT EXISTS idx_readings_ts ON readings(ts)")) {
            return false;
        }
    }

    if (version < 2) {
        // User-editable network configuration; the app is the source of truth
        // and reasserts these to the OS on boot. The WiFi PSK is NOT stored
        // here — it lives in the OS secret store.
        if (!run("CREATE TABLE IF NOT EXISTS net_config ("
                 "    iface    TEXT PRIMARY KEY,"
                 "    mode     TEXT NOT NULL,"
                 "    ip       TEXT,"
                 "    prefix   INTEGER,"
                 "    gateway  TEXT,"
                 "    dns1     TEXT,"
                 "    dns2     TEXT,"
                 "    ssid     TEXT,"
                 "    security TEXT"
                 ")")) {
            return false;
        }
    }

    if (version < 3) {
        // The digit-reader log feature was removed; drop its now-unused table.
        if (!run("DROP INDEX IF EXISTS idx_readings_ts")) {
            return false;
        }
        if (!run("DROP TABLE IF EXISTS readings")) {
            return false;
        }
    }

    // PRAGMA can't be parameterized; SCHEMA_VERSION is a trusted constant.
    QSqlQuery set_ver(db);
    return set_ver.exec(QStringLiteral("PRAGMA user_version = %1").arg(SCHEMA_VERSION));
}

} // namespace denso::db
