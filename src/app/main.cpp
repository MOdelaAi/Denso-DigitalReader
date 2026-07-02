// Application entry point — a thin orchestrator, mirroring the Rust `main`:
// build the window → open the DB → migrate → import any legacy settings.json →
// reassert saved network config to the OS (non-fatal) → load settings → apply
// startup state → run. UI callback wiring lives in the window/dialog classes,
// not here.
#include "db/db.h"
#include "network/backend.h"
#include "settings/repo.h"
#include "settings/settings.h"
#include "ui/camera/shared/detection/model_sync.h"
#include "ui/startup.h"

#include <QApplication>
#include <QDebug>
#include <QFile>
#include <QFileInfo>
#include <QLocale>
#include <QMutex>
#include <QString>
#include <QtGlobal>

#include <opencv2/core.hpp>

#include <cstdio>
#include <memory>

namespace {

// Route Qt log messages to `denso.log` next to the executable (and stderr).
// The app is a GUI-subsystem binary on Windows, so qWarning() is otherwise
// invisible; a file sink also helps field debugging on the Pi/Jetson. Thread-
// safe — capture workers log from their own threads.
void file_message_handler(QtMsgType type, const QMessageLogContext& ctx,
                          const QString& msg) {
    static QMutex mutex;
    static QFile file(QCoreApplication::applicationDirPath() +
                      QStringLiteral("/denso.log"));
    static const bool opened =
        file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text);

    const QString line = qFormatLogMessage(type, ctx, msg);
    QMutexLocker lock(&mutex);
    if (opened) {
        file.write(line.toUtf8());
        file.write("\n");
        file.flush();
    }
    std::fprintf(stderr, "%s\n", line.toLocal8Bit().constData());
}

} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    qInstallMessageHandler(file_message_handler);

    // Run OpenCV ops (e.g. cvtColor in the live grid's frame conversion) inline
    // rather than via its internal parallel_for_ pool. Each camera already has
    // its own capture thread, so the pool would just oversubscribe the CPU
    // (N capture threads × M pool threads) for no gain on these small per-frame
    // conversions.
    cv::setNumThreads(0);

    // Force Western Arabic digits in numeric widgets (QSpinBox/QDoubleSpinBox),
    // regardless of the OS regional format (e.g. Thai, which renders Thai numerals).
    QLocale::setDefault(QLocale(QLocale::English, QLocale::UnitedStates));

    const QString db_path = denso::db::default_path();
    auto db = denso::db::Db::open(db_path);
    if (!db) {
        qFatal("open database");
    }
    QSqlDatabase conn = db->handle();
    if (!denso::db::run_migrations(conn)) {
        qFatal("run migrations");
    }

    // One-time migration of any pre-SQLite settings.json sitting beside the DB.
    const QString legacy_json = QFileInfo(db_path).absolutePath() + QStringLiteral("/settings.json");
    denso::settings::import_legacy(conn, legacy_json);

    // Sync the detection-model catalog with the models/ folder beside the exe:
    // any *.onnx present becomes selectable in the camera Models step.
    denso::ui::sync_models(conn, QCoreApplication::applicationDirPath() + QStringLiteral("/models"));

    // The app owns network config: reassert it to the OS at boot. Non-fatal —
    // a failed apply is logged, never blocks startup.
    for (const auto& [iface, err] : denso::network::reassert(conn, *denso::network::backend())) {
        qWarning().noquote() << QStringLiteral("network: failed to apply %1 config: %2")
                                    .arg(QString::fromStdString(iface), QString::fromStdString(err));
    }

    auto state = std::make_shared<denso::settings::Settings>(denso::settings::load(conn));

    return denso::ui::launch(app, conn, state);
}
