// A thin, non-interactive step indicator for the camera add/edit wizard:
// "① Source — ② Configure — ③ Models — ④ Areas" with the current step
// emphasized, earlier steps shown as done, later steps dim. Pure presentation;
// the dialog drives it via set_steps()/set_current(). Navigation stays with the
// wizard's Back/Next buttons.
//
// The labels are settable because the wizard's fourth step depends on the
// operating mode. The stepper does not know or read the mode — the dialog, which
// already owns which page is showing, tells it what the steps are called.
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
    /// Replace the step labels. A no-op when they are already these, so the
    /// dialog can call it on every page switch without rebuilding widgets or
    /// losing the emphasized step.
    void set_steps(const QStringList& steps);
    void set_current(int index);  // 0-based step to emphasize

private:
    void rebuild(const QStringList& steps);

    QStringList steps_;
    QList<QLabel*> labels_;
    int current_ = 0;
};

} // namespace denso::ui
