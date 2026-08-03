// The DESTRUCTIVE operating-mode switch transaction. Pure - Qt Core/Sql only,
// pops no UI, owns no widget - so it lives in denso_core and is fully
// unit-testable.
//
// It preserves every camera ROW (id + all connection/capture columns) and the
// backend ADDRESS, and destroys everything else the outgoing mode configured:
// model bindings, class selections, ROI areas and their decimal formats, stored
// readings, migration receipts, Ball Leveler bindings/zones/calibration, and the
// per-camera setup flags. The destination mode therefore opens UNCONFIGURED.
//
// This supersedes the non-destructive switch (operator decision, 2026-07-31).
// Preserving both modes' configuration across a switch cost more engineering
// than it was worth, and the guarantee that actually matters is the one this
// transaction still makes: either the whole switch lands, or the previously
// persisted mode and its configuration remain exactly as they were.
//
// There is deliberately no `preview_counts` companion. The confirmation states
// plainly that the current setup is cleared; a count of rows about to be deleted
// would add a query that can fail for no decision the operator can act on.
#pragma once

#include "mode/mode.h"

#include <QSqlDatabase>

#include <string>

namespace denso::mode {

struct ResetResult {
    bool ok = false;
    std::string error;  // SQL error verbatim on failure; empty on success
};

// DESTRUCTIVE mode switch, in ONE checked transaction: writes mode.target,
// disables brazing.enabled, and clears the configured processing setup of BOTH
// modes. `brazing.base_url` is preserved so the operator need not retype it; no
// table is dropped, and camera rows are UPDATEd, never deleted.
//
// Any statement or commit failure rolls the WHOLE transaction back and returns
// {ok:false, error:<verbatim SQL error>} - the previously persisted mode and
// every configuration row survive intact.
//
// CALL ORDER IS PART OF THE CONTRACT: the caller must have torn down the old
// mode's runtime (joining capture/inference threads and destroying the reporter)
// BEFORE calling this, so nothing can still be producing readings into a
// configuration this is about to delete. See MainWindow::perform_switch.
ResetResult switch_and_reset(const QSqlDatabase& db, TargetMode new_mode);

} // namespace denso::mode
