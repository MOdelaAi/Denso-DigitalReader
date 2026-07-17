#include "cli/args.h"

namespace denso::cli {

QString usage() {
    return QStringLiteral(
        "usage: denso [--version | --check [--engine <file>]... |\n"
        "              --check-running | --check-migrations <db-path>]\n"
        "\n"
        "  (no flags)                 run the application\n"
        "  --version                  print the version and exit\n"
        "  --check                    validate the data dir + every engine the DB\n"
        "                             references, plus each --engine named here\n"
        "                             (does not mutate the primary database)\n"
        "  --engine <file>            repeatable; a models/ filename --check must\n"
        "                             validate even when no DB references it\n"
        "  --check-running            exit 0 if an instance holds the lock, 1 if not,\n"
        "                             4 if that cannot be determined (lock unusable)\n"
        "  --check-migrations <db>    run the migration chain against <db> ONLY\n");
}

bool is_headless(Mode m) { return m != Mode::Gui; }

namespace {

Command error(const QString& why) { return Command{Mode::Error, {}, {}, why}; }

/// --check [--engine <file>]...  — the only mode taking trailing options.
Command parse_check(const QStringList& rest) {
    Command c;
    c.mode = Mode::Check;
    for (int i = 0; i < rest.size(); ++i) {
        if (rest.at(i) != QStringLiteral("--engine")) {
            return error(QStringLiteral("unexpected argument after --check: %1")
                             .arg(rest.at(i)));
        }
        if (i + 1 >= rest.size()) {
            return error(QStringLiteral("--engine requires a models/ filename"));
        }
        c.engines << rest.at(++i);
    }
    return c;
}

} // namespace

Command parse(const QStringList& args) {
    if (args.isEmpty()) return Command{Mode::Gui, {}, {}, {}};

    const QString& flag = args.first();
    const QStringList rest = args.mid(1);

    if (flag == QStringLiteral("--check")) return parse_check(rest);

    if (flag == QStringLiteral("--check-migrations")) {
        if (rest.size() == 1) return Command{Mode::CheckMigrations, rest.first(), {}, {}};
        return error(QStringLiteral("--check-migrations requires exactly one "
                                    "database path"));
    }

    if (rest.isEmpty()) {
        if (flag == QStringLiteral("--version"))       return Command{Mode::Version, {}, {}, {}};
        if (flag == QStringLiteral("--check-running")) return Command{Mode::CheckRunning, {}, {}, {}};
    }

    return error(QStringLiteral("unknown or malformed option: %1").arg(flag));
}

} // namespace denso::cli
