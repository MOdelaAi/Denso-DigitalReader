#include "paths/paths.h"

#include <QCoreApplication>
#include <QDir>

namespace denso::paths {

QString data_dir() {
    // qEnvironmentVariable returns an empty string for BOTH unset and
    // set-but-empty; both must fall back, never yield an empty path.
    const QString env = qEnvironmentVariable("DENSO_DATA_DIR");
    if (!env.isEmpty()) {
        // cleanPath resolves ".."/"." and strips a trailing slash, so the
        // derived paths below can concatenate unconditionally.
        return QDir::cleanPath(env);
    }
    const QString dir = QCoreApplication::applicationDirPath();
    if (dir.isEmpty()) {
        // No QCoreApplication yet (or no argv[0]) — mirror db::default_path()'s
        // historical fallback rather than returning an empty path.
        return QStringLiteral(".");
    }
    return dir;
}

// QDir::filePath, NOT string concatenation: cleanPath() strips a trailing
// separator from every path EXCEPT a filesystem root, so "/" + "/denso.db" would
// yield "//denso.db" (and "C:/" → "C://denso.db").
QString db_file()              { return QDir(data_dir()).filePath(QStringLiteral("denso.db")); }
QString log_file()             { return QDir(data_dir()).filePath(QStringLiteral("denso.log")); }
QString models_dir()           { return QDir(data_dir()).filePath(QStringLiteral("models")); }
QString trt_cache_dir()        { return QDir(models_dir()).filePath(QStringLiteral("trt_cache")); }
QString lock_file()            { return QDir(data_dir()).filePath(QStringLiteral("denso.lock")); }
QString legacy_settings_json() { return QDir(data_dir()).filePath(QStringLiteral("settings.json")); }

} // namespace denso::paths
