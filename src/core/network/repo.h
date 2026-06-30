// Persistence for NetConfig in the SQLite `net_config` table. User-editable
// (full CRUD), one row per interface. Typed struct in C++, rows in the DB;
// write/read failures surface as bool / nullopt so callers can react.
// Ported 1:1 from Rust `network::repo`.
#pragma once

#include "network/model.h"

#include <QSqlDatabase>

#include <optional>
#include <string>
#include <vector>

namespace denso::network {

/// Upsert one interface's configuration, keyed by `iface`. Returns false on a
/// write error.
bool save(const QSqlDatabase& db, const NetConfig& c);

/// Load one interface's saved configuration, or nullopt if unset (or on error).
std::optional<NetConfig> load(const QSqlDatabase& db, const std::string& iface);

/// Every saved interface configuration, ordered by interface name.
std::vector<NetConfig> all(const QSqlDatabase& db);

} // namespace denso::network
