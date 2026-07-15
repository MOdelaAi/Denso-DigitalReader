#include "detection/model_sync.h"

#include "detection/detection.h"
#include "detection/repo.h"
#ifdef _WIN32
#include "detection/ort_engine.h"
#else
#include "detection/class_names_sidecar.h"
#include <filesystem>
#include <stdexcept>
#endif

#include <QDebug>
#include <QDir>
#include <QFileInfo>

namespace denso::ui {

void sync_models(const QSqlDatabase& db, const QString& models_dir) {
#ifdef _WIN32
    // Windows/ORT: catalog every *.onnx, sourcing class names from the model's
    // embedded metadata.
    QDir dir(models_dir);
    const QStringList files = dir.entryList({QStringLiteral("*.onnx")}, QDir::Files);
    for (const QString& f : files) {
        const QString path = dir.absoluteFilePath(f);
        detection::DetectionModel m;
        m.filename = f.toStdString();
        m.name = QFileInfo(f).completeBaseName().toStdString();
        m.class_names = OrtEngine::read_names(path.toStdString());
        if (m.class_names.empty()) {
            qWarning().noquote() << "[model_sync] no class names in" << f
                                 << "- skipping";
            continue;
        }
        if (!detection::upsert_model(db, m)) {
            qWarning().noquote() << "[model_sync] upsert failed for" << f;
        }
    }
#else
    // Linux/Jetson: catalog every prebuilt *.engine, sourcing class names from
    // its <stem>.names.json sidecar. A model without a valid sidecar is skipped
    // (it simply won't appear in the catalog); the authoritative fail-loud check
    // is at engine load during warm-up.
    QDir dir(models_dir);
    const QStringList files = dir.entryList({QStringLiteral("*.engine")}, QDir::Files);
    for (const QString& f : files) {
        const QString path = dir.absoluteFilePath(f);
        detection::DetectionModel m;
        m.filename = f.toStdString();
        m.name = QFileInfo(f).completeBaseName().toStdString();
        try {
            const auto names =
                read_names_sidecar(std::filesystem::path(path.toStdString()));
            if (!names) {
                qWarning().noquote()
                    << "[model_sync] no .names.json sidecar for" << f << "- skipping";
                continue;
            }
            m.class_names = *names;
        } catch (const std::exception& e) {
            qWarning().noquote()
                << "[model_sync] bad sidecar for" << f << ":" << e.what() << "- skipping";
            continue;
        }
        if (!detection::upsert_model(db, m)) {
            qWarning().noquote() << "[model_sync] upsert failed for" << f;
        }
    }
#endif
}

} // namespace denso::ui
