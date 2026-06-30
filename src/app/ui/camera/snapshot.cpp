#include "ui/camera/snapshot.h"

#include "ui/camera/frame_convert.h"

#include <QTransform>

#include <opencv2/imgproc.hpp>
#include <opencv2/videoio.hpp>

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

Snapshot grab_snapshot(std::optional<int> index, const QString& url,
                       int width, int height) {
    return {QImage(), QStringLiteral("not implemented")};  // Task 2 implements
}

} // namespace denso::ui
