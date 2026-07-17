#include "ui/camera/dialog/list_page.h"

#include "camera/repo.h"
#include "ui/camera/dialog/page_util.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QStringList>
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
        const QString cam_name = QString::fromStdString(cam.name);
        connect(del, &QPushButton::clicked, this, [this, id, cam_name] {
            // Confirm with the CONSEQUENCE, not a generic "are you sure": this
            // destroys the hand-drawn ROI polygons and silently stops whatever
            // zones they reported. Matches how the Areas page confirms a delete.
            const std::vector<camera::CameraArea> areas = camera::areas_for(db_, id);
            QString msg = QStringLiteral("Delete camera \"%1\"?").arg(cam_name);
            if (!areas.empty()) {
                QStringList zones;
                for (const camera::CameraArea& a : areas) {
                    if (a.zone) zones << QString::number(*a.zone);
                }
                msg += QStringLiteral("\n\nIts %1 detection area(s) are deleted too.")
                           .arg(areas.size());
                if (!zones.isEmpty()) {
                    msg += QStringLiteral(" Zone %1 will stop being reported.")
                               .arg(zones.join(QStringLiteral(", ")));
                }
            }
            if (QMessageBox::question(this, QStringLiteral("Delete camera"), msg,
                                      QMessageBox::Yes | QMessageBox::No,
                                      QMessageBox::No) != QMessageBox::Yes) {
                return;
            }
            // Honour the result: reporting success on a failed delete leaves the
            // camera on screen and the operator believing it is gone.
            if (!camera::remove(db_, id)) {
                QMessageBox::warning(
                    this, QStringLiteral("Delete failed"),
                    QStringLiteral("Could not delete \"%1\". It is unchanged.")
                        .arg(cam_name));
                return;
            }
            emit changed();
            reload();
        });
        rl->addWidget(del, 0);

        rows_box_->addWidget(row);
    }
}

} // namespace denso::ui
