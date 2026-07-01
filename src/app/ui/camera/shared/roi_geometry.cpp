#include "ui/camera/shared/roi_geometry.h"

#include <algorithm>

namespace denso::ui {

namespace {
double clamp01(double v) { return std::clamp(v, 0.0, 1.0); }
}

QRectF fitted_image_rect(const QSizeF& image, const QSizeF& widget) {
    if (image.width() <= 0 || image.height() <= 0 || widget.width() <= 0 ||
        widget.height() <= 0) {
        return {};
    }
    const double scale = std::min(widget.width() / image.width(),
                                  widget.height() / image.height());
    const double w = image.width() * scale;
    const double h = image.height() * scale;
    const double left = (widget.width() - w) / 2.0;
    const double top = (widget.height() - h) / 2.0;
    return QRectF(left, top, w, h);
}

QPointF to_normalized(const QPointF& widget_pt, const QRectF& image_rect) {
    if (image_rect.width() <= 0 || image_rect.height() <= 0) {
        return {};
    }
    const double nx = (widget_pt.x() - image_rect.left()) / image_rect.width();
    const double ny = (widget_pt.y() - image_rect.top()) / image_rect.height();
    return QPointF(clamp01(nx), clamp01(ny));
}

QPointF to_widget(const QPointF& norm, const QRectF& image_rect) {
    return QPointF(image_rect.left() + norm.x() * image_rect.width(),
                   image_rect.top() + norm.y() * image_rect.height());
}

} // namespace denso::ui
