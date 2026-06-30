#include "ui/camera_dialog.h"

#include <QFont>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

namespace denso::ui {

CameraDialog::CameraDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle(QStringLiteral("Camera"));
    setObjectName(QStringLiteral("dialogPanel"));
    resize(900, 560);

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(24, 24, 24, 24);
    outer->setSpacing(22);

    // ── Header ──
    auto* header = new QVBoxLayout;
    header->setSpacing(10);
    auto* title_row = new QHBoxLayout;
    auto* title = new QLabel(QStringLiteral("Camera"));
    QFont tf = title->font();
    tf.setBold(true);
    tf.setPointSizeF(tf.pointSizeF() + 6.0);
    title->setFont(tf);
    auto* close_glyph = new QPushButton(QStringLiteral("✕"));
    close_glyph->setProperty("flatText", true);
    close_glyph->setFixedSize(28, 28);
    connect(close_glyph, &QPushButton::clicked, this, &QDialog::reject);
    title_row->addWidget(title, 1);
    title_row->addWidget(close_glyph, 0);
    header->addLayout(title_row);
    auto* underline = new QFrame;
    underline->setObjectName(QStringLiteral("goldUnderline"));
    underline->setFixedSize(48, 3);
    header->addWidget(underline, 0, Qt::AlignLeft);
    outer->addLayout(header);

    // ── Preview placeholder ──
    auto* preview = new QFrame;
    preview->setObjectName(QStringLiteral("mainContent"));
    preview->setMinimumHeight(240);
    auto* pv = new QVBoxLayout(preview);
    pv->addStretch(1);
    auto* line1 = new QLabel(QStringLiteral("Camera preview"));
    line1->setProperty("dim", true);
    line1->setAlignment(Qt::AlignCenter);
    auto* line2 = new QLabel(QStringLiteral("Coming soon"));
    line2->setProperty("faint", true);
    line2->setAlignment(Qt::AlignCenter);
    pv->addWidget(line1);
    pv->addWidget(line2);
    pv->addStretch(1);
    outer->addWidget(preview, 1);

    // ── Footer ──
    auto* footer = new QHBoxLayout;
    footer->setSpacing(8);
    footer->addStretch(1);
    auto* close_btn = new QPushButton(QStringLiteral("Close"));
    connect(close_btn, &QPushButton::clicked, this, &QDialog::reject);
    auto* add = new QPushButton(QStringLiteral("Add Camera"));
    add->setProperty("gold", true);
    // add-camera is a no-op placeholder, mirroring the Slint TODO.
    footer->addWidget(close_btn, 0);
    footer->addWidget(add, 0);
    outer->addLayout(footer);
}

} // namespace denso::ui
