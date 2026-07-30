#include "ui/settings/mode_confirm_text.h"

#include <QStringList>

namespace denso::ui {

namespace {

// The operator-facing display name of each mode (spec §2, §7.1).
QString mode_display_name(mode::TargetMode m) {
    switch (m) {
        case mode::TargetMode::DigitReader: return QStringLiteral("Digital Number Reader");
        case mode::TargetMode::BallLeveler: return QStringLiteral("Floating Ball Leveler");
    }
    return QStringLiteral("Digital Number Reader");
}

} // namespace

QString mode_confirm_body(mode::TargetMode target) {
    // Exactly two modes exist, so the mode being LEFT is the opposite of `target`.
    const mode::TargetMode leaving =
        target == mode::TargetMode::BallLeveler ? mode::TargetMode::DigitReader
                                                : mode::TargetMode::BallLeveler;

    QStringList paras;
    paras << QStringLiteral("Switch to %1?").arg(mode_display_name(target));

    // Connections first - this was true under the destructive switch too, and
    // stays first because it is what an operator worries about most.
    paras << QStringLiteral(
        "Your camera connections are kept - sources, credentials, resolution "
        "and orientation are all preserved.");

    // The sentence that REPLACES the deletion warning. It names the mode being
    // left explicitly, because "nothing is deleted" is easy to read as "nothing
    // happens": the old mode's work is retained and will still be there on the
    // way back.
    paras << QStringLiteral(
                 "Nothing is deleted. Your %1 setup - model bindings, detection "
                 "areas, reported zones and stored readings - is kept exactly as "
                 "it is, and will still be there if you switch back. Any %2 setup "
                 "you have already done is kept too.")
                 .arg(mode_display_name(leaving), mode_display_name(target));

    // The one real cost of the switch, stated plainly so it is not a surprise.
    // Only digit_reader actually RESUMES. Ball Leveler has no processor yet and
    // lands on the guarded "not available" page, so promising that processing
    // "starts again on its own" would be a straight falsehood for that target.
    if (target == mode::TargetMode::DigitReader) {
        paras << QStringLiteral(
            "Processing pauses while %1 is prepared, then starts again on its "
            "own.").arg(mode_display_name(target));
    } else {
        paras << QStringLiteral(
            "Processing stops while %1 is prepared.")
            .arg(mode_display_name(target));
    }

    // The reporting guarantee (spec §6.6): disabled, address kept, manual re-enable.
    paras << QStringLiteral(
        "Server reporting will be turned off. The server address is kept; you "
        "must re-enable reporting yourself.");

    // GUARD (Slice 1): the destination's unavailability must appear BEFORE the
    // operator commits. Removing this line would advertise a wizard that
    // apply_camera_button_gate() still refuses to open.
    if (target == mode::TargetMode::BallLeveler) {
        paras << QStringLiteral(
            "Floating Ball Leveler setup is not available in this release.");
    }

    return paras.join(QStringLiteral("\n\n"));
}

} // namespace denso::ui
