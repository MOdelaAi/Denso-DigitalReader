// One cell of the live grid: paints the latest frame for a camera (aspect-fit),
// with the camera name and a status dot overlaid. When connecting or offline it
// shows a placeholder instead of a frame. Purely a view — frames and status
// arrive via slots wired to a CameraStream; it owns no capture logic.
#pragma once

#include "camera/camera.h"
#include "ui/camera/grid/fps_meter.h"

#include <QImage>
#include <QString>
#include <QWidget>

#include <vector>

class QPainter;
class QRectF;

namespace denso::ui {

class CameraTile : public QWidget {
    Q_OBJECT

public:
    explicit CameraTile(const QString& name, QWidget* parent = nullptr);

    /// The camera's saved ROI polygons (normalized to the oriented frame),
    /// drawn as outlines over the live feed. Empty = no overlay.
    void set_areas(std::vector<camera::CameraArea> areas);

public slots:
    void set_frame(const QImage& frame);
    void set_status(int status);  // CameraStream::Status as int

protected:
    void paintEvent(QPaintEvent*) override;

private:
    void draw_areas(QPainter& p, const QRectF& image_rect) const;

    QString name_;
    QImage frame_;
    int status_ = 0;  // Connecting
    std::vector<camera::CameraArea> areas_;
    FpsMeter meter_;  // real live fps from frame arrivals
};

} // namespace denso::ui
