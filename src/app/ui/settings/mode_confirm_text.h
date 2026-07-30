// The pure copy builder for the operating-mode switch confirmation. Qt Core only
// - NO Qt Widgets - so it is unit-testable in the backend-free denso_tests and is
// compiled into BOTH denso_tests and denso_app (the grid_layout.cpp precedent).
// It reads no database and pops no UI.
//
// It takes NO counts. The switch is non-destructive, so there is nothing to
// count and nothing to warn about; the copy's job is now to state accurately
// what IS preserved. Keeping a counts parameter would have kept
// mode::preview_counts alive purely to feed a sentence that no longer exists.
#pragma once

#include "mode/mode.h"    // TargetMode

#include <QString>

namespace denso::ui {

// Build the confirmation body for switching TO `target`. The copy states that
// camera connections are kept, that BOTH modes' saved configuration is kept,
// that processing pauses while the target mode is prepared, and that server
// reporting is turned off while its address is kept. It must never promise
// deletion or irreversibility - the switch does neither. For ball_leveler it
// also states that its setup is not available in this release.
QString mode_confirm_body(mode::TargetMode target);

} // namespace denso::ui
