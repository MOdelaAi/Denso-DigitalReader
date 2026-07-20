#pragma once
#include <QList>
#include <QString>
namespace denso::cli {
struct MigrateInputs {
    QString models_dir;       // dir holding manifest.json + engines/sidecars
    QString db_path;          // sqlite file (Db::open; coordinator migrates it)
    QString old_engine;       // currently-attached engine filename (basename)
    QString new_engine;       // target engine filename (must be a manifest generation)
    QString class_map_path;   // optional; empty = none
    QList<qint64> cameras;    // camera ids to swap
};
struct MigrateOutcome {
    int exit_code = 0;
    QString json;    // machine-readable, ALWAYS set (success and failure)
    QString error;   // human-readable; empty on success
};
// Full headless flow, no reliance on denso::paths (tests inject temp dirs).
MigrateOutcome run_migrate(const MigrateInputs& in);
}
