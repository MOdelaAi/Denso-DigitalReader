#include "ui/camera/dialog/models_page.h"

#include "detection/repo.h"
#include "models/model_identity.h"          // diagnostic_filename
#include "ui/camera/dialog/model_empty_state.h"

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

    // The empty state lives OUTSIDE models_layout_, which load_for() empties on
    // every reload: a label parented into that layout would be deleted by the
    // next rebuild and the page would silently go blank again.
    empty_state_ = new QLabel;
    empty_state_->setObjectName(QStringLiteral("modelsEmptyState"));
    empty_state_->setWordWrap(true);
    empty_state_->setTextInteractionFlags(Qt::TextSelectableByMouse);  // copy the code
    empty_state_->setProperty("warning", true);
    empty_state_->hide();
    root->addWidget(empty_state_);

    // ── Classes to detect ──
    root->addWidget(new QLabel(QStringLiteral("Classes to detect")));
    auto* filter_row = new QHBoxLayout;
    search_ = new QLineEdit;
    search_->setPlaceholderText(QStringLiteral("Filter classes…"));
    connect(search_, &QLineEdit::textChanged, this, [this] { apply_filter(); });
    filter_row->addWidget(search_, 1);
    // Select all / Clear act on the rows currently VISIBLE under the filter — so
    // you can narrow with the filter and select just that subset, or select every
    // class when the filter is empty (e.g. all 10 digits for the digit reader).
    auto* select_all = new QPushButton(QStringLiteral("Select all"));
    select_all->setProperty("flatText", true);
    connect(select_all, &QPushButton::clicked, this, [this] { set_visible_checked(true); });
    filter_row->addWidget(select_all, 0);
    auto* clear_all = new QPushButton(QStringLiteral("Clear"));
    clear_all->setProperty("flatText", true);
    connect(clear_all, &QPushButton::clicked, this, [this] { set_visible_checked(false); });
    filter_row->addWidget(clear_all, 0);
    root->addLayout(filter_row);

    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    auto* holder = new QWidget;
    class_layout_ = new QVBoxLayout(holder);
    scroll->setWidget(holder);
    root->addWidget(scroll, 1);

    // ── Footer ──
    // TWO explicit terminal choices, because ROI areas are OPTIONAL: no areas
    // means whole-frame detection, which is a legitimate configuration and must
    // be reachable by a button that says so. Previously the only way to finish
    // without areas was "Exit without saving" on the next step — a label that
    // reads like a cancel while actually committing the camera.
    auto* footer = new QHBoxLayout;
    auto* back = new QPushButton(QStringLiteral("Back"));
    auto* whole_frame = new QPushButton(QStringLiteral("Finish — use whole frame"));
    auto* next = new QPushButton(QStringLiteral("Next: Detection areas →"));
    next->setProperty("gold", true);  // the recommended path
    footer->addWidget(back);
    footer->addStretch(1);
    footer->addWidget(whole_frame);
    footer->addWidget(next);
    root->addLayout(footer);
    connect(back, &QPushButton::clicked, this, &ModelsPage::back_requested);
    connect(next, &QPushButton::clicked, this, &ModelsPage::next_requested);
    connect(whole_frame, &QPushButton::clicked, this,
            &ModelsPage::finish_whole_frame_requested);
}

ModelsPage::~ModelsPage() = default;

void ModelsPage::load_for(int64_t camera_id, denso::mode::TargetMode mode,
                          const denso::models::ManifestView& view,
                          const denso::models::PlatformInfo& platform) {
    // THE seam (spec 6.1): the offered set is the central policy's answer for the
    // committed mode. This page applies no rule of its own — it renders the rows
    // it is handed. A model the policy rejects never reaches a checkbox, so it can
    // be neither displayed nor returned by selections().
    offered_.clear();
    // ONE evaluation of the catalog, read twice: the rows the policy ALLOWS become
    // the offered set, and the rows it refused become the empty-state reasons. Two
    // separate calls could disagree; this cannot. A rejected row still never
    // reaches a checkbox, so it can be neither displayed as selectable nor
    // returned by selections().
    std::vector<RejectedModelNote> rejected;
    for (auto& e : denso::detection::evaluated_models(db_, mode, view, platform)) {
        if (e.result.allowed()) {
            offered_.push_back(std::move(e.row));
            continue;
        }
        // diagnostic_filename, never the raw column: the catalog filename is
        // operator-editable and must not carry anything credential-shaped into a
        // label (spec §12). The row id keeps an unprintable name identifiable.
        rejected.push_back(RejectedModelNote{
            QStringLiteral("%1 (catalog #%2)")
                .arg(QString::fromStdString(
                         denso::models::diagnostic_filename(e.row.filename)))
                .arg(e.row.id),
            e.result.reason_code});
    }

    // Never a blank page. When nothing is offered, say which mode this is and why
    // each catalog model was refused, in the policy's own stable reason codes.
    if (offered_.empty()) {
        empty_state_->setText(model_empty_state_text(mode, rejected));
        empty_state_->show();
    } else {
        empty_state_->clear();
        empty_state_->hide();
    }

    const auto attached = denso::detection::models_for(db_, camera_id);

    // Seed remembered selections from the DB (name → {selected, conf}), first
    // conf seen wins if two models disagree on a shared name.
    selected_state_.clear();
    class_rows_.clear();  // drop the previous camera's rows so the reseeding
                          // rebuild does not fold their stale state back in
    if (search_) search_->clear();  // reset the class filter for the new camera
    for (const auto& cm : attached) {
        const denso::detection::DetectionModel* dm = nullptr;
        for (const auto& m : offered_) {
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
    for (const auto& m : offered_) {
        auto* cb = new QCheckBox(QString::fromStdString(m.name));
        // Distinguishes the ensemble checkboxes from the class-row ones for
        // findChildren-based inspection, exactly as the top-bar buttons do.
        cb->setObjectName(QStringLiteral("modelCheck"));
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
    for (const auto& m : offered_) {
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

void ModelsPage::set_visible_checked(bool on) {
    // !isHidden() reflects the filter state set by apply_filter() directly,
    // independent of whether the page/ancestors are currently shown.
    for (const ClassRow& r : class_rows_) {
        if (!r.row->isHidden()) r.on->setChecked(on);
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
        for (const auto& m : offered_) {
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
