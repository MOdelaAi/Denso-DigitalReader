#include "ui/camera/dialog/list_page.h"

#include "camera/repo.h"
#include "ui/camera/dialog/page_util.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

#include <cstdint>
#include <vector>

namespace denso::ui {

CameraListPage::CameraListPage(QSqlDatabase db, QWidget* parent)
    : QWidget(parent), db_(std::move(db)) {
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(10);

    empty_label_ = dim_label(QStringLiteral("No cameras — add one to get started."));
    empty_label_->setAlignment(Qt::AlignCenter);
    v->addWidget(empty_label_);

    auto* rows_host = new QWidget;
    rows_box_ = new QVBoxLayout(rows_host);
    rows_box_->setContentsMargins(0, 0, 0, 0);
    rows_box_->setSpacing(8);
    v->addWidget(rows_host);
    v->addStretch(1);

    auto* footer = new QHBoxLayout;
    footer->addStretch(1);
    auto* add_btn = new QPushButton(QStringLiteral("+ Add Camera"));
    add_btn->setProperty("gold", true);
    connect(add_btn, &QPushButton::clicked, this, &CameraListPage::add_requested);
    footer->addWidget(add_btn, 0);
    v->addLayout(footer);
}

void CameraListPage::reload() {
    while (QLayoutItem* item = rows_box_->takeAt(0)) {
        if (QWidget* w = item->widget()) w->deleteLater();
        delete item;
    }

    const std::vector<camera::Camera> cams = camera::all(db_);
    empty_label_->setVisible(cams.empty());

    for (const camera::Camera& cam : cams) {
        auto* row = new QWidget;
        row->setObjectName(QStringLiteral("card"));
        auto* rl = new QHBoxLayout(row);
        rl->setContentsMargins(12, 8, 12, 8);
        rl->setSpacing(8);

        rl->addWidget(new QLabel(QString::fromStdString(cam.name)), 1);

        auto* badge = dim_label(cam.camera_type == "ip" ? QStringLiteral("IP")
                                                        : QStringLiteral("USB"));
        rl->addWidget(badge, 0);

        const camera::Camera row_cam = cam;  // capture by value for the lambdas

        auto* cfg = new QPushButton(QStringLiteral("Configure"));
        cfg->setProperty("flatText", true);
        connect(cfg, &QPushButton::clicked, this,
                [this, row_cam] { emit configure_requested(row_cam); });
        rl->addWidget(cfg, 0);

        auto* areas = new QPushButton(QStringLiteral("Areas"));
        areas->setProperty("flatText", true);
        connect(areas, &QPushButton::clicked, this,
                [this, row_cam] { emit areas_requested(row_cam); });
        rl->addWidget(areas, 0);

        auto* del = new QPushButton(QStringLiteral("Delete"));
        del->setProperty("flatText", true);
        const int64_t id = cam.id;
        connect(del, &QPushButton::clicked, this, [this, id] {
            camera::remove(db_, id);
            emit changed();
            reload();
        });
        rl->addWidget(del, 0);

        rows_box_->addWidget(row);
    }
}

} // namespace denso::ui
