#include "cli/args.h"

namespace denso::cli {

QString usage() {
    return QStringLiteral(
        "usage: denso [--version | --check [--engine <file>]... |\n"
        "              --check-running | --check-migrations <db-path> |\n"
        "              --migrate-model --old <file> --new <file>\n"
        "                             --camera <id>... [--class-map <path>]]\n"
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
        "  --check-migrations <db>    run the migration chain against <db> ONLY\n"
        "  --migrate-model            re-point cameras from an old engine to a new\n"
        "                             one; requires --old, --new, and at least one\n"
        "                             --camera <id> (repeatable); --class-map <path>\n"
        "                             is optional\n");
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

/// --migrate-model --old <f> --new <f> --camera <id>... [--class-map <p>]
Command parse_migrate(const QStringList& rest) {
    Command c; c.mode = Mode::MigrateModel;
    for (int i = 0; i < rest.size(); ++i) {
        const QString& a = rest.at(i);
        auto need = [&](QString& dst) -> bool {
            if (i + 1 >= rest.size()) return false;
            dst = rest.at(++i); return true;
        };
        if (a == QStringLiteral("--old"))            { if (!need(c.old_engine))     return error(QStringLiteral("--old requires a filename")); }
        else if (a == QStringLiteral("--new"))       { if (!need(c.new_engine))     return error(QStringLiteral("--new requires a filename")); }
        else if (a == QStringLiteral("--class-map")) { if (!need(c.class_map_path)) return error(QStringLiteral("--class-map requires a path")); }
        else if (a == QStringLiteral("--camera")) {
            if (i + 1 >= rest.size()) return error(QStringLiteral("--camera requires an id"));
            bool ok = false; const qint64 id = rest.at(++i).toLongLong(&ok);
            if (!ok) return error(QStringLiteral("--camera id must be an integer"));
            if (c.cameras.contains(id)) return error(QStringLiteral("duplicate --camera id: %1").arg(id));
            c.cameras << id;
        } else return error(QStringLiteral("unexpected argument to --migrate-model: %1").arg(a));
    }
    if (c.old_engine.isEmpty() || c.new_engine.isEmpty() || c.cameras.isEmpty())
        return error(QStringLiteral("--migrate-model requires --old, --new, and at least one --camera"));
    return c;
}

} // namespace

Command parse(const QStringList& args) {
    if (args.isEmpty()) return Command{Mode::Gui, {}, {}, {}};

    const QString& flag = args.first();
    const QStringList rest = args.mid(1);

    if (flag == QStringLiteral("--check")) return parse_check(rest);

    if (flag == QStringLiteral("--migrate-model")) return parse_migrate(rest);

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
