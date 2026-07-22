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

#include <optional>

namespace denso::mode {

/// Load mode.target; an absent or corrupt key resolves to DigitReader.
TargetMode load(const QSqlDatabase& db);

/// Upsert mode.target. Returns the SQL execution result (false on write error).
bool save(const QSqlDatabase& db, TargetMode m);

/// Does `mode` still need first-run setup? The one place the §2.1 "Leveler setup
/// is unavailable" rule lives, so every status writer stays consistent.
///  - ball_leveler → ALWAYS true this release (ships no Leveler setup, so it is
///    never "configured"); independent of any camera state.
///  - digit_reader → true if NO camera row has setup_complete = 1 (setup
///    required); false when at least one completed camera exists — even if it is
///    inactive (NOT runtime()-based: a configured-but-disabled fleet is not
///    "setup required", spec §9).
///  - nullopt when the camera query fails → the caller OMITS the field from
///    status.json (undeterminable is never guessed).
std::optional<bool> mode_setup_required(const QSqlDatabase& db, TargetMode mode);

} // namespace denso::mode
