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
// Hit targets are deliberately generous: the panel is used with gloves on a
// remote desktop, and an over-tight target here means a mis-drop the operator
// then can't see. They cost nothing on a mouse.
constexpr double kCloseRadiusPx = 16.0;   // click this near vertex 0 to close
constexpr double kGrabRadiusPx = 14.0;    // click this near a vertex to drag it
constexpr double kEdgeHitPx = 8.0;        // click this near an edge to split it
constexpr double kEdgeTolerancePx = 2.0;  // slop for a click on the frame border
constexpr double kVertexRadiusPx = 5.0;
constexpr double kVertexHoverRadiusPx = 8.0;
constexpr int kMinVertices = 3;

const QColor kEdge(250, 204, 21);  // gold, matches the app accent
const QColor kFill(250, 204, 21, 40);
const QColor kVertex(255, 255, 255);
const QColor kContextEdge(148, 163, 184, 170);  // slate — present but recessive
const QColor kContextFill(148, 163, 184, 30);
const QColor kLabelBg(17, 17, 17, 190);
}  // namespace

RoiCanvas::RoiCanvas(QWidget* parent) : QWidget(parent) {
    setMinimumHeight(220);
    setFocusPolicy(Qt::StrongFocus);  // receive key events (Backspace/Esc/Enter)
    setMouseTracking(true);           // hover feedback on vertices
    setCursor(Qt::CrossCursor);
}

void RoiCanvas::set_frame(const QImage& oriented) {
    frame_ = oriented;
    update();
}

void RoiCanvas::set_context_areas(const std::vector<camera::CameraArea>& others) {
    context_ = others;
    update();
}

void RoiCanvas::begin_draw() {
    points_.clear();
    closed_ = false;
    mode_ = Mode::Drawing;
    drag_vertex_ = -1;
    hover_vertex_ = -1;
    emit changed();
    update();
}

void RoiCanvas::edit_polygon(const std::vector<camera::Point>& pts) {
    points_ = pts;
    closed_ = points_.size() >= kMinVertices;
    mode_ = Mode::Editing;
    drag_vertex_ = -1;
    hover_vertex_ = -1;
    emit changed();
    update();
}

void RoiCanvas::go_idle() {
    points_.clear();
    closed_ = false;
    mode_ = Mode::Idle;
    drag_vertex_ = -1;
    hover_vertex_ = -1;
    emit changed();
    update();
}

void RoiCanvas::clear() {
    points_.clear();
    closed_ = false;
    drag_vertex_ = -1;
    hover_vertex_ = -1;
    emit changed();
    update();
}

void RoiCanvas::undo_point() {
    if (mode_ == Mode::Drawing && !points_.empty()) {
        points_.pop_back();
        emit changed();
        update();
    }
}

void RoiCanvas::close_polygon() { try_close(); }

QRectF RoiCanvas::image_rect() const {
    return fitted_image_rect(QSizeF(frame_.size()), QSizeF(size()));
}

QList<QPointF> RoiCanvas::widget_points() const {
    const QRectF img = image_rect();
    QList<QPointF> out;
    out.reserve(static_cast<int>(points_.size()));
    for (const camera::Point& pt : points_) {
        out.push_back(to_widget(QPointF(pt.x, pt.y), img));
    }
    return out;
}

void RoiCanvas::try_close() {
    if (mode_ == Mode::Drawing && !closed_ && points_.size() >= kMinVertices) {
        closed_ = true;
        emit closed();
        emit changed();
        update();
    }
}

void RoiCanvas::mousePressEvent(QMouseEvent* e) {
    if (frame_.isNull() || mode_ == Mode::Idle) {
        return;
    }
    const QRectF img = image_rect();
    const QList<QPointF> wpts = widget_points();

    // Right-click removes a vertex while editing — but never below a triangle.
    if (e->button() == Qt::RightButton && mode_ == Mode::Editing) {
        const int v = hit_test_vertex(wpts, e->position(), kGrabRadiusPx);
        if (v < 0) {
            return;
        }
        if (points_.size() <= kMinVertices) {
            emit rejected(QStringLiteral(
                "An area needs at least 3 corners — delete the whole area "
                "instead."));
            return;
        }
        points_.erase(points_.begin() + v);
        emit polygon_edited(points_);
        emit changed();
        update();
        return;
    }

    if (e->button() != Qt::LeftButton) {
        return;
    }

    if (mode_ == Mode::Editing) {
        const int v = hit_test_vertex(wpts, e->position(), kGrabRadiusPx);
        if (v >= 0) {
            drag_vertex_ = v;  // grab it; mouseMove does the work
            return;
        }
        const int insert_at =
            nearest_edge_insert_index(wpts, e->position(), kEdgeHitPx);
        if (insert_at >= 0 &&
            image_contains(img, e->position(), kEdgeTolerancePx)) {
            const QPointF n = to_normalized(e->position(), img);
            points_.insert(points_.begin() + insert_at,
                           camera::Point{static_cast<float>(n.x()),
                                         static_cast<float>(n.y())});
            drag_vertex_ = insert_at;  // let the operator place it in one gesture
            emit polygon_edited(points_);
            emit changed();
            update();
        }
        return;
    }

    // Mode::Drawing — click near the first vertex closes (needs 3+ points).
    if (closed_) {
        return;
    }
    if (points_.size() >= kMinVertices) {
        const QPointF first = to_widget(
            QPointF(points_.front().x, points_.front().y), img);
        if (QLineF(first, e->position()).length() <= kCloseRadiusPx) {
            try_close();
            return;
        }
    }
    // Reject rather than clamp: a click in the letterbox bars is not a vertex
    // at the frame's edge, it's a miss — and a silently-pinned vertex is worse
    // than no vertex, because nothing tells the operator it happened.
    if (!image_contains(img, e->position(), kEdgeTolerancePx)) {
        emit rejected(
            QStringLiteral("Tap on the camera image — that spot is outside it."));
        return;
    }
    const QPointF n = to_normalized(e->position(), img);
    points_.push_back(camera::Point{static_cast<float>(n.x()),
                                    static_cast<float>(n.y())});
    emit changed();
    update();
}

