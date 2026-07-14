#include "ui/common/form_widgets.h"

#include <QFont>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QStyle>
#include <QWidget>

namespace denso::ui::common {

QLabel* eyebrow(const QString& text) {
    auto* l = new QLabel(text);
    l->setProperty("faint", true);
    QFont f = l->font();
    f.setBold(true);
    f.setPointSizeF(f.pointSizeF() - 1.0);
    f.setLetterSpacing(QFont::AbsoluteSpacing, 1.5);
    l->setFont(f);
    return l;
}

QLabel* dim_label(const QString& text) {
    auto* l = new QLabel(text);
    l->setProperty("dim", true);
    return l;
}

QWidget* spec_row(const QString& label, QLabel** value_out) {
    auto* w = new QWidget;
    auto* row = new QHBoxLayout(w);
    row->setContentsMargins(0, 0, 0, 0);
    row->addWidget(dim_label(label), 1);
    auto* v = new QLabel;
    row->addWidget(v, 0);
    *value_out = v;
    return w;
}

QFrame* hline() {
    auto* line = new QFrame;
    line->setFrameShape(QFrame::HLine);
    line->setFixedHeight(1);
    return line;
}

void mark_invalid(QWidget* field, bool invalid) {
    if (!field) {
        return;
    }
    field->setProperty("invalid", invalid);
    // A dynamic-property change doesn't restyle a widget on its own — re-polish.
    if (QStyle* s = field->style()) {
        s->unpolish(field);
        s->polish(field);
    }
    field->update();
}

} // namespace denso::ui::common
