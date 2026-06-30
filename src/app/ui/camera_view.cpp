#include "ui/camera_view.h"

#include <QFont>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

namespace denso::ui {

CameraView::CameraView(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("mainContent"));  // picks up the content-panel background

    // Center the empty-state column vertically and horizontally.
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
    connect(add, &QPushButton::clicked, this, &CameraView::request_add_camera);

    col->addWidget(glyph);
    col->addWidget(title);
    col->addWidget(subtitle);
    col->addSpacing(8);

    // Keep the button at its natural width, centered.
    auto* btn_row = new QHBoxLayout;
    btn_row->addStretch(1);
    btn_row->addWidget(add, 0);
    btn_row->addStretch(1);
    col->addLayout(btn_row);

    root->addLayout(col);
    root->addStretch(1);
}

void CameraView::request_add_camera() {
    // No-op stub: the add-camera flow is wired in a later slice.
}

} // namespace denso::ui
