#include "ui/camera/dialog/level_canvas.h"

#include "ui/camera/shared/roi_geometry.h"

#include <QMouseEvent>
#include <QPainter>
#include <QPen>
#include <QRectF>
#include <QSizeF>
#include <QString>

#include <algorithm>
#include <cmath>

namespace denso::ui {
namespace {

/// How close a press must land to a reference line to grab it. Generous, because
/// the target is a 1-px line and the panel may be a touchscreen.
constexpr double kGrabPx = 10.0;
/// Below this the rubber band is a stray click or a tap, not a rectangle.
constexpr double kMinBandPx = 2.0;
/// A press must land on the drawn frame, within this slack, to count.
constexpr double kOnFramePx = 1.0;

// The ghost palette, taken VERBATIM from roi_canvas.cpp rather than re-picked.
// The digit Areas page already taught the operator what "recessive slate dashes"
// mean; inventing a second ghost style here would make one appliance speak two
// visual languages for one idea.
const QColor kContextEdge(148, 163, 184, 170);  // slate — present but recessive
const QColor kContextFill(148, 163, 184, 30);
const QColor kLabelBg(17, 17, 17, 190);
// The selected zone's reference lines, dimmed to the same recessive weight. The
// HUES are kept (green = 100%, red = 0%) because they are how the operator reads
// which line is which; only the emphasis drops.
const QColor kContext100(102, 217, 160, 110);
const QColor kContext0(232, 122, 122, 110);

}  // namespace

LevelCanvas::LevelCanvas(QWidget* parent) : QWidget(parent) {
    setMinimumSize(320, 240);
    setMouseTracking(true);
}

void LevelCanvas::set_frame(const QImage& oriented) {
    frame_ = oriented;
    update();
}

void LevelCanvas::set_calibration(const denso::level::LevelCalibration& c,
                                  bool has_rect) {
    c_ = c;
    has_rect_ = has_rect;
    update();
}

void LevelCanvas::set_context_zones(
    const std::vector<denso::level::LevelZone>& others) {
    context_ = others;
    update();
}

void LevelCanvas::begin_draw() {
    mode_ = Mode::Drawing;
    banding_ = false;
    grabbed_ = 0;
    update();
}

void LevelCanvas::begin_edit() {
    mode_ = Mode::Editing;
    banding_ = false;
    grabbed_ = 0;
    update();
}

QRectF LevelCanvas::image_rect() const {
    if (frame_.isNull()) {
        return QRectF();
    }
    return fitted_image_rect(QSizeF(frame_.size()), QSizeF(size()));
}

int LevelCanvas::line_at(const QPointF& widget_pt) const {
    const QRectF img = image_rect();
    if (!has_rect_ || img.isEmpty()) {
        return 0;
    }
    const double y100 = to_widget(QPointF(0.0, c_.y_100), img).y();
    const double y0 = to_widget(QPointF(0.0, c_.y_0), img).y();
    const double d100 = std::abs(widget_pt.y() - y100);
    const double d0 = std::abs(widget_pt.y() - y0);
    if (d100 > kGrabPx && d0 > kGrabPx) {
        return 0;
    }
    // Ties go to the 100% line — an arbitrary but FIXED choice, so a press exactly
    // between two coincident lines always grabs the same one instead of depending
    // on floating-point noise.
    return d100 <= d0 ? 1 : 2;
}

void LevelCanvas::mousePressEvent(QMouseEvent* e) {
    if (e->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(e);
        return;
    }
    const QRectF img = image_rect();
    if (img.isEmpty()) {
        emit rejected(QStringLiteral("Capture a preview frame first."));
        return;
    }
    // Rejected, never clamped: a press in the letter-/pillar-box bars that got
    // clamped would silently pin an edge or a line to the image boundary — a
    // calibration the operator never drew.
    if (!image_contains(img, e->position(), kOnFramePx)) {
        emit rejected(
            QStringLiteral("Click inside the camera image, not the border."));
        return;
    }

    if (mode_ == Mode::Drawing) {
        banding_ = true;
        band_from_ = e->position();
        band_to_ = e->position();
        update();
        return;
    }
    grabbed_ = line_at(e->position());
    if (grabbed_ != 0) {
        // Apply immediately so a tap ON the line (press+release, no move) still
        // places it where the operator tapped.
        const double y = to_normalized(e->position(), img).y();
        if (grabbed_ == 1) emit y_100_moved(y);
        else emit y_0_moved(y);
    }
}

void LevelCanvas::mouseMoveEvent(QMouseEvent* e) {
    const QRectF img = image_rect();
    if (img.isEmpty()) {
        return;
    }
    if (banding_) {
        // Clamped, not rejected: the operator is holding the handle, and a drag
        // that runs off the frame should stop at the edge rather than abort.
        band_to_ = e->position();
        update();
        return;
    }
    if (grabbed_ == 0) {
        return;
    }
    const double y = to_normalized(e->position(), img).y();
    if (grabbed_ == 1) emit y_100_moved(y);
    else emit y_0_moved(y);
}

void LevelCanvas::mouseReleaseEvent(QMouseEvent* e) {
    if (e->button() != Qt::LeftButton) {
        QWidget::mouseReleaseEvent(e);
        return;
    }
    grabbed_ = 0;
    if (!banding_) {
        return;
    }
    banding_ = false;
    band_to_ = e->position();
    const QRectF img = image_rect();
    if (img.isEmpty()) {
        update();
        return;
    }
    // A rectangle needs area. A stray click would otherwise produce a zero-sized
    // rectangle that the draft refuses, leaving the operator with a dead canvas
    // and no explanation.
    if (std::abs(band_to_.x() - band_from_.x()) < kMinBandPx ||
        std::abs(band_to_.y() - band_from_.y()) < kMinBandPx) {
        update();
        emit rejected(QStringLiteral(
            "Drag out a rectangle around the sight glass — a single click is "
            "not a measurement area."));
        return;
    }
    const QPointF a = to_normalized(band_from_, img);
    const QPointF b = to_normalized(band_to_, img);
    const double x = std::min(a.x(), b.x());
    const double y = std::min(a.y(), b.y());
    emit rect_drawn(x, y, std::abs(b.x() - a.x()), std::abs(b.y() - a.y()));
    update();
}

void LevelCanvas::draw_rubber_band(QPainter& p, const QRectF& img) const {
    QPen pen(QColor(255, 209, 102), 1.0, Qt::DashLine);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    p.drawRect(QRectF(band_from_, band_to_).normalized().intersected(img));
}

void LevelCanvas::draw_calibration(QPainter& p, const QRectF& img) const {
    const QPointF tl = to_widget(QPointF(c_.rect_x, c_.rect_y), img);
    const QPointF br =
        to_widget(QPointF(c_.rect_x + c_.rect_w, c_.rect_y + c_.rect_h), img);
    const QRectF rect(tl, br);

    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(QColor(255, 209, 102), 2.0));
    p.drawRect(rect);

