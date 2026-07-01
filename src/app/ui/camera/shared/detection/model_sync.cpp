#include "ui/camera/shared/detection/model_sync.h"

#include "detection/detection.h"
#include "detection/repo.h"
#include "ui/camera/shared/detection/ort_engine.h"

#include <QDebug>
#include <QDir>
#include <QFileInfo>

namespace denso::ui {

void sync_models(const QSqlDatabase& db, const QString& models_dir) {
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
}

} // namespace denso::ui
