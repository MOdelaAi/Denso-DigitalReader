// A game-style "Keep these display settings?" confirmation with an auto-revert
// countdown. Modal, always-on-top so it shows above a fullscreen window. Accept
// = Keep; Reject (button or timeout) = revert. Display-specific by design.
#pragma once

#include <QDialog>

class QLabel;
class QTimer;

namespace denso::ui {

class DisplayConfirmDialog : public QDialog {
    Q_OBJECT

public:
    explicit DisplayConfirmDialog(int seconds, QWidget* parent = nullptr);

protected:
    void showEvent(QShowEvent* event) override;

private:
    void tick();

    QLabel* message_ = nullptr;
    QTimer* timer_ = nullptr;
    int remaining_ = 0;
};

} // namespace denso::ui
