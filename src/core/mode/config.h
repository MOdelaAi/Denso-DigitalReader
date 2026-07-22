// Load/save of the appliance operating mode over the `settings` key/value table
// (key "mode.target"). Modeled on brazing/config.cpp, but save() returns a
// CHECKED bool. Qt-free of widgets — denso_core only. No schema migration: the
// mode key rides the existing settings table (schema stays v13).
//
// NOTE: the switch-and-reset transaction (a later slice) writes mode.target
// DIRECTLY inside its own transaction, NOT via save(); save() is only for
// non-transactional callers/tests.
#pragma once

#include "mode/mode.h"

#include <QSqlDatabase>

namespace denso::mode {

/// Load mode.target; an absent or corrupt key resolves to DigitReader.
TargetMode load(const QSqlDatabase& db);

/// Upsert mode.target. Returns the SQL execution result (false on write error).
bool save(const QSqlDatabase& db, TargetMode m);

} // namespace denso::mode
