#include "ui/camera/dialog/models_page.h"

#include "detection/repo.h"

#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <map>
#include <set>
#include <utility>

namespace denso::ui {

ModelsPage::ModelsPage(QWidget* parent) : QWidget(parent) {
    auto* root = new QVBoxLayout(this);

    // ── Ensemble models ──
    root->addWidget(new QLabel(QStringLiteral("Models in ensemble")));
    auto* models_holder = new QWidget;
    models_layout_ = new QVBoxLayout(models_holder);
    models_layout_->setContentsMargins(0, 0, 0, 0);
    root->addWidget(models_holder);

    // ── Classes to detect ──
    root->addWidget(new QLabel(QStringLiteral("Classes to detect")));
    search_ = new QLineEdit;
    search_->setPlaceholderText(QStringLiteral("Filter classes…"));
    root->addWidget(search_);
    connect(search_, &QLineEdit::textChanged, this, [this] { apply_filter(); });

    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    auto* holder = new QWidget;
    class_layout_ = new QVBoxLayout(holder);
    scroll->setWidget(holder);
    root->addWidget(scroll, 1);

    // ── Footer ──
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
    catalog_ = denso::detection::list_models(db_);
    const auto attached = denso::detection::models_for(db_, camera_id);

    // Seed remembered selections from the DB (name → {selected, conf}), first
    // conf seen wins if two models disagree on a shared name.
    selected_state_.clear();
    for (const auto& cm : attached) {
        const denso::detection::DetectionModel* dm = nullptr;
        for (const auto& m : catalog_) {
            if (m.id == cm.model_id) {
                dm = &m;
                break;
            }
        }
        if (!dm) continue;
        for (const auto& s : cm.classes) {
            if (s.class_id < 0 ||
                s.class_id >= static_cast<int>(dm->class_names.size())) {
                continue;
            }
            const QString name = QString::fromStdString(dm->class_names[s.class_id]);
            if (selected_state_.find(name) == selected_state_.end()) {
                selected_state_[name] = {true, static_cast<double>(s.conf)};
            }
        }
    }

    // Build the ensemble model checkboxes (checked == currently attached).
    QLayoutItem* it = nullptr;
    while ((it = models_layout_->takeAt(0)) != nullptr) {
        delete it->widget();
        delete it;
    }
    model_checks_.clear();
    for (const auto& m : catalog_) {
        auto* cb = new QCheckBox(QString::fromStdString(m.name));
        const bool is_attached =
            std::any_of(attached.begin(), attached.end(),
                        [&](const denso::detection::CameraModel& a) {
                            return a.model_id == m.id;
                        });
        cb->setChecked(is_attached);
        connect(cb, &QCheckBox::toggled, this, [this] { rebuild_class_list(); });
        models_layout_->addWidget(cb);
        model_checks_.push_back({m.id, cb});
    }

    rebuild_class_list();
}

void ModelsPage::rebuild_class_list() {
    // Fold the current widget values back into the remembered map so a model
    // toggle keeps whatever the user already set.
    for (const ClassRow& r : class_rows_) {
        selected_state_[r.name] = {r.on->isChecked(), r.conf->value()};
    }

    // Clear existing rows.
    class_rows_.clear();
    QLayoutItem* it = nullptr;
    while ((it = class_layout_->takeAt(0)) != nullptr) {
        delete it->widget();  // null for the trailing stretch — delete(nullptr) is ok
        delete it;
    }

    // Which models are currently in the ensemble.
    std::set<int64_t> checked;
    for (const ModelCheck& mc : model_checks_) {
        if (mc.on->isChecked()) checked.insert(mc.model_id);
    }

    // Union of the checked models' class names (std::set → unique + sorted).
    std::set<QString> names;
    for (const auto& m : catalog_) {
        if (checked.find(m.id) == checked.end()) continue;
        for (const auto& n : m.class_names) {
            names.insert(QString::fromStdString(n));
        }
    }

    for (const QString& name : names) {
        auto* row = new QWidget;
        auto* h = new QHBoxLayout(row);
        h->setContentsMargins(0, 0, 0, 0);
        auto* on = new QCheckBox(name);
        auto* conf = new QDoubleSpinBox;
        conf->setRange(0.0, 1.0);
        conf->setSingleStep(0.05);
        conf->setValue(0.5);
        const auto prev = selected_state_.find(name);
        if (prev != selected_state_.end()) {
            on->setChecked(prev->second.first);
            conf->setValue(prev->second.second);
        }
        h->addWidget(on, 1);
        h->addWidget(conf);
        class_layout_->addWidget(row);
        class_rows_.push_back({name, on, conf, row});
    }
    class_layout_->addStretch(1);
    apply_filter();
}

void ModelsPage::apply_filter() {
    const QString q = search_->text().trimmed();
    for (const ClassRow& r : class_rows_) {
        const bool show = q.isEmpty() || r.name.contains(q, Qt::CaseInsensitive);
        r.row->setVisible(show);
    }
}

std::vector<denso::detection::CameraModel> ModelsPage::selections(
    int64_t camera_id) const {
    // Current checked class names → conf (one global value per name).
    std::map<QString, double> chosen;
    for (const ClassRow& r : class_rows_) {
        if (r.on->isChecked()) chosen[r.name] = r.conf->value();
    }

    // Fan out to per-model selections: each checked model contributes the
    // chosen classes it actually has, mapped to its own class_id.
    std::vector<denso::detection::CameraModel> out;
    for (const ModelCheck& mc : model_checks_) {
        if (!mc.on->isChecked()) continue;
        const denso::detection::DetectionModel* dm = nullptr;
        for (const auto& m : catalog_) {
            if (m.id == mc.model_id) {
                dm = &m;
                break;
            }
        }
        if (!dm) continue;

        denso::detection::CameraModel cm;
        cm.camera_id = camera_id;
        cm.model_id = mc.model_id;
        for (size_t k = 0; k < dm->class_names.size(); ++k) {
            const QString name = QString::fromStdString(dm->class_names[k]);
            const auto sel = chosen.find(name);
            if (sel == chosen.end()) continue;
            cm.classes.push_back(denso::detection::ModelClassSelection{
                static_cast<int>(k), static_cast<float>(sel->second)});
        }
        out.push_back(std::move(cm));
    }
    return out;
}

} // namespace denso::ui
