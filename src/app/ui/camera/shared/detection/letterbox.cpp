#include "ui/camera/shared/detection/letterbox.h"

#include <opencv2/imgproc.hpp>

#include <algorithm>

namespace denso::ui {

LetterboxInfo letterbox(const cv::Mat& src, cv::Mat& dst, int size,
                        int pad_value) {
    LetterboxInfo lb;
    lb.size = size;
    const float scale = std::min(static_cast<float>(size) / src.cols,
                                 static_cast<float>(size) / src.rows);
    lb.scale = scale;
    const int new_w = static_cast<int>(std::round(src.cols * scale));
    const int new_h = static_cast<int>(std::round(src.rows * scale));
    lb.pad_x = (size - new_w) / 2;
    lb.pad_y = (size - new_h) / 2;

    cv::Mat resized;
    cv::resize(src, resized, cv::Size(new_w, new_h), 0, 0, cv::INTER_LINEAR);
    dst = cv::Mat(size, size, src.type(), cv::Scalar::all(pad_value));
    resized.copyTo(dst(cv::Rect(lb.pad_x, lb.pad_y, new_w, new_h)));
    return lb;
}

cv::Rect undo_letterbox(float cx, float cy, float w, float h,
                        const LetterboxInfo& lb, int orig_w, int orig_h) {
    // De-pad, then de-scale back to original pixels.
    const float x1 = (cx - w / 2.0f - lb.pad_x) / lb.scale;
    const float y1 = (cy - h / 2.0f - lb.pad_y) / lb.scale;
    const float x2 = (cx + w / 2.0f - lb.pad_x) / lb.scale;
    const float y2 = (cy + h / 2.0f - lb.pad_y) / lb.scale;
    int ix1 = std::clamp(static_cast<int>(std::round(x1)), 0, orig_w);
    int iy1 = std::clamp(static_cast<int>(std::round(y1)), 0, orig_h);
    int ix2 = std::clamp(static_cast<int>(std::round(x2)), 0, orig_w);
    int iy2 = std::clamp(static_cast<int>(std::round(y2)), 0, orig_h);
    return cv::Rect(ix1, iy1, std::max(0, ix2 - ix1), std::max(0, iy2 - iy1));
}

} // namespace denso::ui
