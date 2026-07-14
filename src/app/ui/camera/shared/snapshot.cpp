#include "ui/camera/shared/snapshot.h"

#include "ui/camera/shared/frame_convert.h"
#include "ui/camera/shared/gst_pipeline.h"  // rtsp_gst_pipeline (NVDEC)

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
    [[maybe_unused]] const double dist = std::max(w, h);

    QTransform t;
    t.translate(w / 2.0, h / 2.0);              // pivot about the image centre
    t.rotate(roll + degrees, Qt::ZAxis);        // in-plane: preset + roll
    // Out-of-plane tilt ⇒ perspective. The frame-proportional viewer distance
    // overload landed in Qt 6.5; on older Qt (e.g. Jetson's 6.2) fall back to the
    // 2-arg form (fixed built-in distance) — the tilt still renders.
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    t.rotate(pitch, Qt::XAxis, dist);
#else
    t.rotate(pitch, Qt::XAxis);
#endif
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
        // Match the live stream's capture path (which CAP_ANY does not on the
        // Jetson): hardware NVDEC via GStreamer, H.264 then H.265, then a plain
        // CAP_ANY fallback for non-Jetson hosts. `url` already carries creds.
        const std::string u = url.toStdString();
        cap.open(rtsp_gst_pipeline(u, Codec::H264), cv::CAP_GSTREAMER);
        if (!cap.isOpened()) {
            cap.open(rtsp_gst_pipeline(u, Codec::H265), cv::CAP_GSTREAMER);
        }
        if (!cap.isOpened()) {
            cap.open(u, cv::CAP_ANY, params);
        }
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
