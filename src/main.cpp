// Application entry point — a thin orchestrator, mirroring the Rust `main`:
// build the window → open the DB → migrate → import any legacy settings.json →
// reassert saved network config to the OS (non-fatal) → load settings → apply
// startup state → run. UI callback wiring lives in the window/dialog classes,
// not here.
#include "db/db.h"
#include "network/backend.h"
#include "settings/repo.h"
#include "settings/settings.h"
#include "ui/mainwindow.h"

#include <QApplication>
#include <QDebug>
#include <QFileInfo>
#include <QString>
#include <QtGlobal>

#include <memory>

int main(int argc, char** argv) {
    QApplication app(argc, argv);

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

    // The app owns network config: reassert it to the OS at boot. Non-fatal —
    // a failed apply is logged, never blocks startup.
    for (const auto& [iface, err] : denso::network::reassert(conn, *denso::network::backend())) {
        qWarning().noquote() << QStringLiteral("network: failed to apply %1 config: %2")
                                    .arg(QString::fromStdString(iface), QString::fromStdString(err));
    }

    auto state = std::make_shared<denso::settings::Settings>(denso::settings::load(conn));

    denso::ui::MainWindow window(conn, state);
    window.apply_startup();
    window.show();

    return app.exec();
}
