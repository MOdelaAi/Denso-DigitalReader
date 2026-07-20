#include "cli/run_headless.h"

#include "cli/migrate_coordinator.h"
#include "db/db.h"
#include "detection/engine_registry.h"   // denso::ui::BackendEngine
#include "detection/repo.h"
#include "instance/single_instance.h"
#include "paths/paths.h"

#include <QDir>
#include <QFileInfo>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QTemporaryFile>

#include <algorithm>
#include <cstdio>
#include <exception>
#include <optional>
#include <string>
#include <vector>

namespace denso::app {

namespace {

int run_version() {
    std::printf("%s\n", APP_VERSION);
    return 0;
}

int run_check_running() {
    // 0 = an instance is running, 1 = none, 4 = could not determine (the lock
    // file itself is unusable — missing dir, permissions, ...). (prerm reads
    // this to refuse an upgrade under a live app; 4 must never masquerade as
    // the clean 1 it would need to see to proceed.)
    using denso::instance::RunState;
    const RunState state =
        denso::instance::SingleInstance::running_state(denso::paths::lock_file());
    switch (state) {
        case RunState::Running:
            std::printf("running\n");
            return 0;
        case RunState::NotRunning:
            std::printf("not running\n");
            return 1;
        case RunState::Indeterminate:
            std::fprintf(stderr,
                         "check-running: cannot determine — lock file unusable at %s\n",
                         qPrintable(denso::paths::lock_file()));
            return 4;
    }
    return 4;
}

int run_check_migrations(const QString& db_path) {
    // Deliberately the NORMAL open() + run_migrations(): the caller hands us a
    // throwaway copy, so mutation is confined there and --check's
    // no-persistent-mutation contract is untouched.
    auto db = denso::db::Db::open(db_path);
    if (!db) {
        std::fprintf(stderr, "check-migrations: cannot open %s\n",
                     qPrintable(db_path));
        return 1;
    }
    if (!denso::db::run_migrations(db->handle())) {
        std::fprintf(stderr, "check-migrations: migration chain FAILED on %s\n",
                     qPrintable(db_path));
        return 1;
    }
    std::printf("check-migrations: ok (%s)\n", qPrintable(db_path));
    return 0;
}

/// Real create-and-remove probe. access(W_OK) is weaker: it does not prove a
/// create succeeds under the actual mount, ACL, quota, or read-only conditions.
bool data_dir_writable() {
    const QString dir = denso::paths::data_dir();
    if (!QFileInfo(dir).isDir()) {
        std::fprintf(stderr, "check: data dir does not exist: %s\n", qPrintable(dir));
        return false;
    }
    // QDir::filePath, not string concatenation — paths.cpp's own rule: with
    // DENSO_DATA_DIR=/, "dir + "/..."" would double the separator.
    QTemporaryFile probe(QDir(dir).filePath(QStringLiteral(".denso-check-XXXXXX")));
    if (!probe.open()) {
        std::fprintf(stderr, "check: data dir is not writable: %s\n", qPrintable(dir));
        return false;
    }
    // Do not trust QTemporaryFile's auto-remove-on-close: removal can fail
    // independently of creation (ACLs, another process holding it open) and
    // fails silently, which would leave a stray probe file behind and break
    // both this function's own create-and-remove intent and the "data dir
    // left byte-for-byte empty" contract the gate script asserts.
    probe.close();
    if (!probe.remove()) {
        std::fprintf(stderr, "check: cannot remove writability probe in %s\n", qPrintable(dir));
        return false;
    }
    return true;
}

/// The engines configured cameras actually need. A MISSING database is an empty
/// set, never an error and never a reason to create one: a fresh DB references
/// no cameras (the query joins camera_model), so a clean install legitimately
/// needs no *configured* engines at all.
///
/// Returns nullopt for "present but UNREADABLE" — a hard failure that must never
/// be confused with the empty fresh-install case. This is why it uses
/// try_attached_model_filenames: the plain attached_model_filenames returns {}
/// for a failed query too (repo.cpp:53).
std::optional<std::vector<std::string>> configured_models() {
    const QString db_path = denso::paths::db_file();
    if (!QFileInfo::exists(db_path)) {
        std::printf("check: no database yet (fresh install) — no configured engines\n");
        return std::vector<std::string>{};
    }
    auto db = denso::db::Db::open_read_only(db_path);
    if (!db) {
        std::fprintf(stderr, "check: cannot read database: %s\n", qPrintable(db_path));
        return std::nullopt;
    }
    auto models = denso::detection::try_attached_model_filenames(db->handle());
    if (!models) {
        std::fprintf(stderr, "check: database is unreadable (bad schema?): %s\n",
                     qPrintable(db_path));
        return std::nullopt;
    }
    return models;
}

/// Load one model the way the APP does — construct the backend directly. That
/// reads the file and, on Linux, writes nothing (trt_engine.cpp:87 ignores
/// cache_dir). `trtexec --loadEngine` would only prove TensorRT can read the
/// plan, not that THIS app can load and bind it.
///
/// Class names are validated via engine.class_names(), NOT by parsing a sidecar
/// here: the backends source names differently and each already enforces its own
/// rule — TrtEngine reads <stem>.names.json in its ctor (trt_engine.cpp:89) and
/// throws without it; OrtEngine reads them from the ONNX metadata (there is no
/// sidecar for a .onnx). Parsing a sidecar unconditionally would fail every
/// valid Windows model.
bool validate_model(const std::string& filename, const QString& cache_dir) {
    const QString path =
        QDir(denso::paths::models_dir()).filePath(QString::fromStdString(filename));
    if (!QFileInfo::exists(path)) {
        std::fprintf(stderr, "check: FAIL %s — file missing from %s\n", filename.c_str(),
                     qPrintable(denso::paths::models_dir()));
        return false;
    }
    try {
        // Both signals are needed — the backends fail differently.
        // TrtEngine::ok() is a hardcoded `return true` (trt_engine.h:42 — "ctor
        // either succeeds or throws"), so on the Jetson the try/catch IS the
        // gate; OrtEngine::ok() is a real check (ort_engine.h:26).
        denso::ui::BackendEngine engine(path.toStdString(), cache_dir.toStdString());
        if (!engine.ok()) {
            std::fprintf(stderr, "check: FAIL %s — did not load\n", filename.c_str());
            return false;
        }
        if (engine.class_names().empty()) {
            std::fprintf(stderr, "check: FAIL %s — no class names (missing sidecar / "
                                 "ONNX metadata)\n", filename.c_str());
            return false;
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "check: FAIL %s — %s\n", filename.c_str(), e.what());
        return false;
    }
    std::printf("check: ok   %s\n", filename.c_str());
    return true;
}

/// Wire the parsed --migrate-model command to the headless coordinator: the
/// real appliance paths come from denso::paths, everything else from the parsed
/// command. No logic here — the coordinator owns validation, the transaction,
/// and the machine-readable JSON (printed verbatim for the caller to consume).
int run_migrate_model(const denso::cli::Command& cmd) {
    denso::cli::MigrateInputs in;
    in.models_dir = denso::paths::models_dir();
    in.db_path = denso::paths::db_file();
    in.old_engine = cmd.old_engine;
    in.new_engine = cmd.new_engine;
    in.class_map_path = cmd.class_map_path;
    in.cameras = cmd.cameras;
    const denso::cli::MigrateOutcome outcome = denso::cli::run_migrate(in);
    std::printf("%s\n", outcome.json.toStdString().c_str());
    return outcome.exit_code;
}

int run_check(const QStringList& extra_engines) {
    if (!data_dir_writable()) return 1;

    const auto configured = configured_models();
    if (!configured) return 1;  // present but unreadable — NOT the same as empty

    // Validate the UNION of what the DB references and what the caller named.
    // Why the union: on a fresh install the DB is empty, so configured-only would
    // load ZERO engines and pass even with a corrupt packaged engine — yet the
    // spec makes package engines an activation blocker. Why not scan models/*:
    // an unrelated operator engine must never block an upgrade. Slice 2's
    // denso-setup passes --engine from its tracked package manifest.
    std::vector<std::string> targets = *configured;
    for (const QString& e : extra_engines) targets.push_back(e.toStdString());
    std::sort(targets.begin(), targets.end());
    targets.erase(std::unique(targets.begin(), targets.end()), targets.end());

    if (targets.empty()) {
        std::printf("check: PASS (fresh install; no engines required)\n");
        return 0;
    }

    // Throwaway cache: TrtEngine ignores cache_dir (trt_engine.cpp:87) but
    // OrtEngine uses it, so this keeps the real models/trt_cache untouched on
    // BOTH platforms.
    QTemporaryDir cache;
    if (!cache.isValid()) {
        std::fprintf(stderr, "check: cannot create a temporary cache dir\n");
        return 1;
    }

    bool ok = true;
    for (const std::string& m : targets) ok = validate_model(m, cache.path()) && ok;

    std::printf("check: %s (%zu model(s) validated)\n", ok ? "PASS" : "FAIL",
                targets.size());
    return ok ? 0 : 1;
}

} // namespace

int run_headless(const denso::cli::Command& cmd) {
    using denso::cli::Mode;
    switch (cmd.mode) {
        case Mode::Version:         return run_version();
        case Mode::CheckRunning:    return run_check_running();
        case Mode::CheckMigrations: return run_check_migrations(cmd.arg);
        case Mode::Check:           return run_check(cmd.engines);
        case Mode::MigrateModel:    return run_migrate_model(cmd);
        case Mode::Error:
            std::fprintf(stderr, "denso: %s\n\n%s", qPrintable(cmd.error),
                         qPrintable(denso::cli::usage()));
            return 2;
        case Mode::Gui:
            std::fprintf(stderr, "denso: internal error: Gui is not headless\n");
            return 2;
    }
    return 2;
}

} // namespace denso::app
