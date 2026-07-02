#include "ui/camera/grid/camera_tile.h"

#include "ui/camera/grid/camera_stream.h"  // Status enum
#include "ui/camera/shared/roi_geometry.h"   // to_widget

#include <QColor>
#include <QFont>
#include <QPainter>
#include <QPen>
#include <QPointF>
#include <QPolygonF>
#include <QRectF>

#include <utility>

namespace denso::ui {

namespace {
const QColor kBg(20, 20, 20);
const QColor kName(229, 231, 235);
const QColor kFaint(148, 148, 148);
const QColor kRoi(250, 204, 21);  // gold — matches the ROI drawing canvas

struct StatusLook {
    QColor dot;
    QString text;
};

StatusLook look_for(int status) {
    switch (static_cast<CameraStream::Status>(status)) {
        case CameraStream::Status::Live:
            return {QColor(34, 197, 94), QStringLiteral("Live")};      // green
        case CameraStream::Status::Offline:
            return {QColor(239, 68, 68), QStringLiteral("Offline")};   // red
        case CameraStream::Status::Connecting:
        default:
            return {QColor(250, 204, 21), QStringLiteral("Connecting…")};  // gold
    }
}
}

CameraTile::CameraTile(const QString& name, QWidget* parent)
    : QWidget(parent), name_(name) {
    setMinimumSize(240, 160);
}

void CameraTile::set_frame(const QImage& frame) {
    frame_ = frame;
    meter_.tick(FpsMeter::clock::now());  // one displayed frame → update the rate
    update();
}

void CameraTile::set_status(int status) {
    status_ = status;
    if (static_cast<CameraStream::Status>(status) != CameraStream::Status::Live) {
        meter_.reset();  // don't carry a stale rate across an offline gap
    }
    update();
}

void CameraTile::set_areas(std::vector<camera::CameraArea> areas) {
    areas_ = std::move(areas);
    update();
}

void CameraTile::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.fillRect(rect(), kBg);

    if (!frame_.isNull()) {
        // Stretch-to-fill — the CCTV/NVR full-bleed look: the frame is scaled to
        // the whole tile with no bands and nothing cropped (aspect may distort
        // slightly). Same rect the ROI overlay maps onto so the polygons track
        // the displayed image.
        const QRectF img(rect());
        p.drawImage(img, frame_);
        draw_areas(p, img);
    } else {
        p.setPen(kFaint);
        QFont gf = p.font();
        gf.setPointSize(28);
        p.setFont(gf);
        p.drawText(rect(), Qt::AlignCenter, QStringLiteral("📷"));
    }

    // Overlays: name (top-left) + status dot & label (top-right).
    const StatusLook look = look_for(status_);
    p.setPen(kName);
    p.drawText(rect().adjusted(10, 8, -10, 0), Qt::AlignTop | Qt::AlignLeft, name_);

    const QString status_text = look.text;
    const QRectF tr = rect().adjusted(0, 8, -10, 0);
    p.setPen(kFaint);
    p.drawText(tr, Qt::AlignTop | Qt::AlignRight, status_text);
    // Dot just left of the status text.
    const qreal text_w = p.fontMetrics().horizontalAdvance(status_text);
    p.setBrush(look.dot);
    p.setPen(Qt::NoPen);
    p.drawEllipse(QPointF(width() - 10 - text_w - 8, 8 + p.fontMetrics().ascent() / 2.0),
                  4, 4);

    // Live fps, a line below the status (only while actually streaming).
    if (static_cast<CameraStream::Status>(status_) == CameraStream::Status::Live &&
        meter_.fps() > 0.0) {
        const QString fps_text = QStringLiteral("%1 fps").arg(meter_.fps(), 0, 'f', 1);
        p.setPen(kFaint);
        p.drawText(rect().adjusted(0, 8 + p.fontMetrics().lineSpacing(), -10, 0),
                   Qt::AlignTop | Qt::AlignRight, fps_text);
    }
}

void CameraTile::draw_areas(QPainter& p, const QRectF& image_rect) const {
    if (areas_.empty()) {
        return;  // no ROIs → raw frame, nothing overlaid
    }
    p.save();
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(kRoi, 2.0));
    for (const camera::CameraArea& area : areas_) {
        if (area.points.size() < 3) {
            continue;  // not a polygon
        }
        QPolygonF poly;
        for (const camera::Point& pt : area.points) {
            poly << to_widget(QPointF(pt.x, pt.y), image_rect);
        }
        p.drawPolygon(poly);  // closed outline
        if (!area.name.empty()) {
            p.drawText(poly.first() + QPointF(4, -4),
                       QString::fromStdString(area.name));
        }
    }
    p.restore();
}

} // namespace denso::ui
