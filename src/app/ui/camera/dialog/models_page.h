// The wizard's "Models" step, class-centric: pick which models form the
// detection ensemble, then pick classes *once* (union of the chosen models'
// classes, by name) with one confidence each. Selections fan back out to the
// per-model schema by class name in selections(). Reads catalog + current
// attachments from detection::repo; the coordinator saves via
// detection::set_camera_models on Finish. Pure widget — owns its controls,
// emits requests, holds no business logic.
#pragma once

#include "detection/detection.h"

#include <QSqlDatabase>
#include <QString>
#include <QWidget>

#include <cstdint>
#include <map>
#include <utility>
#include <vector>

class QVBoxLayout;
class QCheckBox;
class QDoubleSpinBox;
class QLineEdit;

namespace denso::ui {

class ModelsPage : public QWidget {
    Q_OBJECT
public:
    explicit ModelsPage(QWidget* parent = nullptr);
    ~ModelsPage() override;

    void set_db(QSqlDatabase db) { db_ = std::move(db); }
    void load_for(int64_t camera_id);
    std::vector<denso::detection::CameraModel> selections(int64_t camera_id) const;

signals:
    void back_requested();
    void finish_requested();

private:
    void rebuild_class_list();  // union of the checked models' class names
    void apply_filter();        // show/hide class rows by the search text

    QSqlDatabase db_;
    QVBoxLayout* models_layout_ = nullptr;  // ensemble model checkboxes
    QLineEdit* search_ = nullptr;
    QVBoxLayout* class_layout_ = nullptr;   // class rows

    std::vector<denso::detection::DetectionModel> catalog_;  // cached for rebuilds

    struct ModelCheck {
        int64_t model_id = 0;
        QCheckBox* on = nullptr;
    };
    std::vector<ModelCheck> model_checks_;

    struct ClassRow {
        QString name;
        QCheckBox* on = nullptr;
        QDoubleSpinBox* conf = nullptr;
        QWidget* row = nullptr;
    };
    std::vector<ClassRow> class_rows_;

    // Remembered class selections (name → {selected, conf}) so toggling a model
    // in/out of the ensemble preserves what the user set. Seeded from the DB in
    // load_for, folded from the live widgets on every rebuild.
    std::map<QString, std::pair<bool, double>> selected_state_;
};

} // namespace denso::ui
