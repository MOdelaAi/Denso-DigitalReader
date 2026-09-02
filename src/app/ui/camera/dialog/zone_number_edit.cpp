#include "ui/camera/dialog/zone_number_edit.h"

#include "camera/camera.h"   // kMinZone / kMaxZone / zone_in_range — THE bound

#include <QCheckBox>
#include <QLabel>
#include <QLineEdit>
#include <QSignalBlocker>
#include <QStringList>
#include <QVBoxLayout>

#include <utility>

namespace denso::ui {

namespace {

/// The range, spelled once for the operator.
QString range_text() {
    return QStringLiteral("%1 to %2").arg(camera::kMinZone).arg(camera::kMaxZone);
}

/// True only for a non-empty run of ASCII digits. This is the whole
/// well-formedness rule, and it is what rejects "-1", "1.5", "1 2" and "abc"
/// in one test — a QIntValidator would accept "-1" as an intermediate edit, and
/// toInt()'s own parse would quietly read "12abc" as 12.
bool is_digits(const QString& t) {
    if (t.isEmpty()) {
        return false;
    }
    for (const QChar c : t) {
        if (c < QLatin1Char('0') || c > QLatin1Char('9')) {
            return false;
        }
    }
    return true;
}

}  // namespace

ZoneNumberEdit::ZoneNumberEdit(bool allow_unassigned, QWidget* parent)
    : QWidget(parent), allow_unassigned_(allow_unassigned) {
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(4);

    if (allow_unassigned_) {
        report_check_ = new QCheckBox(QStringLiteral("Report this area to a zone"));
        report_check_->setObjectName(QStringLiteral("zoneReportCheck"));
        connect(report_check_, &QCheckBox::toggled, this, [this](bool on) {
            edit_->setEnabled(on);
            if (on) {
                edit_->setFocus();
            } else {
                const QSignalBlocker block(edit_);
                edit_->clear();
            }
            revalidate();
        });
        v->addWidget(report_check_);
    }

    edit_ = new QLineEdit;
    edit_->setObjectName(QStringLiteral("zoneNumberEdit"));
    edit_->setPlaceholderText(QStringLiteral("Zone number (%1)").arg(range_text()));
    // No input mask and no validator: a mask would swallow "100" a character at
    // a time and leave the operator staring at "10" with no explanation. Bad
    // input is ACCEPTED into the field and then NAMED below it.
    // textChanged, not textEdited: every write to this field has to be
    // re-judged, including a programmatic one. set_zone() blocks the signal and
    // calls revalidate() itself, so painting a selection still never re-emits.
    connect(edit_, &QLineEdit::textChanged, this,
            [this](const QString&) { revalidate(); });
    v->addWidget(edit_);

    used_ = new QLabel;
    used_->setWordWrap(true);
    used_->setObjectName(QStringLiteral("zoneUsedLabel"));
    used_->setStyleSheet(QStringLiteral("color:#94a3b8;"));
    v->addWidget(used_);

    warning_ = new QLabel;
    warning_->setWordWrap(true);
    warning_->setObjectName(QStringLiteral("zoneWarningLabel"));
    warning_->setStyleSheet(QStringLiteral("color:#f87171;"));
    warning_->setVisible(false);
    v->addWidget(warning_);

    if (allow_unassigned_) {
        edit_->setEnabled(false);   // detection-only is the default for a new ROI
    }
    refresh_used_label();
    revalidate();
}

void ZoneNumberEdit::set_unavailable(std::map<int, QString> unavailable) {
    unavailable_ = std::move(unavailable);
    refresh_used_label();
    // Availability changed under a value that was fine a moment ago (another
    // area just claimed it), so the verdict has to be recomputed, not kept.
    revalidate();
}

void ZoneNumberEdit::set_zone(std::optional<int> zone) {
    syncing_ = true;
    if (report_check_) {
        const QSignalBlocker block(report_check_);
        report_check_->setChecked(zone.has_value());
        edit_->setEnabled(zone.has_value());
    }
    {
        const QSignalBlocker block(edit_);
        edit_->setText(zone ? QString::number(*zone) : QString());
    }
    revalidate();
    syncing_ = false;
}

void ZoneNumberEdit::revalidate() {
    const std::optional<int> before = zone_;
    const QString text = edit_->text().trimmed();
    const bool wants_zone = !report_check_ || report_check_->isChecked();

    problem_.clear();
    if (!wants_zone) {
        // Detection-only: the field is empty and disabled. This is the ONLY way
        // to express unassigned; typing a number is the only way to express a
        // zone, and 0 is not one of the numbers that works.
        zone_ = std::nullopt;
    } else if (text.isEmpty()) {
        problem_ = QStringLiteral("Enter a zone number from %1.").arg(range_text());
    } else if (!is_digits(text)) {
        problem_ =
            QStringLiteral("\"%1\" is not a zone number. Use a whole number from %2.")
                .arg(text, range_text());
    } else {
        bool ok = false;
        // Cap the length before toInt(): a 12-digit entry overflows int and
        // would come back as a plausible-looking in-range number.
        const int value = text.size() > 3 ? -1 : text.toInt(&ok);
        if (!ok || !camera::zone_in_range(value)) {
            problem_ = QStringLiteral("Zone %1 does not exist. Use %2.")
                           .arg(text, range_text());
        } else {
            const auto owner = unavailable_.find(value);
            if (owner != unavailable_.end()) {
                problem_ = owner->second.isEmpty()
                               ? QStringLiteral("Zone %1 is already used.").arg(value)
                               : QStringLiteral("Zone %1 is already used by %2.")
                                     .arg(value)
                                     .arg(owner->second);
            } else {
                zone_ = value;
            }
        }
    }

    warning_->setText(problem_);
    warning_->setVisible(!problem_.isEmpty());
    // The field itself carries the state too: on a touch panel the operator may
    // never look below the box they are typing in.
    edit_->setStyleSheet(problem_.isEmpty()
                             ? QString()
                             : QStringLiteral("border:1px solid #f87171;"));

    if (!syncing_ && problem_.isEmpty() && zone_ != before) {
        emit zone_changed(zone_);
    }
}

void ZoneNumberEdit::refresh_used_label() {
    if (unavailable_.empty()) {
        used_->setText(QStringLiteral("No zone numbers are in use yet."));
        return;
    }
    // The USED numbers, not all 99 candidates: the operator needs to know what
    // to avoid, and a hundred-entry list of mostly-free numbers says nothing.
    QStringList nos;
    for (const auto& [zone, owner] : unavailable_) {
        nos << QString::number(zone);
    }
    used_->setText(
        QStringLiteral("Used zones: %1").arg(nos.join(QStringLiteral(", "))));
}

} // namespace denso::ui
