// The Models step's empty state, as a pure function of the policy's OWN output.
//
// This unit exists because a blank Models step is indistinguishable between a
// missing manifest, a failed provenance check, a wrong-mode model and an empty
// catalog — and an operator staring at two empty boxes cannot tell which. It
// turns the stable reason codes that models::model_compatibility already emits
// into one operator-facing sentence.
//
// IT IS NOT A SECOND COMPATIBILITY MATRIX, and must never become one. It decides
// nothing about which model may run in which mode: it is handed the verdicts and
// only chooses wording. It never maps a model to a mode, never names a model
// family, and never infers a reason a policy call did not produce — an
// unrecognised code is passed through verbatim rather than guessed at.
//
// Qt Core only (no Widgets, no SQL, no filesystem), so it is compiled into
// denso_tests as well as denso_app and unit-tested without a display.
#pragma once

#include "mode/mode.h"

#include <QString>

#include <string>
#include <vector>

namespace denso::ui {

/// One catalog model the Models step is NOT offering: how it identifies itself in
/// a diagnostic, and the stable reason code the policy rejected it with.
struct RejectedModelNote {
    QString     display;      // diagnostic-safe name — NEVER a credential-bearing path
    std::string reason_code;  // verbatim from models::CompatibilityResult
};

/// The message to show when the Models step can offer nothing in `mode`.
///
/// `rejected` is every catalog row that was evaluated and refused, in catalog
/// order. An EMPTY `rejected` means the catalog itself is empty — a different
/// fault from "models exist but none qualify", and worded as such.
///
/// The returned text always contains the raw reason code(s), because those are a
/// stable, greppable file-format-grade vocabulary shared with status.json: the
/// operator quotes them and the log confirms them.
/// `manifest_declares_nothing` is the ManifestView's own answer to "did any
/// manifest load at all" (no generations — absent, or unparseable and collapsed
/// to empty). It gates the seeding remedy and nothing else. It is NOT a
/// compatibility input: a reason code says a model was refused, this says whether
/// installing the packaged manifest could possibly change that. A manifest that
/// loaded and simply does not cover one artifact is a different fault, and
/// seed-manifest would correctly refuse to "fix" it.
QString model_empty_state_text(denso::mode::TargetMode mode,
                               const std::vector<RejectedModelNote>& rejected,
                               bool manifest_declares_nothing);

/// The one-line operator explanation for a single stable reason code. Exposed for
/// tests and for reuse anywhere a single model's refusal is reported. An
/// unrecognised code returns the code itself — this function never invents a
/// reason the policy did not give.
QString model_reason_text(const std::string& reason_code);

}  // namespace denso::ui