void RoiCanvas::mouseMoveEvent(QMouseEvent* e) {
    if (frame_.isNull()) {
        return;
    }
    const QRectF img = image_rect();

    if (drag_vertex_ >= 0 && drag_vertex_ < static_cast<int>(points_.size())) {
        // Dragging clamps (unlike a click): the operator is holding the handle
        // and pulling past the border, which reads as "put it on the edge".
        const QPointF n = to_normalized(e->position(), img);
        points_[static_cast<size_t>(drag_vertex_)] = {
            static_cast<float>(n.x()), static_cast<float>(n.y())};
        emit changed();
        update();
        return;
    }

    const int was = hover_vertex_;
    hover_vertex_ = (mode_ == Mode::Editing)
                        ? hit_test_vertex(widget_points(), e->position(),
                                          kGrabRadiusPx)
                        : -1;
    if (hover_vertex_ != was) {
        setCursor(hover_vertex_ >= 0 ? Qt::SizeAllCursor : Qt::CrossCursor);
        update();
    }
}

void RoiCanvas::mouseReleaseEvent(QMouseEvent*) {
    if (drag_vertex_ >= 0) {
        drag_vertex_ = -1;
        emit polygon_edited(points_);  // commit the move to the page's working set
    }
}

void RoiCanvas::mouseDoubleClickEvent(QMouseEvent* e) {
    if (e->button() == Qt::LeftButton) {
        try_close();  // a double-click is a convenient "done" gesture
    }
}

void RoiCanvas::keyPressEvent(QKeyEvent* e) {
    switch (e->key()) {
        case Qt::Key_Backspace:
            undo_point();
            break;
        case Qt::Key_Escape:
            if (mode_ == Mode::Drawing) {
                clear();
            }
            break;
        case Qt::Key_Return:
        case Qt::Key_Enter:
            try_close();
            break;
        default:
            QWidget::keyPressEvent(e);
    }
}

void RoiCanvas::draw_context(QPainter& p, const QRectF& img) const {
    for (const camera::CameraArea& a : context_) {
        if (a.points.size() < kMinVertices) {
            continue;
        }
        QPolygonF poly;
        for (const camera::Point& pt : a.points) {
            poly << to_widget(QPointF(pt.x, pt.y), img);
        }
        QPainterPath path;
        path.addPolygon(poly);
        path.closeSubpath();
        p.fillPath(path, kContextFill);
        p.setPen(QPen(kContextEdge, 1.5, Qt::DashLine));
        p.setBrush(Qt::NoBrush);
        p.drawPolygon(poly);

        // Label each area at its centroid so the operator can tell which
        // physical display is which zone without clicking through the list.
        const QString label =
            a.zone ? QStringLiteral("%1 · Zone %2")
                         .arg(QString::fromStdString(a.name))
                         .arg(*a.zone)
                   : QString::fromStdString(a.name);
        const QPointF c = poly.boundingRect().center();
        const QRectF box =
            QRectF(p.fontMetrics().boundingRect(label))
                .adjusted(-5, -3, 5, 3)
                .translated(
                    c - QPointF(p.fontMetrics().horizontalAdvance(label) / 2.0,
                                0));
        p.setPen(Qt::NoPen);
        p.setBrush(kLabelBg);
        p.drawRoundedRect(box, 4, 4);
        p.setPen(QColor(226, 232, 240));
        p.drawText(box, Qt::AlignCenter, label);
    }
}

void RoiCanvas::draw_active(QPainter& p, const QRectF& img) const {
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

    p.setPen(QPen(kEdge, 2.0));
    p.setBrush(Qt::NoBrush);
    if (closed_) {
        p.drawPolygon(poly);
    } else {
        p.drawPolyline(poly);
    }

    for (int i = 0; i < poly.size(); ++i) {
        // While drawing, the first vertex is the close target and is drawn as a
        // ring — shape, not just colour, so it reads over any camera image.
        const bool close_target =
            (i == 0 && mode_ == Mode::Drawing && !closed_ &&
             poly.size() >= kMinVertices);
        const double r = (i == hover_vertex_ || i == drag_vertex_)
                             ? kVertexHoverRadiusPx
                             : kVertexRadiusPx;
        p.setBrush(close_target ? QBrush(Qt::NoBrush) : QBrush(kVertex));
        p.setPen(QPen(close_target ? kEdge : QColor(17, 17, 17),
                      close_target ? 3.0 : 1.0));
        p.drawEllipse(poly[i], close_target ? kCloseRadiusPx * 0.6 : r,
                      close_target ? kCloseRadiusPx * 0.6 : r);
    }
}

void RoiCanvas::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.fillRect(rect(), QColor(17, 17, 17));

    const QRectF img = image_rect();
    if (frame_.isNull()) {
        p.setPen(QColor(148, 148, 148));
        p.drawText(rect(), Qt::AlignCenter,
                   QStringLiteral("No preview — press “Refresh preview”."));
        return;
    }
    p.drawImage(img, frame_);

    draw_context(p, img);
    draw_active(p, img);
}

} // namespace denso::ui
