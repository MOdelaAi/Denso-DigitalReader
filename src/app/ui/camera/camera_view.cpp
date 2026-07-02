#include "ui/camera/camera_view.h"

#include "camera/repo.h"
#include "ui/camera/grid/camera_grid.h"

#include <QFont>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QStackedWidget>
#include <QVBoxLayout>

namespace denso::ui {

CameraView::CameraView(QSqlDatabase db, std::shared_ptr<EngineRegistry> engines,
                       QWidget* parent)
    : QWidget(parent), db_(std::move(db)), engines_(std::move(engines)) {
    setObjectName(QStringLiteral("mainContent"));  // content-panel background

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(24, 24, 24, 24);

    stack_ = new QStackedWidget;
    root->addWidget(stack_);

    // ── Page 0: empty state ────────────────────────────────────────────────
    auto* empty = new QWidget;
    auto* ev = new QVBoxLayout(empty);
    ev->addStretch(1);

    auto* col = new QVBoxLayout;
    col->setSpacing(12);

    auto* glyph = new QLabel(QStringLiteral("📷"));
    glyph->setProperty("faint", true);
    QFont gf = glyph->font();
    gf.setPointSize(48);
    glyph->setFont(gf);
    glyph->setAlignment(Qt::AlignCenter);

    auto* title = new QLabel(QStringLiteral("No cameras yet"));
    QFont tf = title->font();
    tf.setPointSizeF(tf.pointSizeF() + 4.0);
    tf.setBold(true);
    title->setFont(tf);
    title->setAlignment(Qt::AlignCenter);

    auto* subtitle = new QLabel(QStringLiteral("Add a camera to start reading"));
    subtitle->setProperty("faint", true);
    subtitle->setAlignment(Qt::AlignCenter);

    auto* add = new QPushButton(QStringLiteral("+ Add Camera"));
    add->setProperty("gold", true);
    connect(add, &QPushButton::clicked, this, &CameraView::add_camera_requested);

    col->addWidget(glyph);
    col->addWidget(title);
    col->addWidget(subtitle);
    col->addSpacing(8);
    auto* btn_row = new QHBoxLayout;
    btn_row->addStretch(1);
    btn_row->addWidget(add, 0);
    btn_row->addStretch(1);
    col->addLayout(btn_row);

    ev->addLayout(col);
    ev->addStretch(1);
    stack_->addWidget(empty);  // index 0

    // ── Page 1: live grid ──────────────────────────────────────────────────
    grid_ = new CameraGrid(db_, engines_);
    stack_->addWidget(grid_);  // index 1

    reload();
}

void CameraView::reload() {
    const int n = static_cast<int>(camera::all(db_).size());
    grid_->reload();  // rebuild + start streams (clears to nothing when n == 0)
    stack_->setCurrentIndex(n == 0 ? 0 : 1);
}

void CameraView::release_streams() {
    grid_->release_streams();
}

} // namespace denso::ui
