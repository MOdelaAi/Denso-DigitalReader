// Application entry point — a thin orchestrator, mirroring the Rust `main`:
// build the window → open the DB → migrate → import any legacy settings.json →
// reassert saved network config to the OS (non-fatal) → load settings → apply
// startup state → run. UI callback wiring lives in the window/dialog classes,
// not here.
#include "camera/repo.h"
#include "cli/args.h"
#include "cli/run_headless.h"
#include "db/db.h"
#include "instance/single_instance.h"
#include "logging/log_sink.h"
#include "network/backend.h"
#include "paths/paths.h"
#include "settings/repo.h"
#include "settings/settings.h"
#include "detection/model_sync.h"
#include "ui/startup.h"

#include <QApplication>
#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QLocale>
#include <QMessageBox>
#include <QSqlQuery>
#include <QString>
#include <QSysInfo>
#include <QTimer>
#include <QUuid>
#include <QtGlobal>

#include <opencv2/core.hpp>

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <memory>

namespace {

// The bounded rotating log sink (installed in main once the app dir is known).
denso::logging::RotatingLogSink* g_sink = nullptr;
int g_min_rank = 1;  // default: Info and above

int level_rank(QtMsgType t) {
    switch (t) {
        case QtDebugMsg: return 0;
        case QtInfoMsg: return 1;
        case QtWarningMsg: return 2;
        case QtCriticalMsg: return 3;
        case QtFatalMsg: return 4;
    }
    return 1;
}

// DENSO_LOG_LEVEL = debug|info|warning|critical|off (default info). "off"
// suppresses everything but fatal.
int level_from_env() {
    const char* s = std::getenv("DENSO_LOG_LEVEL");
    if (!s) return 1;
    const QString v = QString::fromLatin1(s).trimmed().toLower();
    if (v == QLatin1String("debug")) return 0;
    if (v == QLatin1String("info")) return 1;
    if (v == QLatin1String("warning")) return 2;
    if (v == QLatin1String("critical")) return 3;
    if (v == QLatin1String("off")) return 5;
    std::fprintf(stderr, "[log] unknown DENSO_LOG_LEVEL '%s' — using info\n", s);
    return 1;
}

// Level-filter, then route to the bounded rotating file sink (+ stderr inside
// it). Fatal always passes. Thread-safe — workers log from their own threads.
void message_handler(QtMsgType type, const QMessageLogContext& ctx,
                     const QString& msg) {
    if (type != QtFatalMsg && level_rank(type) < g_min_rank) {
        return;
    }
    const QByteArray line = qFormatLogMessage(type, ctx, msg).toUtf8();
    if (g_sink) {
        g_sink->write(line);
    } else {
        std::fprintf(stderr, "%.*s\n", static_cast<int>(line.size()), line.constData());
    }
}

} // namespace

