// The destructive "Switch and Reset Target Mode" transaction (spec §6.3) and the
// read-only count preview that backs its confirmation dialog (spec §7.1). Pure —
// Qt Core/Sql only, pops no UI, owns no widget — so it lives in denso_core and is
// fully unit-testable. The reset preserves every camera row (id + all
// connection/capture columns) and destroys ONLY the mode-owned processing
// workspace, switching the operating mode in one atomic transaction.
#pragma once

#include "mode/mode.h"

#include <QSqlDatabase>

#include <optional>
#include <string>
#include <vector>

namespace denso::mode {

// Real counts for the A3 destructive-confirmation dialog (spec §7.1). Every field
// is a live count read immediately before the dialog is shown, never a guess.
struct SwitchCounts {
    int cameras = 0;              // camera rows kept (retained, not deleted)
    int model_bindings = 0;       // camera_model rows
    int areas = 0;                // camera_area rows
    std::vector<int> zones;       // distinct non-null, non-zero zone numbers (ascending)
    int readings = 0;             // reading rows
    int receipts = 0;             // model_migration_receipt rows
};

// Returns nullopt if ANY count/zone query fails. A3 requires REAL counts before
// destruction — a query error must abort the confirmation, never render as "0",
// which would let an operator authorise deleting data they were told was empty.
std::optional<SwitchCounts> preview_counts(const QSqlDatabase& db);

struct ResetResult {
    bool ok = false;
    std::string error;  // SQL error verbatim on failure; empty on success
};

// Destroys the mode-owned workspace and switches the operating mode in ONE
// checked transaction (spec §6.3): deletes camera_model_class (unconditionally, to
// repair pre-existing orphans), camera_model, camera_area, reading, and
// model_migration_receipt; resets every camera's setup_complete/areas_need_review
// (never deleting a camera row or any of its connection/capture columns); writes
// mode.target and disables brazing.enabled while preserving brazing.base_url. Any
// statement or commit failure rolls the whole transaction back and returns
// {ok:false, error:<verbatim SQL error>}. Pops no UI.
ResetResult switch_and_reset(const QSqlDatabase& db, TargetMode new_mode);

} // namespace denso::mode
