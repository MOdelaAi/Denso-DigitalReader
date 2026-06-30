// Convert an OpenCV BGR frame to a QImage. Isolated in its own header so the
// cv::Mat type stays out of snapshot.h (and thus out of the dialog's include
// graph); only snapshot.cpp and the test include this.
#pragma once

#include <QImage>

#include <opencv2/core.hpp>

namespace denso::ui {

/// BGR `cv::Mat` (CV_8UC3) → RGB888 QImage that owns its bytes. Empty in → null.
QImage mat_to_qimage(const cv::Mat& bgr);

} // namespace denso::ui
