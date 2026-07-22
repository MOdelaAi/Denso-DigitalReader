// The pure copy builder for the destructive "Switch and Reset Target Mode"
// confirmation (spec §7.1). Qt Core only — NO Qt Widgets — so it is unit-testable
// in the backend-free denso_tests and is compiled into BOTH denso_tests and
// denso_app (the grid_layout.cpp precedent). It renders REAL counts supplied by
// the caller (mode::preview_counts); it reads no database and pops no UI.
#pragma once

#include "mode/mode.h"    // TargetMode
#include "mode/reset.h"   // mode::SwitchCounts

#include <QString>

namespace denso::ui {

// Build the confirmation body for switching TO `target`. The copy states, from
// the real `counts`, that the N camera CONNECTIONS are kept (never that cameras
// are deleted), which processing setup is destroyed (with grouped-thousands
// counts and the distinct reported-zone list), that server reporting is turned
// off while the address is kept, and — for ball_leveler — that its setup is not
// available in this release. (spec §7.1)
QString mode_confirm_body(mode::TargetMode target, const mode::SwitchCounts& counts);

} // namespace denso::ui
