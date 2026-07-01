#include "ui/camera/shared/frame_convert.h"

#include <opencv2/imgproc.hpp>

namespace denso::ui {

cv::Mat qimage_to_mat(const QImage& img) {
    if (img.isNull()) {
        return cv::Mat();
    }
    const QImage rgb = img.convertToFormat(QImage::Format_RGB888);
    cv::Mat view(rgb.height(), rgb.width(), CV_8UC3,
                 const_cast<uchar*>(rgb.bits()),
                 static_cast<size_t>(rgb.bytesPerLine()));
    cv::Mat bgr;
    cv::cvtColor(view, bgr, cv::COLOR_RGB2BGR);  // deep-copies into bgr
    return bgr;
}

} // namespace denso::ui
