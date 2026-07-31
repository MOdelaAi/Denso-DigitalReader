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
    // Now true for BOTH targets: each mode has a real runtime, and the models for
    // the destination are loaded after the switch commits, so processing pauses
    // and then resumes on its own either way.
    paras << QStringLiteral(
        "Processing pauses while %1 is prepared, then starts again on its own.")
        .arg(mode_display_name(target));

    // The reporting guarantee (spec §6.6): disabled, address kept, manual re-enable.
    paras << QStringLiteral(
        "Server reporting will be turned off. The server address is kept; you "
        "must re-enable reporting yourself.");

    // The Slice-1 "not available in this release" paragraph was REMOVED at
    // activation, together with the other production guards. It must not come
    // back in isolation: it exists to warn that the destination has no runtime,
    // and the destination now has one.
    return paras.join(QStringLiteral("\n\n"));
}

} // namespace denso::ui
