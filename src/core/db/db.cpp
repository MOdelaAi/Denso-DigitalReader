#include "db/db.h"

#include "camera/camera.h"   // camera::kMaxZone — the ONE zone-number range

#include <QDebug>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

#include <atomic>
#include <set>
#include <utility>
#include <vector>

namespace denso::db {

namespace {

/// Current schema version. Bump and add a `version < N` block in
/// run_migrations() when changing the schema.
constexpr int SCHEMA_VERSION = 15;

/// Monotonic source of unique connection names so connections (especially
/// in-memory test DBs sharing the ":memory:" name) never collide.
QString next_connection_name() {
    static std::atomic<unsigned long long> counter{0};
    return QStringLiteral("denso_%1").arg(counter.fetch_add(1));
}

/// Does `table` exist in this database?
///
/// `nullopt` means the PROBE ITSELF failed, which is not the same as "absent"
/// and must not be silently read as one — the same distinction every cursor site
/// in this codebase draws between end-of-rows and a fetch error.
std::optional<bool> table_exists(const QSqlDatabase& db, const QString& table) {
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = ?"));
    q.addBindValue(table);
    if (!q.exec()) {
        qWarning().noquote() << "schema probe: table_exists(" << table
                             << ") failed:" << q.lastError().text();
        return std::nullopt;
    }
    const bool found = q.next();
    if (q.lastError().isValid()) return std::nullopt;
    return found;
}

/// Does `table` exist AND carry `column`?
///
/// PRAGMA table_info returns an empty result set for a missing table rather than
/// an error, so this answers both questions in one probe.
std::optional<bool> column_exists(const QSqlDatabase& db, const QString& table,
                                  const QString& column) {
    QSqlQuery q(db);
    // PRAGMA cannot be parameterized. Both call sites pass a literal table name
    // owned by this file, never anything operator-supplied.
    if (!q.exec(QStringLiteral("PRAGMA table_info(%1)").arg(table))) {
        qWarning().noquote() << "schema probe: column_exists(" << table << "."
                             << column << ") failed:" << q.lastError().text();
        return std::nullopt;
    }
    bool found = false;
    while (q.next()) {
        if (q.value(1).toString() == column) { found = true; break; }
    }
    if (q.lastError().isValid()) return std::nullopt;
    return found;
}

/// v14 -> v15 backfill: every stored single-tank Ball calibration becomes a
/// camera-level binding plus ONE zone row.
///
/// EVERY input this reads is PROBED before it is queried, and none is inferred
/// from the entry `user_version`. The `version < N` blocks are conditional, so a
/// database entering at v11 never executes v4/v7/v10 — the migration that
/// creates `camera_area.zone`. Reading the version and concluding "v11 >= v10,
/// therefore the column is there" is an assumption about a step that did not run
/// in this process; the schema itself is the only authority on whether a
/// migration completed. Absence is then answered semantically, not defensively:
/// a database with no `camera_area.zone` has no digit zone claims, and one with
/// no `ball_level_calibration` has nothing to migrate. Both are correct answers,
/// not fallbacks.
///
/// The zone number cannot simply be 1 for everyone. Zone numbers are unique
/// MACHINE-WIDE — across cameras and across BOTH modes — because the brazing
/// payload keys by zone number alone. Assigning 1 to every migrated camera would
/// manufacture the exact collision the uniqueness rule exists to prevent, and it
/// would do so silently, at boot, on the operator's live data. So each camera
/// takes the LOWEST number not already claimed by a digit ROI or by a
/// previously-migrated Ball camera; the first camera on a Ball-only machine gets
/// Zone 1, which is the documented case.
///
/// A camera for which no number is free is SKIPPED, not failed: its v14 row
/// stays exactly where it is (this migration drops nothing), so the operator can
/// free a number and reconfigure. Failing the migration instead would refuse to
/// boot the appliance over a configuration problem.
bool migrate_ball_calibration_to_zones(const QSqlDatabase& db) {
    // The SOURCE of this backfill. v14 creates it, but v14's block is skipped on
    // a database entering at v14 or later, so its presence is probed rather than
    // assumed. Nothing to migrate is a successful migration.
    const auto have_legacy = table_exists(db, QStringLiteral("ball_level_calibration"));
    if (!have_legacy.has_value()) return false;
    if (!*have_legacy) return true;

    // Every zone number already spoken for, by EITHER mode. This mirrors
    // camera::zones_owned_by_other_cameras, which is the runtime authority over
    // the same machine-wide namespace: a number claimed by a digit ROI and one
    // claimed by a Ball zone are equally unavailable, because the brazing payload
    // keys by zone number alone.
    std::set<int> taken;

    // Digit claims. `camera_area.zone` arrives in v10 over a table created in
    // v4 and rebuilt in v7 — none of which runs on a database entering above
    // them. No column means no digit zone has ever been claimed.
    const auto have_area_zone =
        column_exists(db, QStringLiteral("camera_area"), QStringLiteral("zone"));
    if (!have_area_zone.has_value()) return false;
    if (*have_area_zone) {
        QSqlQuery q(db);
        if (!q.exec(QStringLiteral("SELECT zone FROM camera_area "
                                   "WHERE zone IS NOT NULL AND zone != 0"))) {
            qWarning().noquote() << "v15: read claimed zones failed:"
                                 << q.lastError().text();
            return false;
        }
        while (q.next()) taken.insert(q.value(0).toInt());
        if (q.lastError().isValid()) return false;
    }

    // Ball claims already on disk. `ball_level_zone` was created unconditionally
    // a few statements above, so it always exists here; it is non-empty only if a
    // previous run of this backfill was interrupted after inserting but before
    // `user_version` was stamped. Reading it is what makes a re-run land on the
    // same numbers instead of handing an already-migrated camera a second zone.
    std::set<qlonglong> already_migrated;
    {
        QSqlQuery q(db);
        if (!q.exec(QStringLiteral("SELECT camera_id, zone_no FROM ball_level_zone"))) {
            qWarning().noquote() << "v15: read existing ball zones failed:"
                                 << q.lastError().text();
            return false;
        }
        while (q.next()) {
            already_migrated.insert(q.value(0).toLongLong());
            taken.insert(q.value(1).toInt());
        }
        if (q.lastError().isValid()) return false;
    }

    struct Legacy {
        qlonglong camera_id = 0;
        qlonglong model_id = 0;
        int class_id = 0;
        double conf = 0, rect_x = 0, rect_y = 0, rect_w = 0, rect_h = 0;
        double y_100 = 0, y_0 = 0;
        int hold_ms = 0;
        QString view_revision;
    };
    std::vector<Legacy> rows;
    {
        // Read the whole set BEFORE writing: QSQLITE holds a read lock for the
        // life of the cursor, and inserting into another table while iterating
        // this one is the kind of thing that works until it doesn't.
        QSqlQuery q(db);
        if (!q.exec(QStringLiteral(
                "SELECT camera_id, model_id, class_id, conf, rect_x, rect_y, "
                "rect_w, rect_h, y_100, y_0, hold_ms, view_revision "
                "FROM ball_level_calibration ORDER BY camera_id"))) {
            qWarning().noquote() << "v15: read ball_level_calibration failed:"
                                 << q.lastError().text();
            return false;
        }
        while (q.next()) {
            Legacy r;
            r.camera_id = q.value(0).toLongLong();
            r.model_id = q.value(1).toLongLong();
            r.class_id = q.value(2).toInt();
            r.conf = q.value(3).toDouble();
            r.rect_x = q.value(4).toDouble();
            r.rect_y = q.value(5).toDouble();
            r.rect_w = q.value(6).toDouble();
            r.rect_h = q.value(7).toDouble();
            r.y_100 = q.value(8).toDouble();
            r.y_0 = q.value(9).toDouble();
            r.hold_ms = q.value(10).toInt();
            r.view_revision = q.value(11).toString();
            rows.push_back(std::move(r));
        }
        // A fetch error mid-scan would migrate a SHORT list and then stamp the
        // new user_version over it, silently losing the rest of the operator's
        // calibrations with no way to notice.
        if (q.lastError().isValid()) {
            qWarning().noquote() << "v15: ball_level_calibration fetch error";
            return false;
        }
    }

    for (const Legacy& r : rows) {
        // Already carries a zone from an interrupted earlier run. Re-deriving a
        // number for it would not be a no-op: INSERT OR IGNORE only ignores a
        // collision on (camera_id, zone_no), so a DIFFERENT number would insert a
        // second zone and hand this camera two tanks it never had.
        if (already_migrated.count(r.camera_id)) continue;

        int zone_no = 0;
        for (int z = 1; z <= denso::camera::kMaxZone; ++z) {
            if (!taken.count(z)) { zone_no = z; break; }
        }
        if (zone_no == 0) {
            qWarning().noquote()
                << "v15: no free zone number for camera" << r.camera_id
                << "- its v14 calibration is retained but not migrated";
            continue;
        }
        taken.insert(zone_no);

        QSqlQuery b(db);
        b.prepare(QStringLiteral(
            "INSERT OR IGNORE INTO ball_level_binding "
            "(camera_id, model_id, class_id, view_revision) VALUES (?, ?, ?, ?)"));
        b.addBindValue(r.camera_id);
        b.addBindValue(r.model_id);
        b.addBindValue(r.class_id);
        b.addBindValue(r.view_revision);
        if (!b.exec()) {
            qWarning().noquote() << "v15: binding insert failed:" << b.lastError().text();
            return false;
        }

        QSqlQuery z(db);
        z.prepare(QStringLiteral(
            "INSERT OR IGNORE INTO ball_level_zone "
            "(camera_id, zone_no, conf, rect_x, rect_y, rect_w, rect_h, "
            " y_100, y_0, hold_ms) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)"));
        z.addBindValue(r.camera_id);
        z.addBindValue(zone_no);
        z.addBindValue(r.conf);
        z.addBindValue(r.rect_x);
        z.addBindValue(r.rect_y);
        z.addBindValue(r.rect_w);
        z.addBindValue(r.rect_h);
        z.addBindValue(r.y_100);
        z.addBindValue(r.y_0);
        z.addBindValue(r.hold_ms);
        if (!z.exec()) {
            qWarning().noquote() << "v15: zone insert failed:" << z.lastError().text();
            return false;
        }
    }
    return true;
}

} // namespace

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

std::optional<Db> Db::open_read_only(const QString& path) {
    const QString name = next_connection_name();
    bool ok = false;
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name);
        db.setDatabaseName(path);
        // QSQLITE_OPEN_READONLY maps to SQLITE_OPEN_READONLY, which fails rather
        // than creating an absent file — exactly the contract we want.
        db.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY"));
        ok = db.open();
        // Deliberately NO journal_mode pragma here (see the header).
        if (ok) {
            // Belt and braces: READONLY already rejects writes, but query_only
            // makes the intent explicit and is what the spec calls for.
            ok = QSqlQuery(db).exec(QStringLiteral("PRAGMA query_only = ON"));
        }
    }
    if (!ok) {
        QSqlDatabase::removeDatabase(name);
        return std::nullopt;
    }
    return Db(name);
}

