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
/// / 2 bad usage.
int run_headless(const denso::cli::Command& cmd);

} // namespace denso::app
