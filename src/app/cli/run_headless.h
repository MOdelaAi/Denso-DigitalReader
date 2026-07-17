// Executes the headless CLI modes. Lives in `denso` rather than denso_core
// because --check constructs the platform inference backend (Task 6).
//
// Every mode here runs under a QCoreApplication — never QApplication — so no
// display is required.
#pragma once

#include "cli/args.h"

namespace denso::app {

/// Returns the process exit code. See the plan's exit-code table:
/// 0 ok (--check-running: running) / 1 failed (--check-running: not running)
/// / 2 bad usage / 4 (--check-running only) cannot determine — the lock file
/// itself is unusable; must never be reported as the clean 1 a caller like
/// prerm needs to see to proceed.
int run_headless(const denso::cli::Command& cmd);

} // namespace denso::app