int supported_schema_version() { return SCHEMA_VERSION; }

std::optional<int> read_user_version(const QSqlDatabase& db) {
    // Own scope for the query: QSQLITE holds the read lock until the QSqlQuery is
    // destroyed, so callers that go on to run DDL must not keep this cursor open.
    QSqlQuery q(db);
    if (!q.exec(QStringLiteral("PRAGMA user_version")) || !q.next()) {
        return std::nullopt;
    }
    return q.value(0).toInt();
}

bool quick_check(const QSqlDatabase& db) {
    QSqlQuery q(db);
    // A healthy database answers with a single row "ok". Corruption yields one or
    // more error rows (and can span pages) — the first non-"ok" row is enough.
    if (!q.exec(QStringLiteral("PRAGMA quick_check")) || !q.next()) {
        return false;
    }
    return q.value(0).toString().compare(QLatin1String("ok"), Qt::CaseInsensitive) == 0;
}

bool run_migrations(const QSqlDatabase& db) {
    if (!db.isValid() || !db.isOpen()) {
        qWarning().noquote() << "run_migrations: db not open/valid:" << db.lastError().text();
        return false;
    }

    // Read the schema version FIRST, before any migration statement. QSQLITE keeps
    // the prepared statement (and its read lock) alive until the QSqlQuery is
    // destroyed, and a live read cursor makes the schema-changing migrations below
    // fail with SQLITE_LOCKED — read_user_version() confines the query to its own
    // scope so the lock is released before any DDL runs.
    const auto read = read_user_version(db);
    if (!read) {
        qWarning().noquote() << "run_migrations: read user_version failed:"
                             << db.lastError().text();
        return false;
    }
    const int version = *read;

    // A database written by a NEWER build must never be migrated or downgraded by
    // an older one: the `version < N` blocks below are all no-ops on a future
    // schema, but the unconditional `PRAGMA user_version = SCHEMA_VERSION` at the
    // end would silently REWRITE the version downward. Refuse before touching
    // anything — no DDL, no user_version write. The boot / --check preflight
    // (health::evaluate_db_schema) reports this as a distinct SchemaNewer blocker.
    if (version > SCHEMA_VERSION) {
        qWarning().noquote() << "run_migrations: REFUSING to migrate — database schema v"
                             << version << "is newer than supported v" << SCHEMA_VERSION
                             << "(a newer app build wrote this DB)";
        return false;
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

    if (version < 5) {
        // IP cameras carry a password (injected into the RTSP URL at capture
        // time). Plaintext in the local DB for now; OS secret store is later
        // hardening. Nullable — USB cameras have none.
        if (!run("ALTER TABLE camera ADD COLUMN password TEXT")) {
            return false;
        }
    }

    if (version < 6) {
        // Structured RTSP inputs persisted alongside the generated `rtsp` URL,
        // so a future edit flow can repopulate the form without parsing the
        // URL. All nullable — USB cameras have none. `channel` addresses
        // NVR/DVR streams; `stream` is 0=main / 1=sub; `manufacturer` is the
        // vendor name.
        if (!run("ALTER TABLE camera ADD COLUMN channel INTEGER")) {
            return false;
        }
        if (!run("ALTER TABLE camera ADD COLUMN stream INTEGER")) {
            return false;
        }
        if (!run("ALTER TABLE camera ADD COLUMN manufacturer TEXT")) {
            return false;
        }
    }

    if (version < 7) {
        // ROI areas become polygons (3+ vertices) instead of a single
        // rectangle. `camera_area` has no writers yet (CRUD lands with this
        // slice), so the old x1..y2 shape carries no data — drop and recreate
        // with one `points` TEXT column holding normalized "x,y;x,y;..." pairs.
        if (!run("DROP INDEX IF EXISTS idx_camera_area_camera")) {
            return false;
        }
        if (!run("DROP TABLE IF EXISTS camera_area")) {
            return false;
        }
        if (!run("CREATE TABLE IF NOT EXISTS camera_area ("
                 "    id        INTEGER PRIMARY KEY,"
                 "    camera_id INTEGER NOT NULL REFERENCES camera(id),"
                 "    name      TEXT    NOT NULL,"
                 "    points    TEXT    NOT NULL"
                 ")")) {
            return false;
        }
        if (!run("CREATE INDEX IF NOT EXISTS idx_camera_area_camera "
                 "ON camera_area(camera_id)")) {
            return false;
        }
    }

    if (version < 8) {
        // Per-camera detection config. `model` is the catalog of .onnx files
        // discovered under models/ (class_names cached from ONNX metadata as a
        // JSON array). A camera attaches 1..N models via `camera_model`; each
        // attachment keeps a subset of classes with a per-class confidence in
        // `camera_model_class`. Detection runs for a camera iff it has ≥1 row
        // in camera_model.
        if (!run("CREATE TABLE IF NOT EXISTS model ("
                 "    id          INTEGER PRIMARY KEY,"
                 "    name        TEXT    NOT NULL,"
                 "    filename    TEXT    NOT NULL UNIQUE,"
                 "    class_names TEXT    NOT NULL"
                 ")")) {
            return false;
        }
        if (!run("CREATE TABLE IF NOT EXISTS camera_model ("
                 "    id        INTEGER PRIMARY KEY,"
                 "    camera_id INTEGER NOT NULL REFERENCES camera(id),"
                 "    model_id  INTEGER NOT NULL REFERENCES model(id)"
                 ")")) {
            return false;
        }
        if (!run("CREATE INDEX IF NOT EXISTS idx_camera_model_camera "
                 "ON camera_model(camera_id)")) {
            return false;
        }
        if (!run("CREATE TABLE IF NOT EXISTS camera_model_class ("
                 "    id              INTEGER PRIMARY KEY,"
                 "    camera_model_id INTEGER NOT NULL REFERENCES camera_model(id),"
                 "    class_id        INTEGER NOT NULL,"
                 "    conf            REAL    NOT NULL"
                 ")")) {
            return false;
        }
        if (!run("CREATE INDEX IF NOT EXISTS idx_cmc_camera_model "
                 "ON camera_model_class(camera_model_id)")) {
            return false;
        }
    }

    if (version < 9) {
        // Recorded readings — an append-only log of detections captured per
        // camera over time, consumed by the logging/export feature. Indexed on
        // (camera_id, ts_ms) for the range queries the export UI runs.
        if (!run("CREATE TABLE IF NOT EXISTS reading ("
                 "    id        INTEGER PRIMARY KEY,"
                 "    camera_id INTEGER NOT NULL REFERENCES camera(id),"
                 "    ts_ms     INTEGER NOT NULL,"
                 "    value     TEXT    NOT NULL,"
                 "    conf      REAL    NOT NULL"
                 ")")) {
            return false;
        }
        if (!run("CREATE INDEX IF NOT EXISTS idx_reading_camera_ts "
                 "ON reading(camera_id, ts_ms)")) {
            return false;
        }
    }

    if (version < 10) {
        // Per-ROI reporting zone. A camera_area with a `zone` set is reported to
        // the brazing backend under key "zone<n>"; NULL = ROI-only (confinement,
        // no reporting). Additive; existing areas default to NULL.
        if (!run("ALTER TABLE camera_area ADD COLUMN zone INTEGER")) {
            return false;
        }
    }

    if (version < 11) {
        // ROI-review quarantine flag. Set when a view-significant source/geometry
        // change is saved; while set, the camera's ROI areas are excluded from
        // detection filtering and zone reporting is paused until the operator
        // re-verifies them (Areas → "Verify & save"). Additive; default 0.
        if (!run("ALTER TABLE camera ADD COLUMN areas_need_review "
                 "INTEGER NOT NULL DEFAULT 0")) {
            return false;
        }
    }

    if (version < 12) {
        // "Setup finished", NOT "enabled" — `active` already means the latter
        // (operator-facing enable/disable) and the two must not be conflated.
        // The wizard inserts the camera at the Configure step because attaching
        // models needs a real id, so an operator who backs out at Models used to
        // leave a LIVE, model-less camera behind. New cameras are now inserted
        // incomplete and only completed by an explicit finish action; the runtime
        // ignores incomplete rows, while the management list still shows them so
        // they can be resumed or deleted.
        //
        // DEFAULT 1 grandfathers every existing camera as complete. That is what
        // makes this upgrade-safe unconditionally rather than by inference: an
        // old version could not have recorded incompleteness, so there is no
        // signal to recover and every pre-existing row must be treated as done.
        if (!run("ALTER TABLE camera ADD COLUMN setup_complete "
                 "INTEGER NOT NULL DEFAULT 1")) {
            return false;
        }
    }

    if (version < 13) {
        // Rollback-complete receipt for a model generation swap (Slice C). Records
        // the exact camera_model rows, both model identities, prior selections +
        // thresholds, and the forward+inverse class-id maps, so a swap can be
        // reversed deterministically. Written by the migrate coordinator; read by
        // rollback. Additive table only — no existing row is touched.
        if (!run("CREATE TABLE IF NOT EXISTS model_migration_receipt ("
                 "id                INTEGER PRIMARY KEY,"
                 "created_utc       TEXT NOT NULL,"
                 "old_filename      TEXT NOT NULL,"
                 "old_model_id      INTEGER NOT NULL,"
                 "old_name          TEXT NOT NULL,"
                 "old_class_names   TEXT NOT NULL,"
                 "new_filename      TEXT NOT NULL,"
                 "new_model_id      INTEGER NOT NULL,"
                 "new_engine_sha256 TEXT NOT NULL,"
                 "forward_map       TEXT NOT NULL,"
                 "inverse_map       TEXT NOT NULL,"
                 "attachments       TEXT NOT NULL)")) {
            return false;
        }
    }

    if (version < 14) {
        // Ball Leveler measurement configuration - ONE row per camera, and the
        // SOLE durable Ball Leveler model-binding + calibration authority.
        //
        // `camera_id` is the PRIMARY KEY, not an ordinary column beside a surrogate
        // `id`: that is what makes "one Floating Ball measurement per camera" true
        // by CONSTRUCTION rather than by a rule a caller can forget. A second
        // measurement for one camera is not a validation failure, it is a
        // constraint violation.
        //
        // Deliberately NOT stored in `camera_model`: that table's contract is a
        // digit-reader ensemble (N models, a per-model class subset, a per-class
        // confidence, cross-model merge), while Ball Leveler v1 is exactly one
        // model, one class and one threshold. Sharing it would make five invalid
        // states representable.
        //
        // Geometry is NORMALIZED oriented-frame coordinates, like
        // camera_area.points. `view_revision` fingerprints the view-significant
        // camera fields the calibration was drawn against - a rotation / pitch /
        // roll / resolution / source change makes the geometry refer to a different
        // physical view, which must invalidate the measurement WITHOUT deleting the
        // operator's work.
        //
        // ADDITIVE ONLY. No existing table is dropped, altered or repurposed, so
        // every Digital Reader row survives this upgrade untouched.
        if (!run("CREATE TABLE IF NOT EXISTS ball_level_calibration ("
                 "    camera_id     INTEGER PRIMARY KEY REFERENCES camera(id),"
                 "    model_id      INTEGER NOT NULL REFERENCES model(id),"
                 "    class_id      INTEGER NOT NULL,"
                 "    conf          REAL    NOT NULL,"
                 "    rect_x        REAL    NOT NULL,"
                 "    rect_y        REAL    NOT NULL,"
                 "    rect_w        REAL    NOT NULL,"
                 "    rect_h        REAL    NOT NULL,"
                 "    y_100         REAL    NOT NULL,"
                 "    y_0           REAL    NOT NULL,"
                 "    hold_ms       INTEGER NOT NULL,"
                 "    view_revision TEXT    NOT NULL"
                 ")")) {
            return false;
        }
    }

    if (version < 15) {
        // Ball Leveler MULTI-ZONE (amendment §10.5). v14 held one model binding
        // AND one geometry in a single `camera_id PRIMARY KEY` row. A camera now
        // owns 1..4 zones sharing ONE model, so the two are split: the binding
        // stays one-per-camera (which is what makes "one model per camera" true
        // by CONSTRUCTION, not by a rule a caller can forget), and the geometry
        // becomes one row per (camera_id, zone_no).
        //
        // ADDITIVE ONLY, like every block above. `ball_level_calibration` is
        // deliberately LEFT IN PLACE and untouched: it is the operator's v14
        // work, the backfill below reads it, and dropping it would make this
        // migration lossy in exactly the way the amendment forbids.
        if (!run("CREATE TABLE IF NOT EXISTS ball_level_binding ("
                 "    camera_id     INTEGER PRIMARY KEY REFERENCES camera(id),"
                 "    model_id      INTEGER NOT NULL REFERENCES model(id),"
                 "    class_id      INTEGER NOT NULL,"
                 // The view the WHOLE camera's geometry was drawn against. Held
                 // once per camera rather than per zone: every zone of a camera
                 // is drawn on the same frame, so a per-zone copy could only
                 // ever disagree with itself.
                 "    view_revision TEXT    NOT NULL"
                 ")")) {
            return false;
        }
        // `zone_no` carries no CHECK on the 1..4 count: a CHECK constrains one
        // ROW and the cap is a property of the SET. The count and the
        // machine-wide uniqueness both belong to level::save_level_configuration,
        // which sees the whole set inside one transaction. A range CHECK is
        // still worth having — it is a per-row fact.
        if (!run("CREATE TABLE IF NOT EXISTS ball_level_zone ("
                 "    camera_id     INTEGER NOT NULL REFERENCES camera(id),"
                 "    zone_no       INTEGER NOT NULL CHECK (zone_no >= 1),"
                 "    conf          REAL    NOT NULL,"
                 "    rect_x        REAL    NOT NULL,"
                 "    rect_y        REAL    NOT NULL,"
                 "    rect_w        REAL    NOT NULL,"
                 "    rect_h        REAL    NOT NULL,"
                 "    y_100         REAL    NOT NULL,"
                 "    y_0           REAL    NOT NULL,"
                 "    hold_ms       INTEGER NOT NULL,"
                 "    PRIMARY KEY (camera_id, zone_no)"
                 ")")) {
            return false;
        }
        if (!migrate_ball_calibration_to_zones(db)) {
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
