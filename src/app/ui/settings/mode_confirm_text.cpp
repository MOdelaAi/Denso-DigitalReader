#include "ui/settings/mode_confirm_text.h"

#include <QLocale>
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

// Group thousands deterministically with an EXPLICIT English (US) locale. The C
// locale does not group (QLocale::c().toString(1284) == "1284"), so the spec's
// "1,284" needs a real grouping locale — and an explicit one, not the ambient
// system locale, so the rendering is deterministic across machines.
QString grouped(int n) {
    return QLocale(QLocale::English, QLocale::UnitedStates)
        .toString(static_cast<qlonglong>(n));
}

// "N reported zones (z1, z2, …)" — or just "0 reported zones" when there are
// none, so no empty "()" is rendered.
QString zones_clause(const std::vector<int>& zones) {
    if (zones.empty()) {
        return QStringLiteral("0 reported zones");
    }
    QStringList nums;
    nums.reserve(static_cast<int>(zones.size()));
    for (int z : zones) nums << QString::number(z);
    return QStringLiteral("%1 reported zones (%2)")
        .arg(grouped(static_cast<int>(zones.size())), nums.join(QStringLiteral(", ")));
}

} // namespace

QString mode_confirm_body(mode::TargetMode target, const mode::SwitchCounts& c) {
    // Exactly two modes exist, so the mode being LEFT is the opposite of `target`.
    const mode::TargetMode leaving =
        target == mode::TargetMode::BallLeveler ? mode::TargetMode::DigitReader
                                                : mode::TargetMode::BallLeveler;

    QStringList paras;
    paras << QStringLiteral("Switch to %1?").arg(mode_display_name(target));

    // Retained connections FIRST — the dialog must never imply cameras are deleted.
    paras << QStringLiteral(
                 "%1 camera connections will be kept — sources, credentials, "
                 "resolution and orientation are preserved.")
                 .arg(grouped(c.cameras));

    // The destroyed processing setup names the mode being LEFT, with real counts.
    paras << QStringLiteral(
                 "Their %1 setup will be deleted: %2 model bindings, "
                 "%3 detection areas, %4, %5 stored readings, and %6 "
                 "model-rollback receipts. Each camera will need processing "
                 "setup again.")
                 .arg(mode_display_name(leaving))
                 .arg(grouped(c.model_bindings))
                 .arg(grouped(c.areas))
                 .arg(zones_clause(c.zones))
                 .arg(grouped(c.readings))
                 .arg(grouped(c.receipts));

    // The reporting guarantee (spec §6.6): disabled, address kept, manual re-enable.
    paras << QStringLiteral(
        "Server reporting will be turned off. The server address is kept; you "
        "must re-enable reporting yourself.");

    // The destination's unavailability must appear BEFORE the operator commits.
    if (target == mode::TargetMode::BallLeveler) {
        paras << QStringLiteral(
            "Floating Ball Leveler setup is not available in this release.");
    }

    paras << QStringLiteral("This cannot be undone.");

    return paras.join(QStringLiteral("\n\n"));
}

} // namespace denso::ui
