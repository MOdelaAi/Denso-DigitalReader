// Settings persistence over the `settings` key/value table. User-editable
// (read + write the whole struct). All paths fail silently to a sensible
// default — a DB hiccup must never crash or block the UI. Also owns the
// one-time `settings.json` import. Ported 1:1 from Rust `settings::repo`
// (the resolution presets / preset_index live in settings.{h,cpp}).
#pragma once

#include "settings/settings.h"

#include <QSqlDatabase>
#include <QString>

namespace denso::settings {

/// Load settings from the DB, falling back to defaults for any missing or
/// unreadable key.
Settings load(const QSqlDatabase& db);

/// Persist all settings fields to the DB. Write errors are silently ignored.
void save(const QSqlDatabase& db, const Settings& settings);

/// One-time migration of a pre-SQLite `settings.json` into the DB. If the file
/// exists and parses, its values are persisted and the file is deleted so this
/// never runs again. A missing or corrupt file is left untouched.
void import_legacy(const QSqlDatabase& db, const QString& json_path);

} // namespace denso::settings
