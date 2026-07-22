#include "ui/settings/mode_confirm_dialog.h"

#include "ui/settings/mode_confirm_text.h"

#include <QDialogButtonBox>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

namespace denso::ui {

ModeConfirmDialog::ModeConfirmDialog(mode::TargetMode target,
                                     const mode::SwitchCounts& counts,
                                     QWidget* parent)
    : QDialog(parent) {
    setWindowTitle(QStringLiteral("Switch and Reset Target Mode"));
    setObjectName(QStringLiteral("dialogPanel"));
    setModal(true);

    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(24, 24, 24, 24);
    v->setSpacing(16);

    auto* body = new QLabel(mode_confirm_body(target, counts));
    body->setObjectName(QStringLiteral("modeConfirmBody"));
    body->setWordWrap(true);
    body->setTextInteractionFlags(Qt::TextSelectableByMouse);
    v->addWidget(body);

    auto* buttons = new QDialogButtonBox;
    auto* cancel =
        buttons->addButton(QStringLiteral("Cancel"), QDialogButtonBox::RejectRole);
    auto* go = buttons->addButton(QStringLiteral("Switch and Reset"),
                                  QDialogButtonBox::AcceptRole);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    v->addWidget(buttons);

    // A3 / spec §7.1: the SAFE action is the default and holds initial focus, so
    // Enter cancels; closing the dialog (Esc / the window X) is already a reject.
    // Explicitly clear the accept button's auto-default so the button box cannot
    // promote the destructive action to default.
    go->setAutoDefault(false);
    go->setDefault(false);
    cancel->setAutoDefault(true);
    cancel->setDefault(true);
    cancel->setFocus();
}

} // namespace denso::ui
