#include "ui/camera_view.h"

#include "camera/repo.h"

#include <QFont>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

namespace denso::ui {

CameraView::CameraView(QSqlDatabase db, QWidget* parent)
    : QWidget(parent), db_(std::move(db)) {
    setObjectName(QStringLiteral("mainContent"));  // picks up the content-panel background

    // Center the column vertically and horizontally.
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(24, 24, 24, 24);
    root->addStretch(1);

    auto* col = new QVBoxLayout;
    col->setSpacing(12);

    auto* glyph = new QLabel(QStringLiteral("📷"));
    glyph->setProperty("faint", true);
    QFont gf = glyph->font();
    gf.setPointSize(48);
    glyph->setFont(gf);
    glyph->setAlignment(Qt::AlignCenter);

    title_ = new QLabel;
    QFont tf = title_->font();
    tf.setPointSizeF(tf.pointSizeF() + 4.0);
    tf.setBold(true);
    title_->setFont(tf);
    title_->setAlignment(Qt::AlignCenter);

    subtitle_ = new QLabel;
    subtitle_->setProperty("faint", true);
    subtitle_->setAlignment(Qt::AlignCenter);

    auto* add = new QPushButton(QStringLiteral("+ Add Camera"));
    add->setProperty("gold", true);
    connect(add, &QPushButton::clicked, this, &CameraView::add_camera_requested);

    col->addWidget(glyph);
    col->addWidget(title_);
    col->addWidget(subtitle_);
    col->addSpacing(8);

    auto* btn_row = new QHBoxLayout;
    btn_row->addStretch(1);
    btn_row->addWidget(add, 0);
    btn_row->addStretch(1);
    col->addLayout(btn_row);

    root->addLayout(col);
    root->addStretch(1);

    reload();
}

void CameraView::reload() {
    const int n = static_cast<int>(camera::all(db_).size());
    if (n == 0) {
        title_->setText(QStringLiteral("No cameras yet"));
        subtitle_->setText(QStringLiteral("Add a camera to start reading"));
    } else {
        title_->setText(QStringLiteral("%1 camera%2 configured")
                            .arg(n)
                            .arg(n == 1 ? QString() : QStringLiteral("s")));
        subtitle_->setText(QStringLiteral("Live preview coming soon"));
    }
}

} // namespace denso::ui
