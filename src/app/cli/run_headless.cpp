#include "cli/run_headless.h"

#include "db/db.h"
#include "instance/single_instance.h"
#include "paths/paths.h"

#include <QString>

#include <cstdio>

namespace denso::app {

namespace {

int run_version() {
    std::printf("%s\n", APP_VERSION);
    return 0;
}

int run_check_running() {
    // 0 = an instance is running, 1 = none. (prerm reads this to refuse an
    // upgrade under a live app.)
    const bool running =
        denso::instance::SingleInstance::is_running(denso::paths::lock_file());
    std::printf("%s\n", running ? "running" : "not running");
    return running ? 0 : 1;
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

// Placeholder — Task 6 replaces this with real engine validation.
int run_check() {
    std::fprintf(stderr, "check: not implemented\n");
    return 1;
}

} // namespace

int run_headless(const denso::cli::Command& cmd) {
    using denso::cli::Mode;
    switch (cmd.mode) {
        case Mode::Version:         return run_version();
        case Mode::CheckRunning:    return run_check_running();
        case Mode::CheckMigrations: return run_check_migrations(cmd.arg);
        case Mode::Check:           return run_check();   // Task 6
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
