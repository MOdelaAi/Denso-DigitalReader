// The pure copy builder for the operating-mode switch confirmation. Qt Core only
// - NO Qt Widgets - so it is unit-testable in the backend-free denso_tests and is
// compiled into BOTH denso_tests and denso_app (the grid_layout.cpp precedent).
// It reads no database and pops no UI.
//
// It takes NO counts. The switch IS destructive, but a count of rows about to be
// deleted would add a query that can fail for no decision the operator can act
// on: the answer to "how many areas will I lose" does not change what the button
// does. The copy names the KINDS of thing that are cleared instead, which is
// what an operator needs to decide.
#pragma once

#include "mode/mode.h"    // TargetMode

#include <QString>

namespace denso::ui {

// Build the confirmation body for switching TO `target`. The copy must state the
// truth about a DESTRUCTIVE switch: reporting stops, the configured camera
// processing setup is cleared (areas and their number formats, or Ball zones and
// calibration), the destination opens unconfigured, and none of it can be
// restored automatically. It must never promise preservation - the switch does
// not preserve anything except the camera connections and the server address.
QString mode_confirm_body(mode::TargetMode target);

} // namespace denso::ui
