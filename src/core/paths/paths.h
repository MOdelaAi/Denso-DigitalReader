// The single source of truth for every MUTABLE path the app owns: the database,
// the rotating log, the model/engine dir, the TensorRT cache, and the instance
// lock. Nothing else may compose these paths.
//
// Resolution: $DENSO_DATA_DIR when set and non-empty, else the directory holding
// the executable (the historical behavior — so Windows dev and the test suite are
// unchanged), else "." when there is no QCoreApplication yet.
//
// Why this exists: an installed build's program dir is root-owned and is
// REPLACED on upgrade, so state kept beside the executable would be unwritable
// at runtime and destroyed on every package upgrade. The launcher points
// DENSO_DATA_DIR at /opt/denso/data.
#pragma once

#include <QString>

namespace denso::paths {

/// The mutable-state root. Never has a trailing separator — except a
/// filesystem root, which is why the derived paths use QDir::filePath.
QString data_dir();

QString db_file();               ///< <data>/denso.db
QString status_file();           ///< <data>/status.json (machine-readable health)
QString log_file();              ///< <data>/denso.log (rotated siblings: .1 … .4)
QString models_dir();            ///< <data>/models
QString trt_cache_dir();         ///< <data>/models/trt_cache
QString lock_file();             ///< <data>/denso.lock (single-instance guard)
QString legacy_settings_json();  ///< <data>/settings.json (one-time pre-SQLite import)

} // namespace denso::paths
