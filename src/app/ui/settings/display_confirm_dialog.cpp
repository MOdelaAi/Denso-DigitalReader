#include "ui/settings/display_confirm_dialog.h"

#include <QDialogButtonBox>
#include <QLabel>
#include <QPushButton>
#include <QShowEvent>
#include <QTimer>
#include <QVBoxLayout>

namespace denso::ui {

DisplayConfirmDialog::DisplayConfirmDialog(int seconds, QWidget* parent)
    : QDialog(parent), remaining_(seconds) {
    setWindowTitle(QStringLiteral("Confirm display settings"));
    setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
    setModal(true);

    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(24, 24, 24, 24);
    v->setSpacing(16);
    message_ = new QLabel;
    message_->setAlignment(Qt::AlignCenter);
    v->addWidget(message_);

    auto* buttons = new QDialogButtonBox;
    auto* keep = buttons->addButton(QStringLiteral("Keep"), QDialogButtonBox::AcceptRole);
    keep->setProperty("gold", true);
    buttons->addButton(QStringLiteral("Revert"), QDialogButtonBox::RejectRole);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    v->addWidget(buttons);

    timer_ = new QTimer(this);
    timer_->setInterval(1000);
    connect(timer_, &QTimer::timeout, this, &DisplayConfirmDialog::tick);
    timer_->start();
    tick();  // paint the initial count
}

void DisplayConfirmDialog::showEvent(QShowEvent* event) {
    QDialog::showEvent(event);
    // Now that the window exists, force it above the (possibly fullscreen) parent.
    raise();
    activateWindow();
}

void DisplayConfirmDialog::tick() {
    if (remaining_ <= 0) {
        timer_->stop();
        reject();  // timeout -> revert
        return;
    }
    message_->setText(
        QStringLiteral("Keep these display settings?\nReverting in %1s…").arg(remaining_));
    --remaining_;
}

} // namespace denso::ui
