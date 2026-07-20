// Pure command-line parsing: argv → Command. No side effects, no I/O, no Qt
// application object — so main() can decide BEFORE constructing QApplication
// whether this run needs a GUI at all. A headless mode must never load the xcb
// platform plugin (the installer calls these with no display).
#pragma once

#include <QList>
#include <QString>
#include <QStringList>

namespace denso::cli {

enum class Mode {
    Gui,              ///< no flags — the normal application
    Version,          ///< --version
    Check,            ///< --check
    CheckRunning,     ///< --check-running
    CheckMigrations,  ///< --check-migrations <db-path>
    MigrateModel,     ///< --migrate-model --old <f> --new <f> --camera <id>...
    Error,            ///< bad usage; `error` says why
};

struct Command {
    Mode mode = Mode::Gui;
    QString arg;         ///< CheckMigrations: the db path to migrate
    QStringList engines; ///< Check: extra engine filenames to validate (--engine)
    QString error;       ///< Error: the human-readable reason
    QString old_engine;      ///< MigrateModel: --old <file>
    QString new_engine;      ///< MigrateModel: --new <file>
    QString class_map_path;  ///< MigrateModel: --class-map <path> (optional)
    QList<qint64> cameras;   ///< MigrateModel: repeated --camera <id>
};

/// `args` EXCLUDES argv[0].
Command parse(const QStringList& args);

/// True for everything except Gui — including Error, which prints usage.
bool is_headless(Mode m);

QString usage();

} // namespace denso::cli
