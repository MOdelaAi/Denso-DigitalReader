#include "ui/camera/dialog/areas_page.h"

#include "ui/camera/dialog/page_util.h"
#include "ui/camera/dialog/roi_canvas.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QVBoxLayout>

#include <utility>

namespace denso::ui {

CameraAreasPage::CameraAreasPage(QWidget* parent) : QWidget(parent) {
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(12);

    auto* body = new QHBoxLayout;
    body->setSpacing(12);

    canvas_ = new RoiCanvas;
    connect(canvas_, &RoiCanvas::closed, this,
            &CameraAreasPage::commit_drawn_polygon);
    body->addWidget(canvas_, 1);

    // Side panel: named-area list + rename + delete + a how-to hint.
    auto* side = new QVBoxLayout;
    side->setSpacing(8);
    side->addWidget(dim_label(QStringLiteral("Areas")));

    list_ = new QListWidget;
    connect(list_, &QListWidget::currentRowChanged, this,
            &CameraAreasPage::select_area);
    side->addWidget(list_, 1);

    name_edit_ = new QLineEdit;
    name_edit_->setPlaceholderText(QStringLiteral("Area name"));
    connect(name_edit_, &QLineEdit::textEdited, this, [this](const QString& t) {
        const int row = list_->currentRow();
        if (row >= 0 && row < static_cast<int>(areas_.size())) {
            areas_[static_cast<size_t>(row)].name = t.toStdString();
            if (auto* it = list_->item(row)) it->setText(t);
        }
    });
    side->addWidget(name_edit_);

    auto* new_btn = new QPushButton(QStringLiteral("+ New area"));
    connect(new_btn, &QPushButton::clicked, this, [this] {
        list_->setCurrentRow(-1);
        name_edit_->clear();
        canvas_->clear();
        canvas_->setFocus();
    });
    side->addWidget(new_btn);

    auto* del_btn = new QPushButton(QStringLiteral("Delete area"));
    del_btn->setProperty("flatText", true);
    connect(del_btn, &QPushButton::clicked, this, [this] {
        const int row = list_->currentRow();
        if (row >= 0 && row < static_cast<int>(areas_.size())) {
            areas_.erase(areas_.begin() + row);
            refresh_list();
            canvas_->clear();
            name_edit_->clear();
        }
    });
    side->addWidget(del_btn);

    hint_ = dim_label(QStringLiteral(
        "Click to drop points. Click the first point (or press Enter) to close. "
        "Backspace undoes a point; Esc clears."));
    hint_->setWordWrap(true);
    side->addWidget(hint_);

    auto* side_host = new QWidget;
    side_host->setLayout(side);
    side_host->setFixedWidth(240);
    body->addWidget(side_host, 0);
    v->addLayout(body, 1);

    auto* footer = new QHBoxLayout;
    auto* back = new QPushButton(QStringLiteral("← Back"));
    connect(back, &QPushButton::clicked, this, &CameraAreasPage::back_requested);
    footer->addWidget(back, 0);
    auto* skip = new QPushButton(QStringLiteral("Skip"));
    connect(skip, &QPushButton::clicked, this, &CameraAreasPage::skip_requested);
    footer->addWidget(skip, 0);
    footer->addStretch(1);
    auto* save = new QPushButton(QStringLiteral("✓ Finish"));
    save->setProperty("gold", true);
    connect(save, &QPushButton::clicked, this,
            [this] { emit save_requested(areas_); });
    footer->addWidget(save, 0);
    v->addLayout(footer);
}

void CameraAreasPage::load(std::vector<camera::CameraArea> areas) {
    areas_ = std::move(areas);
    refresh_list();
    name_edit_->clear();
    canvas_->clear();
    canvas_->setFocus();
}

void CameraAreasPage::set_background(const QImage& oriented) {
    canvas_->set_frame(oriented);
}

void CameraAreasPage::show_save_error() {
    hint_->setText(QStringLiteral("Failed to save areas."));
}

void CameraAreasPage::refresh_list() {
    list_->clear();
    for (const camera::CameraArea& a : areas_) {
        list_->addItem(QString::fromStdString(a.name));
    }
}

void CameraAreasPage::select_area(int row) {
    if (row < 0 || row >= static_cast<int>(areas_.size())) {
        return;
    }
    const camera::CameraArea& a = areas_[static_cast<size_t>(row)];
    name_edit_->setText(QString::fromStdString(a.name));
    canvas_->set_polygon(a.points);
}

void CameraAreasPage::commit_drawn_polygon() {
    if (!canvas_->is_valid()) {
        return;
    }
    camera::CameraArea a;
    // camera_id is assigned by the repo's replace_areas (it ignores this field).
    a.name = QStringLiteral("Area %1").arg(areas_.size() + 1).toStdString();
    a.points = canvas_->polygon();
    areas_.push_back(std::move(a));
    refresh_list();
    list_->setCurrentRow(static_cast<int>(areas_.size()) - 1);  // → select_area
}

} // namespace denso::ui
