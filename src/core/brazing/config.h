// The brazing HTTP reporter's persisted config: whether reporting is on, and the
// server base URL (path is fixed at /api/brazing/update). Stored as two rows in
// the `settings` key/value table (keys "brazing.enabled" / "brazing.base_url").
// Qt-free of widgets — denso_core only.
#pragma once

#include <QSqlDatabase>

#include <string>

namespace denso::brazing {

struct BrazingConfig {
    bool        enabled = false;
    std::string base_url;  // e.g. "http://192.168.1.50:8098"; empty = unset
};

/// Load config, defaulting to {enabled=false, base_url=""} for missing keys.
BrazingConfig load(const QSqlDatabase& db);

/// Persist both fields in ONE checked transaction. Returns false if the
/// transaction could not be opened or either row failed to write, rolling back
/// so a false result means nothing changed — a caller can then refuse to report
/// success and refuse to reconfigure a running pipeline, instead of the old
/// silent swallow. Modeled on mode::save (checked) and mode::switch_and_reset
/// (transactional).
///
/// The single exception is a driver with no transaction support at all, where
/// the two upserts are issued directly and the result is best-effort. Every
/// supported build (QSQLITE) takes the transactional path.
bool save(const QSqlDatabase& db, const BrazingConfig& cfg);

/// The row writes ALONE, with no transaction control, for a caller that owns an
/// enclosing transaction and needs these rows to land or roll back together with
/// its own. save() is exactly this wrapped in a checked transaction. Split out
/// because SQLite has no nested transactions: a caller inside one cannot go
/// through save() at all.
/// NOT a general-purpose save: on its own it is NOT atomic. Call it only
/// from code that has already opened a transaction and will commit or roll
/// back around it; everything else must use save().
bool save_rows(const QSqlDatabase& db, const BrazingConfig& cfg);

} // namespace denso::brazing
