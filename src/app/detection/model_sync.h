// Keep the `model` catalog in sync with the models/ folder: on startup, scan
// for *.onnx, read each one's class names from its ONNX metadata, and upsert a
// catalog row (name defaults to the filename stem). So dropping a new .onnx in
// models/ makes it selectable in the UI next launch; core never touches ONNX.
#pragma once

#include <QSqlDatabase>
#include <QString>

namespace denso::ui {

void sync_models(const QSqlDatabase& db, const QString& models_dir);

} // namespace denso::ui
