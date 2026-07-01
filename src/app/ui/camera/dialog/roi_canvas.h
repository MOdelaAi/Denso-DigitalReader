// A draw-only canvas for one ROI polygon over a camera snapshot. Shows the
// (already oriented) frame as background and lets the user click out a polygon
// of 3+ vertices: click to drop a vertex, click near the first vertex (or
// double-click / Enter) to close it, Backspace to undo the last vertex, Esc to
// clear. Points are kept normalized (0..1) so they're resolution-independent;
// the widget↔normalized mapping lives in roi_geometry (unit-tested). The canvas
// owns no data policy — the dialog loads/persists areas around it.
#pragma once

#include "camera/model.h"

#include <QImage>
#include <QWidget>

#include <vector>

namespace denso::ui {

class RoiCanvas : public QWidget {
    Q_OBJECT

public:
    explicit RoiCanvas(QWidget* parent = nullptr);

    void set_frame(const QImage& oriented);  // background; repaints
    void set_polygon(const std::vector<camera::Point>& pts);  // show a closed area
    void clear();                            // wipe points, ready to draw a new one

    const std::vector<camera::Point>& polygon() const { return points_; }
    bool is_closed() const { return closed_; }
    bool is_valid() const { return points_.size() >= 3; }  // a polygon needs 3+

signals:
    void changed();  // any edit: vertex added/removed/cleared
    void closed();   // the polygon was just closed (3+ vertices)

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;
    void keyPressEvent(QKeyEvent*) override;

private:
    QRectF image_rect() const;  // where the frame is drawn inside the widget
    void try_close();

    QImage frame_;
    std::vector<camera::Point> points_;  // normalized 0..1
    bool closed_ = false;
};

} // namespace denso::ui
