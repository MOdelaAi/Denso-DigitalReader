// The "Switch Target Mode" confirmation modal. It renders the pure
// mode_confirm_body() copy and offers [Cancel] (default/safe) and [Switch].
// It performs NO database read/write and NO teardown, and takes NO counts —
// the switch deletes nothing, so there is nothing to preview. Accepting it only
// resolves the dialog (QDialog::Accepted); it starts no transaction itself.
#pragma once

#include "mode/mode.h"    // TargetMode

#include <QDialog>

namespace denso::ui {

class ModeConfirmDialog : public QDialog {
    Q_OBJECT

public:
    // `target` names the destination mode.
    explicit ModeConfirmDialog(mode::TargetMode target, QWidget* parent = nullptr);
};

} // namespace denso::ui
