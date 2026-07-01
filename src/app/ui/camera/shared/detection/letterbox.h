// Letterbox a frame into the square input a YOLO ONNX model expects: resize
// preserving aspect ratio, then pad the short side with gray to size×size. The
// returned LetterboxInfo carries the scale + padding needed to map detection
// boxes back to original-image pixels. Pure (OpenCV only) — unit-tested.
#pragma once

#include <opencv2/core.hpp>

namespace denso::ui {

struct LetterboxInfo {
    float scale = 1.0f;  // original → letterboxed resize factor
    int pad_x = 0;       // left padding in the letterboxed image
    int pad_y = 0;       // top padding
    int size = 640;      // output square size
};

LetterboxInfo letterbox(const cv::Mat& src, cv::Mat& dst, int size = 640,
                        int pad_value = 114);

cv::Rect undo_letterbox(float cx, float cy, float w, float h,
                        const LetterboxInfo& lb, int orig_w, int orig_h);

} // namespace denso::ui
