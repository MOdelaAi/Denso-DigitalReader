#include "ui/camera/dialog/roi_canvas.h"

#include "ui/camera/shared/roi_geometry.h"

#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QPolygonF>

namespace denso::ui {

namespace {
constexpr double kCloseRadiusPx = 12.0;  // click this near vertex 0 to close
constexpr double kVertexRadiusPx = 4.0;
const QColor kEdge(250, 204, 21);        // gold, matches the app accent
const QColor kFill(250, 204, 21, 40);
const QColor kVertex(255, 255, 255);
}

RoiCanvas::RoiCanvas(QWidget* parent) : QWidget(parent) {
    setMinimumHeight(220);
    setFocusPolicy(Qt::StrongFocus);  // receive key events (Backspace/Esc/Enter)
    setCursor(Qt::CrossCursor);
}

void RoiCanvas::set_frame(const QImage& oriented) {
    frame_ = oriented;
    update();
}

void RoiCanvas::set_polygon(const std::vector<camera::Point>& pts) {
    points_ = pts;
    closed_ = points_.size() >= 3;
    emit changed();
    update();
}

void RoiCanvas::clear() {
    points_.clear();
    closed_ = false;
    emit changed();
    update();
}

QRectF RoiCanvas::image_rect() const {
    return fitted_image_rect(QSizeF(frame_.size()), QSizeF(size()));
}

void RoiCanvas::try_close() {
    if (!closed_ && points_.size() >= 3) {
        closed_ = true;
        emit closed();
        emit changed();
        update();
    }
}

void RoiCanvas::mousePressEvent(QMouseEvent* e) {
    if (e->button() != Qt::LeftButton || frame_.isNull() || closed_) {
        return;
    }
    const QRectF img = image_rect();
    // Click near the first vertex closes the polygon (needs 3+ points).
    if (points_.size() >= 3) {
        const QPointF first = to_widget(
            QPointF(points_.front().x, points_.front().y), img);
        if (QLineF(first, e->position()).length() <= kCloseRadiusPx) {
            try_close();
            return;
        }
    }
    const QPointF n = to_normalized(e->position(), img);
    points_.push_back(camera::Point{static_cast<float>(n.x()),
                                    static_cast<float>(n.y())});
    emit changed();
    update();
}

void RoiCanvas::mouseDoubleClickEvent(QMouseEvent* e) {
    if (e->button() == Qt::LeftButton) {
        try_close();  // a double-click is a convenient "done" gesture
    }
}

void RoiCanvas::keyPressEvent(QKeyEvent* e) {
    switch (e->key()) {
        case Qt::Key_Backspace:
            if (!closed_ && !points_.empty()) {
                points_.pop_back();
                emit changed();
                update();
            }
            break;
        case Qt::Key_Escape:
            clear();
            break;
        case Qt::Key_Return:
        case Qt::Key_Enter:
            try_close();
            break;
        default:
            QWidget::keyPressEvent(e);
    }
}

void RoiCanvas::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.fillRect(rect(), QColor(17, 17, 17));

    const QRectF img = image_rect();
    if (!frame_.isNull()) {
        p.drawImage(img, frame_);
    } else {
        p.setPen(QColor(148, 148, 148));
        p.drawText(rect(), Qt::AlignCenter, QStringLiteral("Capturing…"));
        return;
    }

    if (points_.empty()) {
        return;
    }

    QPolygonF poly;
    for (const camera::Point& pt : points_) {
        poly << to_widget(QPointF(pt.x, pt.y), img);
    }

    if (closed_) {
        QPainterPath path;
        path.addPolygon(poly);
        path.closeSubpath();
        p.fillPath(path, kFill);
    }

    QPen pen(kEdge, 2.0);
    p.setPen(pen);
    if (closed_) {
        p.drawPolygon(poly);
    } else {
        p.drawPolyline(poly);
    }

    // Vertices; the first is emphasized as the close target.
    for (int i = 0; i < poly.size(); ++i) {
        const bool first = (i == 0);
        p.setBrush(first ? kEdge : kVertex);
        p.setPen(QPen(QColor(17, 17, 17), 1.0));
        p.drawEllipse(poly[i], kVertexRadiusPx, kVertexRadiusPx);
    }
}

} // namespace denso::ui
