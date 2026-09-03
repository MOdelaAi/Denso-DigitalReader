// The brazing HTTP reporter's persisted config: whether reporting is on, the
// server base URL, and the reporting API path appended to it. Stored as three
// rows in the `settings` key/value table (keys "brazing.enabled",
// "brazing.base_url", "brazing.api_path"). Qt-free of widgets — denso_core only.
//
// No schema migration was needed for the path: `settings` is a key/value table,
// so the new key is a row, and an installation that predates the setting simply
// has no such row and gets brazing::kDefaultApiPath — the exact endpoint it was
// already posting to.
#pragma once

#include "brazing/url.h"   // kDefaultApiPath — the ONE definition of the default

#include <QSqlDatabase>

#include <string>

namespace denso::brazing {

struct BrazingConfig {
    bool        enabled = false;
    std::string base_url;  // e.g. "http://192.168.1.50:8098"; empty = unset
    // The path appended to base_url. Defaults to the shipped endpoint, which is
    // what makes a configuration with no brazing.api_path row behave exactly as
    // it did before the setting existed. Never empty as loaded.
    std::string api_path = kDefaultApiPath;
};

/// Load config, defaulting to {enabled=false, base_url="", api_path=
/// kDefaultApiPath} for missing keys. A brazing.api_path row that is present but
/// blank is treated as absent — blank is not a path anything could post to, and
/// the UI cannot produce one, so the only way to get here is an externally
/// written row.
BrazingConfig load(const QSqlDatabase& db);

/// Persist every field in ONE checked transaction. Returns false if the
/// transaction could not be opened or any row failed to write, rolling back
/// so a false result means nothing changed — a caller can then refuse to report
/// success and refuse to reconfigure a running pipeline, instead of the old
/// silent swallow. Modeled on mode::save (checked) and mode::switch_and_reset
/// (transactional).
///
/// The single exception is a driver with no transaction support at all, where
/// the upserts are issued directly and the result is best-effort. Every
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
