// The NON-DESTRUCTIVE operating-mode switch transaction. Pure - Qt Core/Sql only,
// pops no UI, owns no widget - so it lives in denso_core and is fully
// unit-testable. It preserves every camera row (id + all connection/capture
// columns) AND both modes' saved configuration, switching the operating mode in
// one atomic transaction.
//
// The destructive `switch_and_reset` and the `preview_counts` dialog preview it
// fed were DELETED in Slice 1, not merely unwired. Once a switch destroys
// nothing there is nothing to preview and nothing to warn about, and leaving a
// working destructive transaction in the tree would be a loaded gun for any
// future call site - the operator decision that superseded it (2026-07-30) is
// only durable if the old path cannot be reached at all.
#pragma once

#include "mode/mode.h"

#include <QSqlDatabase>

#include <string>

namespace denso::mode {

struct ResetResult {
    bool ok = false;
    std::string error;  // SQL error verbatim on failure; empty on success
};

// NON-DESTRUCTIVE mode switch. Writes mode.target and disables brazing.enabled
// while preserving brazing.base_url, in ONE checked transaction, and DELETES
// NOTHING: Digital Reader attachments/classes/areas/readings/receipts and Ball
// Leveler calibration all survive, as does every camera row — including
// setup_complete and areas_need_review, which the destructive switch used to
// clear. Any statement or commit failure rolls the whole transaction back and
// returns {ok:false, error:<verbatim SQL error>}.
ResetResult switch_mode(const QSqlDatabase& db, TargetMode new_mode);

} // namespace denso::mode
