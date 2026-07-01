#include "ui/camera/dialog/models_page.h"

#include "detection/repo.h"

#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>
#include <QWidget>

#include <utility>

namespace denso::ui {

// Per catalog model: an "attach" checkbox + one (select checkbox, conf spin)
// pair per class. selections() reads these back into CameraModel structs.
struct ModelsPage::ModelRowWidgets {
    int64_t model_id = 0;
    QCheckBox* attach = nullptr;
    std::vector<QCheckBox*> class_on;
    std::vector<QDoubleSpinBox*> class_conf;
};

ModelsPage::ModelsPage(QWidget* parent) : QWidget(parent) {
    auto* root = new QVBoxLayout(this);
    root->addWidget(new QLabel(QStringLiteral("Detection models")));

    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    auto* holder = new QWidget;
    list_layout_ = new QVBoxLayout(holder);
    scroll->setWidget(holder);
    root->addWidget(scroll, 1);

    auto* footer = new QHBoxLayout;
    auto* back = new QPushButton(QStringLiteral("Back"));
    // Models is a middle step (Areas follows); the primary button advances.
    auto* finish = new QPushButton(QStringLiteral("Next"));
    footer->addWidget(back);
    footer->addStretch(1);
    footer->addWidget(finish);
    root->addLayout(footer);
    connect(back, &QPushButton::clicked, this, &ModelsPage::back_requested);
    connect(finish, &QPushButton::clicked, this, &ModelsPage::finish_requested);
}

ModelsPage::~ModelsPage() = default;

void ModelsPage::load_for(int64_t camera_id) {
    // Clear previous rows.
    rows_.clear();
    QLayoutItem* item = nullptr;
    while ((item = list_layout_->takeAt(0)) != nullptr) {
        delete item->widget();
        delete item;
    }

    const auto catalog = denso::detection::list_models(db_);
    const auto attached = denso::detection::models_for(db_, camera_id);

    for (const auto& model : catalog) {
        ModelRowWidgets w;
        w.model_id = model.id;
        auto* box = new QGroupBox(QString::fromStdString(model.name));
        auto* v = new QVBoxLayout(box);
        w.attach = new QCheckBox(QStringLiteral("Run this model"));
        v->addWidget(w.attach);

        // Find this model's current attachment (if any) for pre-fill.
        const denso::detection::CameraModel* cur = nullptr;
        for (const auto& a : attached) {
            if (a.model_id == model.id) {
                cur = &a;
                break;
            }
        }
        w.attach->setChecked(cur != nullptr);

        for (size_t k = 0; k < model.class_names.size(); ++k) {
            auto* row = new QHBoxLayout;
            auto* on = new QCheckBox(QString::fromStdString(model.class_names[k]));
            auto* conf = new QDoubleSpinBox;
            conf->setRange(0.0, 1.0);
            conf->setSingleStep(0.05);
            conf->setValue(0.5);
            // Pre-fill from current selection.
            if (cur) {
                for (const auto& s : cur->classes) {
                    if (s.class_id == static_cast<int>(k)) {
                        on->setChecked(true);
                        conf->setValue(s.conf);
                    }
                }
            }
            row->addWidget(on, 1);
            row->addWidget(conf);
            v->addLayout(row);
            w.class_on.push_back(on);
            w.class_conf.push_back(conf);
        }
        list_layout_->addWidget(box);
        rows_.push_back(std::move(w));
    }
    list_layout_->addStretch(1);
}

std::vector<denso::detection::CameraModel> ModelsPage::selections(
    int64_t camera_id) const {
    std::vector<denso::detection::CameraModel> out;
    for (const ModelRowWidgets& w : rows_) {
        if (!w.attach->isChecked()) continue;
        denso::detection::CameraModel cm;
        cm.camera_id = camera_id;
        cm.model_id = w.model_id;
        for (size_t k = 0; k < w.class_on.size(); ++k) {
            if (w.class_on[k]->isChecked()) {
                cm.classes.push_back(denso::detection::ModelClassSelection{
                    static_cast<int>(k),
                    static_cast<float>(w.class_conf[k]->value())});
            }
        }
        out.push_back(std::move(cm));
    }
    return out;
}

} // namespace denso::ui
