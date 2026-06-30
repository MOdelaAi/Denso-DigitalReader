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

/// Location of the database file: `denso.db` next to the executable, falling
/// back to the current directory if the application dir is unavailable.
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
