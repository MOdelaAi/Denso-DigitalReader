#include "ui/camera/dialog/wizard_stepper.h"

#include <QHBoxLayout>
#include <QLabel>

namespace denso::ui {

namespace {
// Circled digits for steps 1..5; beyond that fall back to a plain number.
const char* const kCircled[] = {"①", "②", "③", "④", "⑤"};
constexpr int kCircledCount = 5;

const char* const kCurrentCss = "color:#facc15; font-weight:600;";  // gold accent
const char* const kDoneCss = "color:#e5e7eb;";                      // completed
const char* const kUpcomingCss = "color:#6b7280;";                 // dim
}

WizardStepper::WizardStepper(const QStringList& steps, QWidget* parent)
    : QWidget(parent) {
    auto* row = new QHBoxLayout(this);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(10);

    for (int i = 0; i < steps.size(); ++i) {
        if (i > 0) {
            auto* sep = new QLabel(QStringLiteral("—"));
            sep->setStyleSheet(QStringLiteral("color:#4b5563;"));
            row->addWidget(sep, 0);
        }
        const QString num = i < kCircledCount ? QString::fromUtf8(kCircled[i])
                                              : QString::number(i + 1);
        auto* l = new QLabel(QStringLiteral("%1  %2").arg(num, steps[i]));
        labels_.append(l);
        row->addWidget(l, 0);
    }
    row->addStretch(1);
    set_current(0);
}

void WizardStepper::set_current(int index) {
    for (int i = 0; i < labels_.size(); ++i) {
        const char* css =
            i == index ? kCurrentCss : (i < index ? kDoneCss : kUpcomingCss);
        labels_[i]->setStyleSheet(QString::fromLatin1(css));
    }
}

} // namespace denso::ui