    // The reference lines are BALL-CENTRE positions, not liquid-surface lines.
    // Labelling them is a correctness requirement, not decoration: with a centre
    // reference point, lines placed at the visible surface introduce a constant
    // radius-sized offset into every reading this camera ever reports.
    struct Line {
        double y;
        QColor colour;
        QString label;
    };
    const Line lines[] = {
        {c_.y_100, QColor(102, 217, 160),
         QStringLiteral("100% — ball centre here when full")},
        {c_.y_0, QColor(232, 122, 122),
         QStringLiteral("0% — ball centre here when empty")},
    };
    for (const Line& l : lines) {
        const double y = to_widget(QPointF(0.0, l.y), img).y();
        p.setPen(QPen(l.colour, 2.0));
        p.drawLine(QPointF(rect.left(), y), QPointF(rect.right(), y));
        // Grab handle, so the draggable target is visible rather than folklore.
        p.setBrush(l.colour);
        p.drawEllipse(QPointF(rect.right(), y), 5.0, 5.0);
        p.setBrush(Qt::NoBrush);
        p.drawText(QPointF(rect.left() + 6.0, y - 6.0), l.label);
    }
}

void LevelCanvas::draw_context(QPainter& p, const QRectF& img) const {
    for (const denso::level::LevelZone& z : context_) {
        const denso::level::LevelCalibration& c = z.calibration;
        // A zone with no measurable rectangle has nothing to place on the frame.
        // Drawing a zero-area box would read as a stray mark, not as a zone.
        if (c.rect_w <= 0.0 || c.rect_h <= 0.0) {
            continue;
        }
        const QRectF r(to_widget(QPointF(c.rect_x, c.rect_y), img),
                       to_widget(QPointF(c.rect_x + c.rect_w, c.rect_y + c.rect_h),
                                 img));

        // Fill first, then a dashed outline: filled-and-dashed is what the digit
        // page's context areas look like, so overlap reads the same way here.
        p.setPen(Qt::NoPen);
        p.setBrush(kContextFill);
        p.drawRect(r);
        p.setPen(QPen(kContextEdge, 1.5, Qt::DashLine));
        p.setBrush(Qt::NoBrush);
        p.drawRect(r);

        // Enough calibration geometry to understand WHERE the zone measures,
        // without the grab handles or the long labels the selected zone gets —
        // a handle on something you cannot drag is a lie about affordance.
        const double y100 = to_widget(QPointF(0.0, c.y_100), img).y();
        const double y0 = to_widget(QPointF(0.0, c.y_0), img).y();
        p.setPen(QPen(kContext100, 1.0, Qt::DashLine));
        p.drawLine(QPointF(r.left(), y100), QPointF(r.right(), y100));
        p.setPen(QPen(kContext0, 1.0, Qt::DashLine));
        p.drawLine(QPointF(r.left(), y0), QPointF(r.right(), y0));

        // A small number, so the operator can name what they are looking at.
        const QString label = QStringLiteral("Zone %1").arg(z.zone_no);
        const QRectF box =
            QRectF(p.fontMetrics().boundingRect(label))
                .adjusted(-5, -3, 5, 3)
                .translated(r.center() -
                            QPointF(p.fontMetrics().horizontalAdvance(label) / 2.0,
                                    0));
        p.setPen(Qt::NoPen);
        p.setBrush(kLabelBg);
        p.drawRoundedRect(box, 4, 4);
        p.setPen(QColor(226, 232, 240));
        p.drawText(box, Qt::AlignCenter, label);
    }
}

void LevelCanvas::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    const QRectF img = image_rect();
    if (frame_.isNull() || img.isEmpty()) {
        p.setPen(QColor(150, 150, 150));
        p.drawText(rect(), Qt::AlignCenter,
                   QStringLiteral("No preview frame yet."));
        return;
    }
    p.drawImage(img, frame_);
    // Context FIRST, so the selected zone is painted over the ghosts and always
    // wins the overlap. Order is the whole of "must not visually compete".
    draw_context(p, img);
    if (has_rect_) {
        draw_calibration(p, img);
    }
    if (banding_) {
        draw_rubber_band(p, img);
    }
}

} // namespace denso::ui
