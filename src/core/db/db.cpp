#include "db/db.h"

#include <QCoreApplication>
#include <QDebug>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

#include <atomic>
#include <utility>

namespace denso::db {

namespace {

/// Current schema version. Bump and add a `version < N` block in
/// run_migrations() when changing the schema.
constexpr int SCHEMA_VERSION = 4;

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
            // Rust propagates a pragma failure out of open(); mirror that.
            ok = QSqlQuery(db).exec(QStringLiteral("PRAGMA journal_mode = WAL"));
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
    if (!db.isValid() || !db.isOpen()) {
        qWarning().noquote() << "run_migrations: db not open/valid:" << db.lastError().text();
        return false;
    }

    // Read the schema version in its own scope: QSQLITE keeps the prepared
    // statement (and its read lock) alive until the QSqlQuery is finished or
    // destroyed, and a live read cursor makes the schema-changing migrations
    // below fail with SQLITE_LOCKED ("database table is locked"). Destroying
    // the query here releases that lock before any DDL runs.
    int version = 0;
    {
        QSqlQuery q(db);
        if (!q.exec(QStringLiteral("PRAGMA user_version")) || !q.next()) {
            qWarning().noquote() << "run_migrations: read user_version failed:"
                                 << q.lastError().text();
            return false;
        }
        version = q.value(0).toInt();
    }

    // QSQLITE executes a single statement per exec(), so the Rust
    // `execute_batch` blocks are split into one call per statement here.
    const auto run = [&db](const char* sql) -> bool {
        QSqlQuery s(db);
        if (!s.exec(QString::fromLatin1(sql))) {
            qWarning().noquote() << "run_migrations: statement failed:" << s.lastError().text()
                                 << "\n  SQL:" << sql;
            return false;
        }
        return true;
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

    if (version < 4) {
        // Camera inventory: one row per camera, plus its ROI areas. Optional /
        // type-specific columns are nullable (cam_index for USB; ip/rtsp/
        // username for IP). `index` is a SQL keyword, hence `cam_index`. The IP
        // password is intentionally absent — destined for the OS secret store.
        if (!run("CREATE TABLE IF NOT EXISTS camera ("
                 "    id          INTEGER PRIMARY KEY,"
                 "    name        TEXT    NOT NULL,"
                 "    camera_type TEXT    NOT NULL,"  // "usb" | "ip"
                 "    active      INTEGER NOT NULL,"
                 "    cam_index   INTEGER,"
                 "    ip          TEXT,"
                 "    rtsp        TEXT,"
                 "    username    TEXT,"
                 "    width       INTEGER NOT NULL,"
                 "    height      INTEGER NOT NULL,"
                 "    fps         INTEGER NOT NULL,"
                 "    pitch       REAL    NOT NULL,"
                 "    roll        REAL    NOT NULL,"
                 "    rotation    INTEGER NOT NULL"
                 ")")) {
            return false;
        }
        if (!run("CREATE TABLE IF NOT EXISTS camera_area ("
                 "    id        INTEGER PRIMARY KEY,"
                 "    camera_id INTEGER NOT NULL REFERENCES camera(id),"
                 "    name      TEXT    NOT NULL,"
                 "    x1        REAL    NOT NULL,"
                 "    y1        REAL    NOT NULL,"
                 "    x2        REAL    NOT NULL,"
                 "    y2        REAL    NOT NULL"
                 ")")) {
            return false;
        }
        if (!run("CREATE INDEX IF NOT EXISTS idx_camera_area_camera "
                 "ON camera_area(camera_id)")) {
            return false;
        }
    }

    // PRAGMA can't be parameterized; SCHEMA_VERSION is a trusted constant.
    QSqlQuery set_ver(db);
    if (!set_ver.exec(QStringLiteral("PRAGMA user_version = %1").arg(SCHEMA_VERSION))) {
        qWarning().noquote() << "run_migrations: set user_version failed:"
                             << set_ver.lastError().text();
        return false;
    }
    return true;
}

} // namespace denso::db
