#include "ui/common/dialog_chrome.h"

#include <QDialog>
#include <QFont>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

namespace denso::ui::common {

QVBoxLayout* dialog_header(QDialog* dlg, const QString& title) {
    auto* h = new QVBoxLayout;
    h->setSpacing(10);
    auto* row = new QHBoxLayout;
    auto* label = new QLabel(title);
    QFont tf = label->font();
    tf.setBold(true);
    tf.setPointSizeF(tf.pointSizeF() + 6.0);
    label->setFont(tf);
    auto* close_glyph = new QPushButton(QStringLiteral("✕"));
    close_glyph->setProperty("flatText", true);
    close_glyph->setFixedSize(28, 28);
    QObject::connect(close_glyph, &QPushButton::clicked, dlg, &QDialog::reject);
    row->addWidget(label, 1);
    row->addWidget(close_glyph, 0);
    h->addLayout(row);
    auto* underline = new QFrame;
    underline->setObjectName(QStringLiteral("goldUnderline"));
    underline->setFixedSize(48, 3);
    h->addWidget(underline, 0, Qt::AlignLeft);
    return h;
}

} // namespace denso::ui::common
