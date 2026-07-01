#include "ui/camera/dialog/page_util.h"

#include <QLabel>

namespace denso::ui {

QLabel* dim_label(const QString& text) {
    auto* l = new QLabel(text);
    l->setProperty("dim", true);
    return l;
}

} // namespace denso::ui