int main(int argc, char** argv) {
    // ── Headless dispatch, BEFORE any QApplication exists. QApplication would
    // load the xcb platform plugin and fail with no display — and the installer
    // calls these modes from a root shell that has none.
    QStringList raw;
    raw.reserve(argc - 1);
    for (int i = 1; i < argc; ++i) raw << QString::fromLocal8Bit(argv[i]);
    const denso::cli::Command cmd = denso::cli::parse(raw);
    if (denso::cli::is_headless(cmd.mode)) {
        QCoreApplication app(argc, argv);  // no GUI; gives us applicationDirPath()
        return denso::app::run_headless(cmd);
    }

    QApplication app(argc, argv);

    // ── Single instance, BEFORE the DB, the log sink, or any camera. Two
    // processes would contend on one SQLite file and silently eat log data
    // (rename-based rotation does not follow another process's open fd).
    // Automatic, NOT static: it must outlive app.exec() (it does — main hasn't
    // returned) while being destroyed BEFORE the QApplication above it and
    // before the static log sink below, so releasing the lock can still log.
    denso::instance::SingleInstance instance_guard(denso::paths::lock_file());
    if (!instance_guard.acquire()) {
        // The log sink does not exist yet — stderr is all we have.
        std::fprintf(stderr, "denso: another instance is already running\n");
        QMessageBox::information(nullptr, QStringLiteral("Denso DigitalReader"),
                                 QStringLiteral("Denso DigitalReader is already running."));
        return 3;
    }

    // ── Logging: bounded rotating file sink + timestamped, level-filtered lines.
    g_min_rank = level_from_env();
    // Local time with a baked-in UTC offset (fixed-timezone industrial device;
    // Qt's %{time} has no offset specifier, so compute it once at boot).
    const int off = QDateTime::currentDateTime().offsetFromUtc();
    const QString offset = QStringLiteral("%1%2:%3")
        .arg(QLatin1Char(off < 0 ? '-' : '+'))
        .arg(std::abs(off) / 3600, 2, 10, QLatin1Char('0'))
        .arg(std::abs(off) % 3600 / 60, 2, 10, QLatin1Char('0'));
    qSetMessagePattern(QStringLiteral("%{time yyyy-MM-ddTHH:mm:ss.zzz}") + offset +
        QStringLiteral(" %{type} %{category} pid=%{pid} tid=%{threadid} %{message}"));
    static denso::logging::RotatingLogSink sink(denso::paths::log_file());
    g_sink = &sink;
    qInstallMessageHandler(message_handler);

    // Run OpenCV ops (e.g. cvtColor in the live grid's frame conversion) inline
    // rather than via its internal parallel_for_ pool. Each camera already has
    // its own capture thread, so the pool would just oversubscribe the CPU
    // (N capture threads × M pool threads) for no gain on these small per-frame
    // conversions.
    cv::setNumThreads(0);

    // Force Western Arabic digits in numeric widgets (QSpinBox/QDoubleSpinBox),
    // regardless of the OS regional format (e.g. Thai, which renders Thai numerals).
    QLocale::setDefault(QLocale(QLocale::English, QLocale::UnitedStates));

    const QString db_path = denso::paths::db_file();
    auto db = denso::db::Db::open(db_path);
    if (!db) {
        // NOT qFatal: it aborts without unwinding, so instance_guard's lock file
        // would survive with a dead pid — and an operator retrying immediately
        // after (say) a permissions error on the data dir would be told "already
        // running" instead of the real cause. Expected startup failures exit
        // cleanly; qFatal stays for genuinely unrecoverable aborts.
        qCritical().noquote() << "[fatal] cannot open database:" << db_path;
        return 1;
    }
    QSqlDatabase conn = db->handle();
    if (!denso::db::run_migrations(conn)) {
        qCritical().noquote() << "[fatal] cannot run migrations on:" << db_path;
        return 1;
    }

    // ── Session marker: one line per boot to anchor field diagnosis. No
    // credentials — camera COUNT only, never sources.
    {
        QSqlQuery ver(conn);
        int schema = -1;
        if (ver.exec(QStringLiteral("PRAGMA user_version")) && ver.next()) {
            schema = ver.value(0).toInt();
        }
#ifdef _WIN32
        const char* backend = "onnxruntime";
#else
        const char* backend = "tensorrt";
#endif
        static const char* kLevel[] = {"debug", "info", "warning", "critical", "?", "off"};
        qInfo().noquote().nospace()
            << "SESSION start version=" << QStringLiteral(APP_VERSION)
            << " session=" << QUuid::createUuid().toString(QUuid::WithoutBraces).left(8)
            << " platform=" << QSysInfo::prettyProductName()
            << " arch=" << QSysInfo::currentCpuArchitecture()
            << " schema=v" << schema << " backend=" << backend
            << " log_level=" << kLevel[g_min_rank]
            << " cameras=" << static_cast<int>(denso::camera::all(conn).size())
            << " data=" << denso::paths::data_dir();
    }

    // One-time migration of any pre-SQLite settings.json sitting beside the DB.
    const QString legacy_json = denso::paths::legacy_settings_json();
    denso::settings::import_legacy(conn, legacy_json);

    // Sync the detection-model catalog with the models/ folder beside the exe:
    // any *.onnx present becomes selectable in the camera Models step.
    denso::ui::sync_models(conn, denso::paths::models_dir());

    // The app owns network config: reassert it to the OS shortly after the UI is
    // up (singleShot(0) → first event-loop tick), not before it. A slow or stuck
    // CLI (now bounded by the backend's QProcess timeout) can no longer keep the
    // window from appearing; failures are logged, never fatal.
    QTimer::singleShot(0, [conn] {
        try {
            for (const auto& [iface, err] :
                 denso::network::reassert(conn, *denso::network::backend())) {
                qWarning().noquote()
                    << QStringLiteral("network: failed to apply %1 config: %2")
                           .arg(QString::fromStdString(iface), QString::fromStdString(err));
            }
        } catch (const std::exception& e) {
            qWarning().noquote() << "network: reassert failed:" << e.what();
        }
    });

    // ── Health heartbeat: one INFO line every 5 min so "alive & healthy" is
    // visible at a glance over a 24/7 run (288 lines/day). Richer metrics
    // (cameras up, fps, POST counts) are a follow-up once the grid exposes them.
    const qint64 boot_ms = QDateTime::currentMSecsSinceEpoch();
    auto* heartbeat = new QTimer(&app);
    QObject::connect(heartbeat, &QTimer::timeout, &app, [conn, boot_ms] {
        const qint64 uptime_s = (QDateTime::currentMSecsSinceEpoch() - boot_ms) / 1000;
        qInfo().noquote().nospace()
            << "HEARTBEAT uptime_s=" << uptime_s
            << " cameras=" << static_cast<int>(denso::camera::all(conn).size())
            << " log_degraded=" << (g_sink && g_sink->degraded() ? "true" : "false");
    });
    heartbeat->start(5 * 60 * 1000);

    auto state = std::make_shared<denso::settings::Settings>(denso::settings::load(conn));

    return denso::ui::launch(app, conn, state);
}
