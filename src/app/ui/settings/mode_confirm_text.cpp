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

// What the mode being LEFT actually loses, named concretely. An operator does
// not recognise their work as "processing configuration"; they recognise it as
// the areas they drew and the formats they picked.
QString setup_nouns(mode::TargetMode m) {
    switch (m) {
        case mode::TargetMode::DigitReader:
            return QStringLiteral("model bindings, detection areas, reported zones "
                                  "and the number format of every zone");
        case mode::TargetMode::BallLeveler:
            return QStringLiteral("model bindings, level zones and their "
                                  "0% / 100% calibration");
    }
    return QStringLiteral("the configured camera setup");
}

} // namespace

QString mode_confirm_body(mode::TargetMode target) {
    // Exactly two modes exist, so the mode being LEFT is the opposite of `target`.
    const mode::TargetMode leaving =
        target == mode::TargetMode::BallLeveler ? mode::TargetMode::DigitReader
                                                : mode::TargetMode::BallLeveler;

    QStringList paras;
    paras << QStringLiteral("Switch to %1?").arg(mode_display_name(target));

    // The destructive warning comes FIRST and in the operator's own terms. The
    // previous copy led with what was preserved and promised "Nothing is
    // deleted"; that promise is now false, and a warning placed after a
    // reassurance is a warning most people never reach.
    paras << QStringLiteral(
                 "Switching operating mode stops reporting and clears the current "
                 "camera setup, zones, formats and calibration. The destination "
                 "mode will start unconfigured. This cannot be undone.");

    // Name the losses concretely, for the mode being left.
    paras << QStringLiteral(
                 "Your %1 setup - %2 - will be deleted, along with any stored "
                 "readings. Switching back later will NOT bring it back: you will "
                 "set that mode up again from the beginning.")
                 .arg(mode_display_name(leaving), setup_nouns(leaving));

    // The two things that DO survive. Stated last and stated narrowly, so it
    // cannot be misread as "my setup is safe" - it is deliberately a short list
    // of connection details, not of work.
    paras << QStringLiteral(
        "Your camera connections are kept - sources, credentials, resolution and "
        "orientation - and so is the server address. Nothing else is.");

    // The reporting guarantee: disabled, address kept, manual re-enable.
    paras << QStringLiteral(
        "Server reporting will be turned off. You must re-enable it yourself "
        "once %1 is set up.").arg(mode_display_name(target));

    return paras.join(QStringLiteral("\n\n"));
}

} // namespace denso::ui
