// SQLite persistence base. One file, `denso.db`, next to the executable — the
// single durable store. Connection setup lives here; the schema and its
// version-gated migrations live in run_migrations(). Ported 1:1 from the Rust
// `db` module (rusqlite → Qt6::Sql / QSQLITE driver).
//
// Access control is by API surface, not SQL grants: each feature's `repo`
// exposes only the operations its data policy allows.
#pragma once

#include <QSqlDatabase>
#include <QString>

#include <optional>

namespace denso::db {

/// The database file inside the data dir (see denso::paths). Prefer
/// paths::db_file() directly in new code.
QString default_path();

/// Owns one uniquely-named QSqlDatabase connection, removing it on destruction
/// so file and in-memory test databases don't leak or collide. Move-only.
class Db {
public:
    Db(const Db&) = delete;
    Db& operator=(const Db&) = delete;
    Db(Db&& other) noexcept;
    Db& operator=(Db&& other) noexcept;
    ~Db();

    /// Open (creating if absent) the database at `path` in WAL mode, so the UI
    /// can read while a background thread writes. Does not run migrations —
    /// call run_migrations() after. Returns nullopt if the open fails.
    static std::optional<Db> open(const QString& path);

    /// Open a fresh, private in-memory database (tests). Each call gets its own
    /// connection so in-memory DBs don't collide.
    static std::optional<Db> open_in_memory();

    /// Open an EXISTING database read-only, for inspection that must not mutate
    /// it. Unlike open(), this does NOT run `PRAGMA journal_mode = WAL` — that
    /// pragma rewrites the file header, which is a mutation. Returns nullopt if
    /// the file is absent or unreadable, and never creates it (the --check
    /// contract: a missing DB is an empty configured-model set).
    ///
    /// Guarantee, stated precisely: this does not mutate the PRIMARY database.
    /// It is NOT unconditionally side-effect-free — a WAL reader needs the `-shm`
    /// index and SQLite may create it (and it can outlive an abnormal exit). So
    /// any root-side caller MUST drop to the target user first: that bounds
    /// OWNERSHIP, not mutation, and a root-owned `-shm` in an operator-owned data
    /// dir breaks the app. Do not "fix" this with `immutable=1` — that lets
    /// SQLite ignore WAL state and read a stale image, which could hide the very
    /// camera-model rows a caller is validating.
    static std::optional<Db> open_read_only(const QString& path);

    /// The underlying connection handle.
    QSqlDatabase handle() const;

private:
    explicit Db(QString name);
    QString name_;  // empty once moved-from
};

/// Apply any pending schema migrations, gated by `PRAGMA user_version` so
/// repeated runs are no-ops. Safe to call on every startup.
bool run_migrations(const QSqlDatabase& db);

} // namespace denso::db
