// The single readiness verdict, shared by boot and --check. READ-ONLY by
// contract: it observes state and never mutates, which is what lets --check keep
// its non-mutating guarantee while boot separately runs sync_models() first
// (spec §2.1). Classification is by ERROR PROVENANCE, not severity guesswork.
#pragma once

#include "mode/mode.h"                 // TargetMode
#include "models/model_identity.h"     // ManifestView, PlatformInfo

#include <QSqlDatabase>
#include <QString>
#include <cstdint>
#include <vector>

namespace denso::health {

enum class Readiness { Ready, Degraded, Blocked };

/// Whole-machine faults: no restart fixes these, so boot exits EX_CONFIG (78).
/// ONLY kinds with a real producer are declared (the same no-speculative-enum
/// rule ZoneIssue::Kind follows). DbUnopenable/SchemaNewer/MigrationFailed are
/// produced by the boot and --check paths, which run Db::open + run_migrations
/// before calling the verdict (see startup.cpp / run_headless.cpp);
/// DbQueryFailed/ModelsDirUnreadable/ManifestCorrupt are produced here in
/// evaluate_integrity.
struct GlobalBlocker {
    enum class Kind {
        DbUnopenable, SchemaNewer, MigrationFailed, DbQueryFailed,
        ModelsDirUnreadable, ManifestCorrupt
    };
    Kind    kind;
    QString detail;
};

/// Faults scoped to one camera's zones: the app boots and healthy zones report.
/// ONLY kinds with a real producer are declared. Do NOT add speculative values
/// to "stabilise" status.json — that file uses stable STRING reason codes
/// (reason_code below), so new kinds are additive without placeholders.
struct ZoneIssue {
    /// ModelCompatibilityRejected is deliberately NOT named after the wrong-mode
    /// branch: it covers EVERY way the central policy can reject an ATTACHED
    /// model, of which wrong-mode is only one of seven. Naming the kind after a
    /// single branch would make six other failures self-describe as a mode problem
    /// and send an operator hunting for a mode switch that never happened
    /// (spec §7.3). The kind says what happened to the CAMERA; `policy_reason`
    /// carries the actual cause.
    enum class Kind { EngineMissing, EnginesUnmanifested, ModelCompatibilityRejected,
                      LevelCalibrationInvalid };
    Kind    kind;
    int64_t camera_id = 0;   // 0 = not camera-scoped (e.g. EnginesUnmanifested)
    QString detail;
    /// The policy's OWN stable reason code, verbatim (spec §4.2): one of
    /// model_undeclared / model_unknown_id / model_family_mismatch /
    /// model_shape_unsupported / model_classes_mismatch / model_provenance_failed
    /// / model_mode_incompatible. Only the genuine wrong-mode case may ever carry
    /// `model_mode_incompatible` — a hash mismatch must not describe itself as a
    /// mode problem. Empty for kinds that consult no policy.
    ///
    /// Like reason_code below, these strings are a FILE FORMAT (they reach
    /// status.json): never rename, never reuse, never renumber, only add.
    QString policy_reason;
};

struct IntegrityVerdict {
    Readiness                  status = Readiness::Ready;
    std::vector<GlobalBlocker> blockers;
    std::vector<ZoneIssue>     issues;
};

/// DB-stage readiness, evaluated WITHOUT mutating the database (opens read-only).
/// Shared by GUI boot (main.cpp) and --check (run_headless.cpp) so both classify
/// a future-schema database IDENTICALLY and return the same EX_CONFIG exit code.
/// A missing file is Ready (fresh install — nothing to migrate); an unreadable
/// file is Blocked{DbUnopenable}; a user_version greater than
/// db::supported_schema_version() is Blocked{SchemaNewer}. Run this BEFORE
/// run_migrations() on the primary DB — it is the producer of the SchemaNewer /
/// DbUnopenable global blockers.
IntegrityVerdict evaluate_db_schema(const QString& db_path);

/// Evaluate the installation. `db` must already be open and migrated.
///
/// Every ATTACHED catalog model is additionally resolved through
/// `models::resolve_model_metadata` and judged by the central
/// `models::model_compatibility` (spec §7.3). A rejected attachment becomes a
/// camera-scoped ZoneIssue{ModelCompatibilityRejected} carrying the real
/// `camera_id` and the policy's verbatim reason — classified **Degraded (exit
/// 10)**, never a GlobalBlocker: bricking a four-camera appliance because one
/// camera has a bad attachment is the opposite of the per-zone fail-closed
/// contract. An UNATTACHED artifact is never escalated into a camera issue, no
/// matter what the policy thinks of it (spec §5.1).
///
/// `mode`, `view` and `platform` have NO default — a forgotten call site must
/// fail to compile, never silently authorize. This function duplicates NO
/// compatibility rule: it asks the one central policy.
IntegrityVerdict evaluate_integrity(const QSqlDatabase& db, const QString& models_dir,
                                    denso::mode::TargetMode mode,
                                    const denso::models::ManifestView& view,
                                    const denso::models::PlatformInfo& platform);

/// Process exit code for a verdict: 0 Ready / 10 Degraded / 78 Blocked (spec §2.1).
int exit_code_for(Readiness r);

/// Stable, machine-readable reason codes for status.json. These strings are a
/// FILE FORMAT: never renumber, never reuse, only add. Deliberately not enum
/// ordinals — an ordinal shifts whenever a value is inserted, silently
/// remapping every historical status.json.
QString reason_code(GlobalBlocker::Kind k);
QString reason_code(ZoneIssue::Kind k);

} // namespace denso::health
