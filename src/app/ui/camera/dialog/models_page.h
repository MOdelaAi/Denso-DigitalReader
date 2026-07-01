// The wizard's "Models" step: attach 1..N detection models to a camera and, per
// attached model, pick which classes to keep and each class's confidence. Reads
// the catalog + current attachments from detection::repo; the coordinator saves
// via detection::set_camera_models on Finish. Pure widget — owns its controls,
// emits requests, holds no business logic.
#pragma once

#include "detection/detection.h"

#include <QSqlDatabase>
#include <QWidget>

#include <cstdint>
#include <utility>
#include <vector>

class QVBoxLayout;

namespace denso::ui {

class ModelsPage : public QWidget {
    Q_OBJECT
public:
    explicit ModelsPage(QWidget* parent = nullptr);
    ~ModelsPage() override;  // out-of-line: ModelRowWidgets is complete in the .cpp

    void set_db(QSqlDatabase db) { db_ = std::move(db); }
    void load_for(int64_t camera_id);
    std::vector<denso::detection::CameraModel> selections(int64_t camera_id) const;

signals:
    void back_requested();
    void finish_requested();

private:
    QSqlDatabase db_;
    QVBoxLayout* list_layout_ = nullptr;
    // One row group per catalog model; see .cpp for the per-model widget bundle.
    struct ModelRowWidgets;
    std::vector<ModelRowWidgets> rows_;
};

} // namespace denso::ui
