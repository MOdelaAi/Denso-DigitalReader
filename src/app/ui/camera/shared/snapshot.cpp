#include "ui/camera/shared/snapshot.h"

#include "ui/camera/shared/frame_convert.h"

#include <QTransform>

#include <opencv2/imgproc.hpp>
#include <opencv2/videoio.hpp>

#include <algorithm>
#include <vector>

namespace denso::ui {

QImage mat_to_qimage(const cv::Mat& bgr) {
    if (bgr.empty()) {
        return {};
    }
    cv::Mat rgb;
    cv::cvtColor(bgr, rgb, cv::COLOR_BGR2RGB);
    const QImage view(rgb.data, rgb.cols, rgb.rows, static_cast<int>(rgb.step),
                      QImage::Format_RGB888);
    return view.copy();  // deep copy: rgb is local, the QImage must own its bytes
}

QImage apply_rotation(const QImage& src, int degrees) {
    if (degrees % 360 == 0) {
        return src;
    }
    QTransform t;
    t.rotate(degrees);
    return src.transformed(t);
}

QImage apply_orientation(const QImage& src, int degrees, double pitch,
                         double roll) {
    if (degrees % 360 == 0 && pitch == 0.0 && roll == 0.0) {
        return src;
    }
    const double w = src.width();
    const double h = src.height();
    // Viewer distance scaled to the frame so the tilt reads naturally at any
    // resolution (a larger frame is treated as proportionally farther away).
    const double dist = std::max(w, h);

    QTransform t;
    t.translate(w / 2.0, h / 2.0);              // pivot about the image centre
    t.rotate(roll + degrees, Qt::ZAxis);        // in-plane: preset + roll
    t.rotate(pitch, Qt::XAxis, dist);           // out-of-plane tilt ⇒ perspective
    t.translate(-w / 2.0, -h / 2.0);
    return src.transformed(t, Qt::SmoothTransformation);
}

Snapshot grab_snapshot(std::optional<int> index, const QString& url,
                       int width, int height) {
    cv::VideoCapture cap;
    // Fail fast instead of hanging on an unreachable RTSP host.
    const std::vector<int> params = {
        cv::CAP_PROP_OPEN_TIMEOUT_MSEC, 5000,
        cv::CAP_PROP_READ_TIMEOUT_MSEC, 5000,
    };
    if (index.has_value()) {
        cap.open(*index, cv::CAP_ANY, params);
    } else {
        cap.open(url.toStdString(), cv::CAP_ANY, params);
    }
    if (!cap.isOpened()) {
        return {QImage(), QStringLiteral("Could not open the camera.")};
    }
    if (width > 0 && height > 0) {
        cap.set(cv::CAP_PROP_FRAME_WIDTH, width);
        cap.set(cv::CAP_PROP_FRAME_HEIGHT, height);
    }
    cv::Mat frame;
    if (!cap.read(frame) || frame.empty()) {
        return {QImage(), QStringLiteral("No frame received from the camera.")};
    }
    return {mat_to_qimage(frame), QString()};
}

} // namespace denso::ui
