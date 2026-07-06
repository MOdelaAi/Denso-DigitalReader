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

/// Persist both fields. Write errors are silently ignored (a DB hiccup must
/// never crash the UI).
void save(const QSqlDatabase& db, const BrazingConfig& cfg);

} // namespace denso::brazing
