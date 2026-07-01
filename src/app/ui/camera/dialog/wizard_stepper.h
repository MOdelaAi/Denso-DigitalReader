// A thin, non-interactive step indicator for the camera add/edit wizard:
// "① Source — ② Configure — ③ Areas" with the current step emphasized, earlier
// steps shown as done, later steps dim. Pure presentation; the dialog drives it
// via set_current(). Navigation stays with the wizard's Back/Next buttons.
#pragma once

#include <QList>
#include <QStringList>
#include <QWidget>

class QLabel;

namespace denso::ui {

class WizardStepper : public QWidget {
    Q_OBJECT

public:
    explicit WizardStepper(const QStringList& steps, QWidget* parent = nullptr);
    void set_current(int index);  // 0-based step to emphasize

private:
    QList<QLabel*> labels_;
};

} // namespace denso::ui
