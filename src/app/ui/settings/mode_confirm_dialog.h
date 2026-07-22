// The destructive "Switch and Reset Target Mode" confirmation modal (spec §7.1,
// decision A3). It renders the pure mode_confirm_body() copy from REAL counts
// supplied by the caller and offers [Cancel] (default/safe) and [Switch and
// Reset] (destructive). It performs NO database read/write and NO teardown — the
// counts are handed in by the Slice-7 orchestrator, which reads them via
// mode::preview_counts immediately before showing this dialog. Accepting it only
// resolves the dialog (QDialog::Accepted); it starts no transaction itself.
#pragma once

#include "mode/mode.h"    // TargetMode
#include "mode/reset.h"   // mode::SwitchCounts

#include <QDialog>

namespace denso::ui {

class ModeConfirmDialog : public QDialog {
    Q_OBJECT

public:
    // `target` names the destination mode; `counts` are the real preview counts.
    ModeConfirmDialog(mode::TargetMode target, const mode::SwitchCounts& counts,
                      QWidget* parent = nullptr);
};

} // namespace denso::ui
