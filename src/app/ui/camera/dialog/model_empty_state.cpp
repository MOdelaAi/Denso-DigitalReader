#include "ui/camera/dialog/model_empty_state.h"

#include <QStringList>

#include <algorithm>

namespace denso::ui {
namespace {

QString mode_label(denso::mode::TargetMode mode) {
    // The serialized mode token, not a prettified name: it is what `mode.target`
    // holds, what status.json reports and what the operator will be asked to
    // quote. Prettifying it here would produce a word that appears nowhere else.
    return QString::fromLatin1(denso::mode::to_string(mode));
}

}  // namespace

QString model_reason_text(const std::string& reason_code) {
    // Every arm below corresponds 1:1 to a reject() in
    // src/core/models/compatibility.cpp. Adding a code there without adding it
    // here degrades gracefully — the default arm shows the raw code, which is
    // still actionable — but the pairing is asserted by a test.
    if (reason_code == "model_undeclared")
        return QStringLiteral(
            "not declared in the model manifest — the manifest is missing from the "
            "models directory, or does not cover this artifact");
    if (reason_code == "model_unknown_id")
        return QStringLiteral(
            "declares an identity this release does not recognise");
    if (reason_code == "model_family_mismatch")
        return QStringLiteral(
            "declares a family that disagrees with its registered identity");
    if (reason_code == "model_shape_unsupported")
        return QStringLiteral(
            "declares a task or input size this release cannot run");
    if (reason_code == "model_classes_mismatch")
        return QStringLiteral(
            "its class names do not match what the manifest declares");
    if (reason_code == "model_provenance_failed")
        return QStringLiteral(
            "failed provenance — its file hashes or build platform do not match "
            "the manifest, so it is not the approved artifact");
    if (reason_code == "model_mode_incompatible")
        return QStringLiteral("is not allowed in this operating mode");
    // Unrecognised: report the code itself rather than inventing a cause.
    return QString::fromStdString(reason_code);
}

QString model_empty_state_text(denso::mode::TargetMode mode,
                               const std::vector<RejectedModelNote>& rejected,
                               bool manifest_declares_nothing) {
    const QString mode_name = mode_label(mode);

    if (rejected.empty()) {
        // Nothing was even evaluated: the catalog holds no models at all. Distinct
        // from "models exist but none qualify" and must not be worded as a mode
        // problem, which would send the operator to change modes for nothing.
        return QStringLiteral(
                   "No detection models are installed.\n\n"
                   "The model catalog is empty, so there is nothing to offer in "
                   "%1. Install the model artifacts and their manifest, then "
                   "reopen this step.")
            .arg(mode_name);
    }

    QStringList lines;
    for (const RejectedModelNote& r : rejected) {
        lines << QStringLiteral("  • %1 — %2 (%3)")
                     .arg(r.display, model_reason_text(r.reason_code),
                          QString::fromStdString(r.reason_code));
    }

    // Lead with the mode, because that is the question the operator is actually
    // asking, then give the per-model reason. The count is stated so a truncated
    // or scrolled list cannot read as the whole story.
    QString text =
        QStringLiteral(
            "No compatible models for %1.\n\n"
            "%2 model%3 in the catalog %4 not selectable here:\n%5")
            .arg(mode_name)
            .arg(rejected.size())
            .arg(rejected.size() == 1 ? QString() : QStringLiteral("s"),
                 rejected.size() == 1 ? QStringLiteral("is") : QStringLiteral("are"),
                 lines.join(QStringLiteral("\n")));

    // A remedy, under TWO conditions that are both necessary.
    //
    // `model_undeclared` alone is NOT enough, and assuming it was is a mistake
    // this comment exists to prevent. That code means "the manifest does not
    // cover this artifact", which includes a perfectly good manifest that simply
    // does not describe an engine the operator put there themselves. Seeding
    // cannot help that box: seed-manifest inspects the same state and refuses,
    // so recommending it would hand the operator a command that fails - the
    // broken remedy this is meant to avoid, arrived at from the other direction.
    //
    // The second condition is the manifest's own emptiness, which is the state
    // seeding actually fixes and the one that shipped: engines present, no
    // manifest beside them, every row undeclared.
    //
    // Still no compatibility knowledge here: one input is a reason code the
    // policy handed us, the other is a fact about the manifest the caller read.
    const bool any_undeclared =
        std::any_of(rejected.begin(), rejected.end(), [](const RejectedModelNote& r) {
            return r.reason_code == "model_undeclared";
        });
    if (any_undeclared && manifest_declares_nothing) {
        text += QStringLiteral(
            "\n\nA model is declared by the manifest stored beside it. If the "
            "manifest was never installed, an administrator can place the "
            "packaged one on the appliance with:\n"
            "  sudo denso-setup seed-manifest");
    }

    return text;
}

}  // namespace denso::ui
