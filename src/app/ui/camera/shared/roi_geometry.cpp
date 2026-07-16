#include "ui/camera/shared/roi_geometry.h"

#include <algorithm>
#include <cmath>

namespace denso::ui {

namespace {

double clamp01(double v) { return std::clamp(v, 0.0, 1.0); }

double dist_sq(const QPointF& a, const QPointF& b) {
    const double dx = a.x() - b.x();
    const double dy = a.y() - b.y();
    return dx * dx + dy * dy;
}

/// Squared distance from `p` to the SEGMENT a→b (clamped to the endpoints, so a
/// point beyond an edge's end is far from it even when it sits on the edge's
/// infinite line).
double dist_sq_to_segment(const QPointF& p, const QPointF& a, const QPointF& b) {
    const double len_sq = dist_sq(a, b);
    if (len_sq <= 0.0) {
        return dist_sq(p, a);  // degenerate edge: both endpoints coincide
    }
    const double t = std::clamp(((p.x() - a.x()) * (b.x() - a.x()) +
                                 (p.y() - a.y()) * (b.y() - a.y())) /
                                    len_sq,
                                0.0, 1.0);
    return dist_sq(p, QPointF(a.x() + t * (b.x() - a.x()),
                              a.y() + t * (b.y() - a.y())));
}

}  // namespace

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

bool image_contains(const QRectF& image_rect, const QPointF& widget_pt,
                    double tolerance_px) {
    if (image_rect.width() <= 0 || image_rect.height() <= 0) {
        return false;
    }
    return image_rect.adjusted(-tolerance_px, -tolerance_px, tolerance_px,
                               tolerance_px)
        .contains(widget_pt);
}

int hit_test_vertex(const QList<QPointF>& widget_pts, const QPointF& probe,
                    double radius_px) {
    int best = -1;
    double best_d2 = radius_px * radius_px;
    for (int i = 0; i < widget_pts.size(); ++i) {
        const double d2 = dist_sq(widget_pts[i], probe);
        if (d2 <= best_d2) {
            best_d2 = d2;
            best = i;
        }
    }
    return best;
}

int nearest_edge_insert_index(const QList<QPointF>& widget_pts,
                              const QPointF& probe, double max_dist_px) {
    if (widget_pts.size() < 3) {
        return -1;
    }
    int best = -1;
    double best_d2 = max_dist_px * max_dist_px;
    for (int i = 0; i < widget_pts.size(); ++i) {
        const int next = (i + 1) % widget_pts.size();  // closing edge included
        const double d2 = dist_sq_to_segment(probe, widget_pts[i],
                                             widget_pts[next]);
        if (d2 <= best_d2) {
            best_d2 = d2;
            best = i + 1;  // insert AFTER the edge's start vertex
        }
    }
    return best;
}

} // namespace denso::ui
