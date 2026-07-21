// The single readiness verdict, shared by boot and --check. READ-ONLY by
// contract: it observes state and never mutates, which is what lets --check keep
// its non-mutating guarantee while boot separately runs sync_models() first
// (spec §2.1). Classification is by ERROR PROVENANCE, not severity guesswork.
#pragma once

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
    enum class Kind { EngineMissing, EnginesUnmanifested };
    Kind    kind;
    int64_t camera_id = 0;   // 0 = not camera-scoped (e.g. EnginesUnmanifested)
    QString detail;
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
IntegrityVerdict evaluate_integrity(const QSqlDatabase& db, const QString& models_dir);

/// Process exit code for a verdict: 0 Ready / 10 Degraded / 78 Blocked (spec §2.1).
int exit_code_for(Readiness r);

/// Stable, machine-readable reason codes for status.json. These strings are a
/// FILE FORMAT: never renumber, never reuse, only add. Deliberately not enum
/// ordinals — an ordinal shifts whenever a value is inserted, silently
/// remapping every historical status.json.
QString reason_code(GlobalBlocker::Kind k);
QString reason_code(ZoneIssue::Kind k);

} // namespace denso::health
